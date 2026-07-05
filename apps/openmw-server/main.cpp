#include "Server.hpp"

#include <algorithm>
#include <map>
#ifdef _WIN32
#  include <windows.h>
#endif
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static mwmp::MPServer* gServer = nullptr;

// ---------------------------------------------------------------------------
// Logging helpers
// ---------------------------------------------------------------------------
namespace
{
    static std::ofstream gLogFile;
    static std::streambuf* gOldCout = nullptr;
    static std::streambuf* gOldCerr = nullptr;

    // Stream buffer that writes to both an ofstream and the original console buf
    class TeeBuffer : public std::streambuf
    {
    public:
        TeeBuffer(std::streambuf* a, std::streambuf* b) : mA(a), mB(b) {}
    protected:
        int overflow(int c) override
        {
            if (c == EOF) return EOF;
            if (mA->sputc(static_cast<char>(c)) == EOF) return EOF;
            if (mB->sputc(static_cast<char>(c)) == EOF) return EOF;
            return c;
        }
        std::streamsize xsputn(const char* s, std::streamsize n) override
        {
            mA->sputn(s, n);
            mB->sputn(s, n);
            return n;
        }
    private:
        std::streambuf* mA;
        std::streambuf* mB;
    };
    static std::unique_ptr<TeeBuffer> gTeeCout;
    static std::unique_ptr<TeeBuffer> gTeeCerr;

    void rotateLog(const std::filesystem::path& logPath, int maxKeep = 10)
    {
        namespace fs = std::filesystem;
        if (!fs::exists(logPath)) return;

        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::ostringstream ts;
        ts << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");

        const std::string stem  = logPath.stem().string();
        const fs::path    dest  = logPath.parent_path() / (stem + "_" + ts.str() + ".log");
        std::error_code ec;
        fs::rename(logPath, dest, ec);

        // Prune oldest rotated logs
        std::vector<fs::path> old;
        for (const auto& entry : fs::directory_iterator(logPath.parent_path(), ec))
        {
            const std::string fname = entry.path().filename().string();
            if (fname.size() > stem.size() + 1
                && fname.substr(0, stem.size() + 1) == stem + "_"
                && fname.size() > 4
                && fname.substr(fname.size() - 4) == ".log")
                old.push_back(entry.path());
        }
        if (static_cast<int>(old.size()) > maxKeep)
        {
            std::sort(old.begin(), old.end());
            for (int i = 0; i < static_cast<int>(old.size()) - maxKeep; ++i)
                fs::remove(old[i], ec);
        }
    }

    void setupServerLogging(const std::filesystem::path& logDir)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(logDir, ec);

        const fs::path logPath = logDir / "openmw-server.log";
        rotateLog(logPath);

        gLogFile.open(logPath, std::ios::out);
        if (!gLogFile.is_open())
        {
            std::cerr << "[Server] Warning: could not open log file " << logPath << "\n";
            return;
        }

        // Tee cout and cerr to both console and file
        gOldCout = std::cout.rdbuf();
        gOldCerr = std::cerr.rdbuf();
        gTeeCout = std::make_unique<TeeBuffer>(gOldCout, gLogFile.rdbuf());
        gTeeCerr = std::make_unique<TeeBuffer>(gOldCerr, gLogFile.rdbuf());
        std::cout.rdbuf(gTeeCout.get());
        std::cerr.rdbuf(gTeeCerr.get());

        std::cout << "[Server] Logging to " << logPath.string() << "\n";
    }

    void teardownServerLogging()
    {
        if (gOldCout) std::cout.rdbuf(gOldCout);
        if (gOldCerr) std::cerr.rdbuf(gOldCerr);
        gTeeCout.reset();
        gTeeCerr.reset();
        gLogFile.close();
    }
}

static void signalHandler(int /*sig*/)
{
    if (gServer)
        gServer->requestStop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Minimal INI parser: sections [name], key = value, # and ; comments.
// Returns a map of "section.key" → "value".
// ─────────────────────────────────────────────────────────────────────────────
static std::map<std::string, std::string> parseIni(const std::filesystem::path& path)
{
    std::map<std::string, std::string> cfg;
    std::ifstream f(path);
    if (!f.is_open()) return cfg;
    std::string section, line;
    while (std::getline(f, line))
    {
        // Trim
        auto ltrim = [](std::string& s){ s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int c){return !std::isspace(c);})); };
        auto rtrim = [](std::string& s){ s.erase(std::find_if(s.rbegin(), s.rend(), [](int c){return !std::isspace(c);}).base(), s.end()); };
        ltrim(line); rtrim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = line.substr(1, line.size() - 2);
            ltrim(section); rtrim(section);
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        ltrim(key); rtrim(key); ltrim(value); rtrim(value);
        // Strip inline comments
        for (char c : {'#', ';'})
        {
            const auto pos = value.find(c);
            if (pos != std::string::npos) { value = value.substr(0, pos); rtrim(value); }
        }
        cfg[(section.empty() ? "" : section + ".") + key] = value;
    }
    return cfg;
}

static bool cfgBool(const std::map<std::string,std::string>& cfg, const std::string& key, bool def)
{
    auto it = cfg.find(key);
    if (it == cfg.end()) return def;
    const std::string& v = it->second;
    return (v == "true" || v == "1" || v == "yes" || v == "on");
}

static int cfgInt(const std::map<std::string,std::string>& cfg, const std::string& key, int def)
{
    auto it = cfg.find(key);
    return (it == cfg.end()) ? def : std::atoi(it->second.c_str());
}

static std::string cfgStr(const std::map<std::string,std::string>& cfg,
                           const std::string& key, const std::string& def = "")
{
    auto it = cfg.find(key);
    return (it == cfg.end()) ? def : it->second;
}

// Write a default server.cfg if none exists
static void writeDefaultConfig(const std::filesystem::path& cfgPath)
{
    std::ofstream f(cfgPath);
    if (!f.is_open()) return;
    f << "# OpenMW Multiplayer - Server Configuration\n"
         "# Lines beginning with # or ; are comments.\n"
         "\n"
         "[server]\n"
         "port            = 25565\n"
         "max_players     = 32\n"
         "db_path         = playerdata.db\n"
         "actorAuthorityExteriorRadius = 1\n"
         "actorAuthorityStickyMs = 3000\n"
         "actorAuthorityPreferExactCell = true\n"
         "\n"
         "[master]\n"
         "public_listing  = false       # set true to appear in the server browser\n"
         "master_url      = https://master.openmw-mp.org\n"
         "server_name     = My OpenMW Server\n"
         "game_mode       = Co-op\n"
         "\n";
    std::cout << "[Server] Created default config: " << cfgPath.string() << "\n";
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    const std::filesystem::path exeDir = std::filesystem::path(argv[0]).parent_path();
    std::filesystem::path cfgPath  = exeDir / "server.cfg";
    std::filesystem::path logDir   = exeDir / "logs";

    // Quick pre-scan for --config / --help before loading cfg
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc)
            cfgPath = argv[++i];
        else if (arg == "--help" || arg == "-h")
        {
            std::cout << "openmw-server [options]\n"
                      << "  --config, -c <path>   Path to server.cfg (default: <exe dir>/server.cfg)\n"
                      << "  --port,   -p <port>   UDP port override (default: from config or 25565)\n"
                      << "  --log-dir,-l <dir>    Log directory override\n";
            return 0;
        }
    }

    // Create default config if missing
    if (!std::filesystem::exists(cfgPath))
        writeDefaultConfig(cfgPath);

    // Load config file
    auto cfg = parseIni(cfgPath);

    // Defaults from config (CLI args below can override)
    uint16_t    port          = static_cast<uint16_t>(cfgInt(cfg, "server.port",        25565));
    std::string dbPath        = cfgStr (cfg, "server.db_path",       "playerdata.db");
    int         maxPlayers    = cfgInt (cfg, "server.max_players",   32);
    int         actorAuthorityExteriorRadius = cfgInt(cfg, "server.actorAuthorityExteriorRadius",
        cfgInt(cfg, "server.actor_authority_exterior_radius", 1));
    int         actorAuthorityStickyMs = cfgInt(cfg, "server.actorAuthorityStickyMs",
        cfgInt(cfg, "server.actor_authority_sticky_ms", 3000));
    bool        actorAuthorityPreferExactCell = cfgBool(cfg, "server.actorAuthorityPreferExactCell",
        cfgBool(cfg, "server.actor_authority_prefer_exact_cell", true));
    bool        publicListing = cfgBool(cfg, "master.public_listing", false);
    std::string masterUrl     = cfgStr (cfg, "master.master_url",    "");
    std::string serverName    = cfgStr (cfg, "master.server_name",   "My OpenMW Server");
    std::string gameMode      = cfgStr (cfg, "master.game_mode",     "Co-op");

    // CLI overrides
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc)
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if ((arg == "--log-dir" || arg == "-l") && i + 1 < argc)
            logDir = argv[++i];
        else if (arg == "--config" || arg == "-c") ++i; // already handled
    }

    setupServerLogging(logDir);

    std::cout << "[Server] Config: " << cfgPath.string() << "\n"
              << "[Server] Port: " << port
              << "  MaxPlayers: " << maxPlayers
              << "  DB: " << dbPath << "\n"
              << "[Server] ActorAuthority: exteriorRadius=" << actorAuthorityExteriorRadius
              << " stickyMs=" << actorAuthorityStickyMs
              << " preferExactCell=" << (actorAuthorityPreferExactCell ? "true" : "false") << "\n";
    if (publicListing)
        std::cout << "[Server] MasterServer: " << masterUrl
                  << "  Name: " << serverName << "\n";

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    try
    {
        mwmp::MPServer server(port);
        server.setDbPath(dbPath);
        server.setMaxPlayers(maxPlayers);
        server.setActorAuthorityExteriorRadius(actorAuthorityExteriorRadius);
        server.setActorAuthorityStickyMs(actorAuthorityStickyMs);
        server.setActorAuthorityPreferExactCell(actorAuthorityPreferExactCell);
        if (publicListing && !masterUrl.empty())
        {
            server.setMasterUrl(masterUrl);
            server.setServerName(serverName);
            server.setGameMode(gameMode);
        }
        gServer = &server;
        std::cout << "[Server] Starting on port " << port << "\n";
        server.run();   // blocks until requestStop()
        gServer = nullptr;
        std::cout << "[Server] Exiting cleanly\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Server] Fatal: " << e.what() << "\n";
        teardownServerLogging();
        return 1;
    }

    teardownServerLogging();
    return 0;
}
