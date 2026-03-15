#include "Server.hpp"

#include <algorithm>
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

int main(int argc, char* argv[])
{
    uint16_t port = 25565;
    std::filesystem::path logDir = std::filesystem::path(argv[0]).parent_path() / "logs";

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc)
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if ((arg == "--log-dir" || arg == "-l") && i + 1 < argc)
            logDir = argv[++i];
        else if (arg == "--help" || arg == "-h")
        {
            std::cout << "openmw-server [--port <port>] [--log-dir <dir>]\n"
                      << "  --port, -p      UDP port to listen on (default: 25565)\n"
                      << "  --log-dir, -l   Directory for log files (default: <exe dir>/logs)\n";
            return 0;
        }
    }

    setupServerLogging(logDir);

    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    try
    {
        mwmp::MPServer server(port);
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
