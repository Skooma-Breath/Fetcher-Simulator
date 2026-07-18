#ifndef COMPONENTS_LUA_PLAYERSCRIPTSTATE_H
#define COMPONENTS_LUA_PLAYERSCRIPTSTATE_H

#include <cstdint>
#include <filesystem>
#include <string>

#include <components/esm/luascripts.hpp>

namespace LuaUtil
{
    class ScriptsConfiguration;

    struct PlayerScriptStateLoadResult
    {
        ESM::LuaScripts mScripts;
        double mSimulationTime = 0.0;
        double mGameTime = 0.0;
        bool mExists = false;
        bool mConfigurationMatches = true;
    };

    // Standalone persistence for multiplayer PLAYER script onSave/onLoad payloads.
    // Script paths are stored instead of numeric configuration ids so added or
    // removed content files do not shift state onto a different script.
    class PlayerScriptState
    {
    public:
        static constexpr std::uint32_t sFormatVersion = 2;
        static constexpr std::uint64_t sMaximumFileSize = 16 * 1024 * 1024;

        static bool load(const std::filesystem::path& path, const ScriptsConfiguration& configuration,
            PlayerScriptStateLoadResult& result, std::string& error);

        static bool save(const std::filesystem::path& path, const ScriptsConfiguration& configuration,
            const ESM::LuaScripts& scripts, double simulationTime, double gameTime, std::string& error);

        static std::uint64_t configurationFingerprint(const ScriptsConfiguration& configuration);
    };
}

#endif
