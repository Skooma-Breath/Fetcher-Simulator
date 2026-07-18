#include <chrono>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include <components/lua/configuration.hpp>
#include <components/lua/playerscriptstate.hpp>

namespace
{
    LuaUtil::ScriptsConfiguration makeConfiguration(
        const std::vector<std::pair<std::string, ESM::LuaScriptCfg::Flags>>& entries)
    {
        ESM::LuaScriptsCfg source;
        for (const auto& [path, flags] : entries)
        {
            ESM::LuaScriptCfg& script = source.mScripts.emplace_back();
            script.mScriptPath = VFS::Path::Normalized(path);
            script.mInitializationData = "init:" + path;
            script.mFlags = flags;
        }
        LuaUtil::ScriptsConfiguration result;
        result.init(std::move(source), false);
        return result;
    }

    std::filesystem::path makeTemporaryDirectory()
    {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path result
            = std::filesystem::temp_directory_path() / ("openmw-player-script-state-test-" + std::to_string(suffix));
        std::filesystem::create_directories(result);
        return result;
    }

    struct TemporaryDirectory
    {
        std::filesystem::path mPath = makeTemporaryDirectory();
        ~TemporaryDirectory()
        {
            std::error_code ignored;
            std::filesystem::remove_all(mPath, ignored);
        }
    };

    TEST(PlayerScriptStateTest, RoundTripsPlayerScriptsByPath)
    {
        TemporaryDirectory temporary;
        const auto path = temporary.mPath / "player_scripts.bin";
        const LuaUtil::ScriptsConfiguration savedConfiguration = makeConfiguration({
            { "scripts/a.lua", ESM::LuaScriptCfg::sPlayer },
            { "scripts/global.lua", ESM::LuaScriptCfg::sGlobal },
            { "scripts/b.lua", ESM::LuaScriptCfg::sPlayer },
        });

        ESM::LuaScripts saved;
        ESM::LuaScript scriptA;
        scriptA.mScriptId = 0;
        scriptA.mData = std::string("a\0state", 7);
        scriptA.mTimers.push_back({ ESM::LuaTimer::Type::GAME_TIME, 123.5, "resume", std::string("x\0y", 3) });
        saved.mScripts.push_back(scriptA);
        saved.mScripts.push_back({ 1, "global state", {} });
        saved.mScripts.push_back({ 2, "b state", {} });

        std::string error;
        ASSERT_TRUE(LuaUtil::PlayerScriptState::save(path, savedConfiguration, saved, 100.0, 200.0, error)) << error;

        const LuaUtil::ScriptsConfiguration reorderedConfiguration = makeConfiguration({
            { "scripts/b.lua", ESM::LuaScriptCfg::sPlayer },
            { "scripts/a.lua", ESM::LuaScriptCfg::sPlayer },
            { "scripts/new.lua", ESM::LuaScriptCfg::sPlayer },
        });
        LuaUtil::PlayerScriptStateLoadResult loaded;
        ASSERT_TRUE(LuaUtil::PlayerScriptState::load(path, reorderedConfiguration, loaded, error)) << error;
        EXPECT_TRUE(loaded.mExists);
        EXPECT_FALSE(loaded.mConfigurationMatches);
        EXPECT_DOUBLE_EQ(loaded.mSimulationTime, 100.0);
        EXPECT_DOUBLE_EQ(loaded.mGameTime, 200.0);
        ASSERT_EQ(loaded.mScripts.mScripts.size(), 2);

        const ESM::LuaScript& loadedA = loaded.mScripts.mScripts[0];
        EXPECT_EQ(loadedA.mScriptId, 1);
        EXPECT_EQ(loadedA.mData, scriptA.mData);
        ASSERT_EQ(loadedA.mTimers.size(), 1);
        EXPECT_EQ(loadedA.mTimers[0].mType, ESM::LuaTimer::Type::GAME_TIME);
        EXPECT_DOUBLE_EQ(loadedA.mTimers[0].mTime, 123.5);
        EXPECT_EQ(loadedA.mTimers[0].mCallbackName, "resume");
        EXPECT_EQ(loadedA.mTimers[0].mCallbackArgument, std::string("x\0y", 3));

        const ESM::LuaScript& loadedB = loaded.mScripts.mScripts[1];
        EXPECT_EQ(loadedB.mScriptId, 0);
        EXPECT_EQ(loadedB.mData, "b state");
    }

    TEST(PlayerScriptStateTest, MissingFileIsAValidFirstRun)
    {
        TemporaryDirectory temporary;
        const LuaUtil::ScriptsConfiguration configuration
            = makeConfiguration({ { "scripts/a.lua", ESM::LuaScriptCfg::sPlayer } });
        LuaUtil::PlayerScriptStateLoadResult loaded;
        std::string error;
        EXPECT_TRUE(LuaUtil::PlayerScriptState::load(temporary.mPath / "missing.bin", configuration, loaded, error));
        EXPECT_FALSE(loaded.mExists);
        EXPECT_TRUE(loaded.mScripts.mScripts.empty());
        EXPECT_TRUE(error.empty());
    }

    TEST(PlayerScriptStateTest, RejectsCorruptPayloadWithoutReturningPartialState)
    {
        TemporaryDirectory temporary;
        const auto path = temporary.mPath / "player_scripts.bin";
        const LuaUtil::ScriptsConfiguration configuration
            = makeConfiguration({ { "scripts/a.lua", ESM::LuaScriptCfg::sPlayer } });
        ESM::LuaScripts saved;
        saved.mScripts.push_back({ 0, "state", {} });
        std::string error;
        ASSERT_TRUE(LuaUtil::PlayerScriptState::save(path, configuration, saved, 1.0, 2.0, error)) << error;

        {
            std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
            ASSERT_TRUE(file.good());
            file.seekp(-1, std::ios::end);
            file.put('!');
        }

        LuaUtil::PlayerScriptStateLoadResult loaded;
        EXPECT_FALSE(LuaUtil::PlayerScriptState::load(path, configuration, loaded, error));
        EXPECT_NE(error.find("checksum"), std::string::npos);
        EXPECT_TRUE(loaded.mScripts.mScripts.empty());
    }
}
