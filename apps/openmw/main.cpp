#include <components/debug/debugging.hpp>
#include <components/fallback/fallback.hpp>
#include <components/fallback/validate.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/misc/osgpluginchecker.hpp>
#include <components/misc/rng.hpp>
#include <components/platform/platform.hpp>
#include <components/version/version.hpp>

#include "mwgui/debugwindow.hpp"

#include "engine.hpp"
#include "options.hpp"
#ifdef BUILD_MULTIPLAYER
#include "mwmp/Identity.hpp"
#include "mwmp/Main.hpp"
#include "mwmp/sha256.hpp"
#endif

#include <boost/program_options/variables_map.hpp>

#if defined(_WIN32)
#include <components/misc/windows.hpp>
// makes __argc and __argv available on windows
#include <cstdlib>

extern "C" __declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 0x00000001;
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <string_view>

#if (defined(__APPLE__) || defined(__linux) || defined(__unix) || defined(__posix))
#include <unistd.h>
#endif

#ifdef BUILD_MULTIPLAYER
namespace
{
    namespace bpo = boost::program_options;

    struct MultiplayerProfilePaths
    {
        std::filesystem::path mConfig;
        std::filesystem::path mUserData;
        std::filesystem::path mLogs;
        std::filesystem::path mKeys;
    };

    Files::MaybeQuotedPath makeMaybeQuotedPath(const std::filesystem::path& value)
    {
        Files::MaybeQuotedPath result;
        static_cast<std::filesystem::path&>(result) = value;
        return result;
    }

    std::string profilePathComponent(std::string_view value)
    {
        std::string slug;
        slug.reserve(value.size());
        bool replaced = false;
        bool previousUnderscore = false;
        for (const unsigned char c : value)
        {
            const bool allowed
                = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
                || c == '_' || c == '.';
            const char output = allowed ? static_cast<char>(c) : '_';
            replaced = replaced || !allowed;
            if (output == '_' && previousUnderscore)
                continue;
            slug.push_back(output);
            previousUnderscore = output == '_';
        }

        while (!slug.empty() && (slug.back() == '.' || slug.back() == ' '))
        {
            slug.pop_back();
            replaced = true;
        }
        if (slug.empty())
        {
            slug = "profile";
            replaced = true;
        }

        static constexpr std::string_view reservedNames[] = {
            "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8",
            "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"
        };
        if (slug.size() > 80)
        {
            slug.resize(80);
            replaced = true;
        }

        std::string lower = slug;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto extension = lower.find('.');
        const std::string_view reservedCandidate
            = extension == std::string::npos ? std::string_view(lower) : std::string_view(lower).substr(0, extension);
        if (std::find(std::begin(reservedNames), std::end(reservedNames), reservedCandidate)
            != std::end(reservedNames))
        {
            slug.insert(slug.begin(), '_');
            replaced = true;
        }

        if (replaced)
            slug += "-" + mwmp::crypto::sha256hex(std::string(value)).substr(0, 12);
        return slug;
    }

    std::optional<MultiplayerProfilePaths> applyMultiplayerProfile(bpo::variables_map& variables)
    {
        const auto profileRootValue = variables["mp-profile-root"].as<Files::MaybeQuotedPath>();
        const std::filesystem::path profileRoot = profileRootValue;
        if (profileRoot.empty())
            return std::nullopt;

        const std::string connect = variables["connect"].as<std::string>();
        std::string account = variables["mp-account"].as<std::string>();
        if (account.empty())
            account = variables["mp-name"].as<std::string>();
        const std::string character = variables["mp-character"].as<std::string>();
        if (connect.empty() || account.empty() || character.empty())
            throw bpo::error("--mp-profile-root requires --connect, --mp-account, and --mp-character");

        const std::filesystem::path accountRoot
            = profileRoot / profilePathComponent(connect) / profilePathComponent(account);
        const std::filesystem::path characterRoot
            = accountRoot / "characters" / profilePathComponent(character);
        MultiplayerProfilePaths paths{
            characterRoot / "config", characterRoot / "userdata", characterRoot / "logs", accountRoot / "mp-keys"
        };

        std::error_code ec;
        for (const auto& path : { paths.mConfig, paths.mUserData, paths.mLogs, paths.mKeys })
        {
            std::filesystem::create_directories(path, ec);
            if (ec)
                throw bpo::error("could not create multiplayer profile directory " + path.string() + ": " + ec.message());
        }

        auto configPaths = variables["config"].as<Files::MaybeQuotedPathContainer>();
        configPaths.push_back(makeMaybeQuotedPath(paths.mConfig));
        variables.at("config") = bpo::variable_value(configPaths, false);
        variables.at("user-data") = bpo::variable_value(makeMaybeQuotedPath(paths.mUserData), false);
        variables.at("log-dir") = bpo::variable_value(makeMaybeQuotedPath(paths.mLogs), false);

        const auto explicitKeysDir = variables["mp-keys-dir"].as<Files::MaybeQuotedPath>();
        if (explicitKeysDir.empty())
            variables.at("mp-keys-dir") = bpo::variable_value(makeMaybeQuotedPath(paths.mKeys), false);
        else
            paths.mKeys = explicitKeysDir;
        return paths;
    }
}
#endif

/**
 * \brief Parses application command line and calls \ref Cfg::ConfigurationManager
 * to parse configuration files.
 *
 * Results are directly written to \ref Engine class.
 *
 * \retval true - Everything goes OK
 * \retval false - Error
 */
bool parseOptions(int argc, char** argv, OMW::Engine& engine, Files::ConfigurationManager& cfgMgr)
{
    // Create a local alias for brevity
    namespace bpo = boost::program_options;
    typedef std::vector<std::string> StringsVector;

    bpo::options_description desc = OpenMW::makeOptionsDescription();
    bpo::variables_map variables;

    Files::parseArgs(argc, argv, variables, desc);
    bpo::notify(variables);

    if (variables.count("help"))
    {
        Debug::getRawStdout() << desc << std::endl;
        return false;
    }

    if (variables.count("version"))
    {
        Debug::getRawStdout() << Version::getOpenmwVersionDescription() << std::endl;
        return false;
    }

    cfgMgr.processPaths(variables, std::filesystem::current_path());

#ifdef BUILD_MULTIPLAYER
    const auto multiplayerProfile = applyMultiplayerProfile(variables);
    engine.setMultiplayerProfileIsolation(multiplayerProfile.has_value());
#endif

    cfgMgr.readConfiguration(variables, desc);

    {
        const auto logDirOverride = variables["log-dir"].as<Files::MaybeQuotedPath>();
        const std::filesystem::path logPath = logDirOverride.empty()
            ? cfgMgr.getLogPath()
            : static_cast<std::filesystem::path>(logDirOverride);
        std::filesystem::create_directories(logPath);
        Debug::setupLogging(logPath, "OpenMW");
    }
    Log(Debug::Info) << Version::getOpenmwVersionDescription();

#ifdef BUILD_MULTIPLAYER
    if (multiplayerProfile)
        Log(Debug::Info) << "Multiplayer profile config: " << multiplayerProfile->mConfig;
#endif

    Settings::Manager::load(cfgMgr);

    MWGui::DebugWindow::startLogRecording();

    engine.setGrabMouse(!variables["no-grab"].as<bool>());

    // Font encoding settings
    std::string encoding(variables["encoding"].as<std::string>());
    Log(Debug::Info) << ToUTF8::encodingUsingMessage(encoding);
    engine.setEncoding(ToUTF8::calculateEncoding(encoding));

    Files::PathContainer dataDirs(asPathContainer(variables["data"].as<Files::MaybeQuotedPathContainer>()));

    Files::PathContainer::value_type local(variables["data-local"]
                                               .as<Files::MaybeQuotedPathContainer::value_type>()
                                               .u8string()); // This call to u8string is redundant, but required to
                                                             // build on MSVC 14.26 due to implementation bugs.
    if (!local.empty())
        dataDirs.push_back(std::move(local));

    cfgMgr.filterOutNonExistingPaths(dataDirs);

    engine.setResourceDir(variables["resources"]
                              .as<Files::MaybeQuotedPath>()
                              .u8string()); // This call to u8string is redundant, but required to build on MSVC 14.26
                                            // due to implementation bugs.
    engine.setDataDirs(dataDirs);

    // fallback archives
    StringsVector archives = variables["fallback-archive"].as<StringsVector>();
    for (StringsVector::const_iterator it = archives.begin(); it != archives.end(); ++it)
    {
        engine.addArchive(*it);
    }

    StringsVector content = variables["content"].as<StringsVector>();
    if (content.empty())
    {
        Log(Debug::Error) << "No content file given (esm/esp, nor omwgame/omwaddon). Aborting...";
        return false;
    }
    engine.addContentFile("builtin.omwscripts");
    std::set<std::string> contentDedupe{ "builtin.omwscripts" };
    for (const auto& contentFile : content)
    {
        if (!contentDedupe.insert(contentFile).second)
        {
            Log(Debug::Error) << "Content file specified more than once: " << contentFile << ". Aborting...";
            return false;
        }
    }

    for (auto& file : content)
    {
        engine.addContentFile(file);
    }

    StringsVector groundcover = variables["groundcover"].as<StringsVector>();
    for (auto& file : groundcover)
    {
        engine.addGroundcoverFile(file);
    }

    if (variables.count("lua-scripts"))
    {
        Log(Debug::Warning) << "Lua scripts have been specified via the old lua-scripts option and will not be loaded. "
                               "Please update them to a version which uses the new omwscripts format.";
    }

    // startup-settings
    engine.setCell(variables["start"].as<std::string>());
    engine.setSkipMenu(variables["skip-menu"].as<bool>(), variables["new-game"].as<bool>());
    if (!variables["skip-menu"].as<bool>() && variables["new-game"].as<bool>())
        Log(Debug::Warning) << "Warning: new-game used without skip-menu -> ignoring it";

    // scripts
    engine.setCompileAll(variables["script-all"].as<bool>());
    engine.setCompileAllDialogue(variables["script-all-dialogue"].as<bool>());
    engine.setScriptConsoleMode(variables["script-console"].as<bool>());
    engine.setStartupScript(variables["script-run"].as<std::string>());
    engine.setWarningsMode(variables["script-warn"].as<int>());
    engine.setSaveGameFile(variables["load-savegame"].as<Files::MaybeQuotedPath>().u8string());

    // other settings
    Fallback::Map::init(variables["fallback"].as<Fallback::FallbackMap>().mMap);
    engine.setSoundUsage(!variables["no-sound"].as<bool>());
    engine.setActivationDistanceOverride(variables["activate-dist"].as<int>());
    engine.enableFontExport(variables["export-fonts"].as<bool>());
    engine.setRandomSeed(variables["random-seed"].as<unsigned int>());

#ifdef BUILD_MULTIPLAYER
    {
        const std::string connectStr = variables["connect"].as<std::string>();
        if (!connectStr.empty())
        {
            std::string host = connectStr;
            uint16_t    port = 25565;
            const auto  sep  = connectStr.rfind(':');
            if (sep != std::string::npos)
            {
                host = connectStr.substr(0, sep);
                port = static_cast<uint16_t>(std::stoi(connectStr.substr(sep + 1)));
            }
            std::string playerName = variables["mp-account"].as<std::string>();
            if (playerName.empty())
                playerName = variables["mp-name"].as<std::string>();
            const std::string characterName = variables["mp-character"].as<std::string>();
            const bool autoEnter = variables["mp-auto-enter"].as<bool>();
            const std::string passwordRaw = variables["mp-password"].as<std::string>();
            // Hash password the same way AccountDialog does so CLI and GUI paths
            // always send the same credential format to the server.
            const std::string passwordHash = passwordRaw.empty()
                ? std::string{}
                : mwmp::crypto::sha256hex(passwordRaw);

            // The GUI character-select path initialises Identity before attempting
            // keypair auth. Do the same for CLI --connect launches so linked
            // accounts can authenticate without --mp-password.
            const auto keysDirValue = variables["mp-keys-dir"].as<Files::MaybeQuotedPath>();
            const std::filesystem::path keysDir = keysDirValue.empty()
                ? std::filesystem::current_path() / "mp-keys"
                : static_cast<const std::filesystem::path&>(keysDirValue);
            mwmp::Main::setStaticKeysDir(keysDir);
            if (keysDir != std::filesystem::current_path() / "mp-keys")
                mwmp::Identity::importLegacyKeypair(
                    host, port, std::filesystem::current_path() / "mp-keys", playerName);

            engine.setMultiplayer(host, port, playerName, passwordHash, characterName, autoEnter);
        }
    }
#endif

    return true;
}

namespace
{
    class OSGLogHandler : public osg::NotifyHandler
    {
        void notify(osg::NotifySeverity severity, const char* msg) override
        {
            // Copy, because osg logging is not thread safe.
            std::string msgCopy(msg);
            if (msgCopy.empty())
                return;

            Debug::Level level;
            switch (severity)
            {
                case osg::ALWAYS:
                case osg::FATAL:
                    level = Debug::Error;
                    break;
                case osg::WARN:
                case osg::NOTICE:
                    level = Debug::Warning;
                    break;
                case osg::INFO:
                    level = Debug::Info;
                    break;
                case osg::DEBUG_INFO:
                case osg::DEBUG_FP:
                default:
                    level = Debug::Debug;
            }
            std::string_view s(msgCopy);
            if (s.find("CullVisitor::apply(Geode&) detected NaN") != std::string_view::npos)
            {
                static std::mutex mutex;
                static std::chrono::steady_clock::time_point lastLogTime;
                static std::size_t suppressedCount = 0;

                const auto now = std::chrono::steady_clock::now();
                std::lock_guard lock(mutex);
                if (lastLogTime.time_since_epoch().count() != 0 && now - lastLogTime < std::chrono::seconds(2))
                {
                    ++suppressedCount;
                    return;
                }

                const std::size_t suppressed = suppressedCount;
                suppressedCount = 0;
                lastLogTime = now;
                Log(level) << (s.back() == '\n' ? s.substr(0, s.size() - 1) : s)
                           << " [rate-limited; suppressed " << suppressed
                           << " repeats; OSG NotifyHandler did not provide the offending node name]";
                return;
            }
            if (s.size() < 1024)
                Log(level) << (s.back() == '\n' ? s.substr(0, s.size() - 1) : s);
            else
            {
                while (!s.empty())
                {
                    size_t lineSize = 1;
                    while (lineSize < s.size() && s[lineSize - 1] != '\n')
                        lineSize++;
                    Log(level) << s.substr(0, s[lineSize - 1] == '\n' ? lineSize - 1 : lineSize);
                    s = s.substr(lineSize);
                }
            }
        }
    };
}

int runApplication(int argc, char* argv[])
{
    Platform::init();

#ifdef __APPLE__
    setenv("OSG_GL_TEXTURE_STORAGE", "OFF", 0);
#endif

    osg::setNotifyHandler(new OSGLogHandler());
    Files::ConfigurationManager cfgMgr;
    std::unique_ptr<OMW::Engine> engine = std::make_unique<OMW::Engine>(cfgMgr);

    engine->setRecastMaxLogLevel(Debug::getRecastMaxLogLevel());

    if (parseOptions(argc, argv, *engine, cfgMgr))
    {
        if (!Misc::checkRequiredOSGPluginsArePresent())
            return 1;

        engine->go();
    }

    return 0;
}

#ifdef ANDROID
extern "C" int SDL_main(int argc, char** argv)
#else
int main(int argc, char** argv)
#endif
{
    return Debug::wrapApplication(&runApplication, argc, argv, "OpenMW");
}

// Platform specific for Windows when there is no console built into the executable.
// Windows will call the WinMain function instead of main in this case, the normal
// main function is then called with the __argc and __argv parameters.
#if defined(_WIN32) && !defined(_CONSOLE)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    return main(__argc, __argv);
}
#endif
