#include <filesystem>
#include <fstream>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <components/lua/asyncpackage.hpp>
#include <components/lua/storage.hpp>

namespace
{
    using namespace testing;

    template <typename T>
    T get(sol::state_view& lua, std::string luaCode)
    {
        return lua.safe_script("return " + luaCode).get<T>();
    }

    TEST(LuaUtilStorageTest, Subscribe)
    {
        // Note: LuaUtil::Callback can be used only if Lua is initialized via LuaUtil::LuaState
        LuaUtil::LuaState luaState{ nullptr, nullptr };
        luaState.protectedCall([](LuaUtil::LuaView& view) {
            sol::state_view& lua = view.sol();
            LuaUtil::LuaStorage::initLuaBindings(view);
            LuaUtil::LuaStorage storage;
            storage.setActive(true);

            sol::table callbackHiddenData(lua, sol::create);
            callbackHiddenData[LuaUtil::ScriptsContainer::sScriptIdKey] = LuaUtil::ScriptId{};
            LuaUtil::getAsyncPackageInitializer(
                lua.lua_state(), []() { return 0.0; }, []() { return 0.0; })(callbackHiddenData);
            lua["async"] = LuaUtil::AsyncPackageId{ nullptr, 0, callbackHiddenData };

            lua["mutable"] = storage.getMutableSection(lua, "test");
            lua["ro"] = storage.getReadOnlySection(lua, "test");

            lua.safe_script(R"(
                callbackCalls = {}
                ro:subscribe(async:callback(function(section, key)
                    table.insert(callbackCalls, section .. '_' .. (key or '*'))
                end))
            )");

            lua.safe_script("mutable:set('x', 5)");
            EXPECT_EQ(get<int>(lua, "mutable:get('x')"), 5);
            EXPECT_EQ(get<int>(lua, "ro:get('x')"), 5);

            EXPECT_THROW(lua.safe_script("ro:set('y', 3)"), std::exception);

            lua.safe_script("t1 = mutable:asTable()");
            lua.safe_script("t2 = ro:asTable()");
            EXPECT_EQ(get<int>(lua, "t1.x"), 5);
            EXPECT_EQ(get<int>(lua, "t2.x"), 5);

            lua.safe_script("mutable:reset()");
            EXPECT_TRUE(get<bool>(lua, "ro:get('x') == nil"));

            lua.safe_script("mutable:reset({x=4, y=7})");
            EXPECT_EQ(get<int>(lua, "ro:get('x')"), 4);
            EXPECT_EQ(get<int>(lua, "ro:get('y')"), 7);

            EXPECT_THAT(get<std::string>(lua, "table.concat(callbackCalls, ', ')"), "test_x, test_*, test_*");
        });
    }

    TEST(LuaUtilStorageTest, Table)
    {
        LuaUtil::LuaState luaState{ nullptr, nullptr };
        luaState.protectedCall([](LuaUtil::LuaView& view) {
            LuaUtil::LuaStorage::initLuaBindings(view);
            LuaUtil::LuaStorage storage;
            auto& lua = view.sol();
            storage.setActive(true);
            lua["mutable"] = storage.getMutableSection(lua, "test");
            lua["ro"] = storage.getReadOnlySection(lua, "test");

            lua.safe_script("mutable:set('x', { y = 'abc', z = 7 })");
            EXPECT_EQ(get<int>(lua, "mutable:get('x').z"), 7);
            EXPECT_THROW(lua.safe_script("mutable:get('x').z = 3"), std::exception);
            EXPECT_NO_THROW(lua.safe_script("mutable:getCopy('x').z = 3"));
            EXPECT_EQ(get<int>(lua, "mutable:get('x').z"), 7);
            EXPECT_EQ(get<int>(lua, "ro:get('x').z"), 7);
            EXPECT_EQ(get<std::string>(lua, "ro:get('x').y"), "abc");
        });
    }

    TEST(LuaUtilStorageTest, Saving)
    {
        LuaUtil::LuaState luaState{ nullptr, nullptr };
        luaState.protectedCall([](LuaUtil::LuaView& view) {
            LuaUtil::LuaStorage::initLuaBindings(view);
            LuaUtil::LuaStorage storage;
            auto& lua = view.sol();
            storage.setActive(true);

            lua["permanent"] = storage.getMutableSection(lua, "permanent");
            lua["temporary"] = storage.getMutableSection(lua, "temporary");
            lua.safe_script("temporary:removeOnExit()");
            lua.safe_script("permanent:set('x', 1)");
            lua.safe_script("temporary:set('y', 2)");

            const auto tmpFile = std::filesystem::temp_directory_path() / "test_storage.bin";
            storage.save(lua, tmpFile);
            EXPECT_EQ(get<int>(lua, "permanent:get('x')"), 1);
            EXPECT_EQ(get<int>(lua, "temporary:get('y')"), 2);

            storage.clearTemporaryAndRemoveCallbacks();
            lua["permanent"] = storage.getMutableSection(lua, "permanent");
            lua["temporary"] = storage.getMutableSection(lua, "temporary");
            EXPECT_EQ(get<int>(lua, "permanent:get('x')"), 1);
            EXPECT_TRUE(get<bool>(lua, "temporary:get('y') == nil"));

            lua.safe_script("permanent:set('x', 3)");
            lua.safe_script("permanent:set('z', 4)");

            LuaUtil::LuaStorage storage2;
            storage2.setActive(true);
            storage2.load(lua, tmpFile);
            lua["permanent"] = storage2.getMutableSection(lua, "permanent");
            lua["temporary"] = storage2.getMutableSection(lua, "temporary");

            EXPECT_EQ(get<int>(lua, "permanent:get('x')"), 1);
            EXPECT_TRUE(get<bool>(lua, "permanent:get('z') == nil"));
            EXPECT_TRUE(get<bool>(lua, "temporary:get('y') == nil"));
        });
    }

    TEST(LuaUtilStorageTest, ReplacePreservesSectionViewsAndRejectsCorruptFiles)
    {
        LuaUtil::LuaState luaState{ nullptr, nullptr };
        luaState.protectedCall([](LuaUtil::LuaView& view) {
            LuaUtil::LuaStorage::initLuaBindings(view);
            auto& lua = view.sol();

            LuaUtil::LuaStorage source;
            source.setActive(true);
            lua["sourceKept"] = source.getMutableSection(lua, "kept");
            lua["sourceAdded"] = source.getMutableSection(lua, "added");
            lua.safe_script("sourceKept:set('value', 42); sourceAdded:set('value', 7)");

            const auto tmpDir = std::filesystem::temp_directory_path() / "openmw-storage-replace-test";
            const auto storageFile = tmpDir / "player_storage.bin";
            source.save(lua, storageFile);

            LuaUtil::LuaStorage target;
            target.setActive(true);
            sol::table callbackHiddenData(lua, sol::create);
            callbackHiddenData[LuaUtil::ScriptsContainer::sScriptIdKey] = LuaUtil::ScriptId{};
            LuaUtil::getAsyncPackageInitializer(
                lua.lua_state(), []() { return 0.0; }, []() { return 0.0; })(callbackHiddenData);
            lua["async"] = LuaUtil::AsyncPackageId{ nullptr, 0, callbackHiddenData };
            lua["kept"] = target.getMutableSection(lua, "kept");
            lua["removed"] = target.getMutableSection(lua, "removed");
            lua.safe_script(R"(
                replacementCallbacks = 0
                kept:subscribe(async:callback(function()
                    replacementCallbacks = replacementCallbacks + 1
                end))
                kept:set('value', 1)
                removed:set('value', 2)
                replacementCallbacks = 0
            )");

            std::string error;
            EXPECT_TRUE(target.replaceFromFile(lua, storageFile, error)) << error;
            EXPECT_EQ(get<int>(lua, "kept:get('value')"), 42);
            EXPECT_TRUE(get<bool>(lua, "removed:get('value') == nil"));
            EXPECT_EQ(get<int>(lua, "replacementCallbacks"), 1);
            lua["added"] = target.getMutableSection(lua, "added");
            EXPECT_EQ(get<int>(lua, "added:get('value')"), 7);

            const auto corruptFile = tmpDir / "corrupt.bin";
            {
                std::ofstream out(corruptFile, std::ios::binary | std::ios::trunc);
                out << "not serialized Lua storage";
            }
            EXPECT_FALSE(target.replaceFromFile(lua, corruptFile, error));
            EXPECT_EQ(get<int>(lua, "kept:get('value')"), 42);

            std::error_code ec;
            std::filesystem::remove_all(tmpDir, ec);
        });
    }

    TEST(LuaUtilStorageTest, ReplaceOverBaselinePreservesNewDefaultsAndClearsPreviousCharacter)
    {
        LuaUtil::LuaState luaState{ nullptr, nullptr };
        luaState.protectedCall([](LuaUtil::LuaView& view) {
            LuaUtil::LuaStorage::initLuaBindings(view);
            auto& lua = view.sol();
            const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
            const auto tmpDir = std::filesystem::temp_directory_path()
                / ("openmw-storage-baseline-test-" + std::to_string(suffix));
            const auto characterAFile = tmpDir / "character-a.bin";

            LuaUtil::LuaStorage characterA;
            characterA.setActive(true);
            lua["savedSettings"] = characterA.getMutableSection(lua, "SettingsTest");
            lua.safe_script("savedSettings:set('existing', 42); savedSettings:set('flag', false); "
                            "savedSettings:set('zero', 0); savedSettings:set('empty', {}); "
                            "savedSettings:set('unknownOldKey', 7)");
            characterA.save(lua, characterAFile);

            LuaUtil::LuaStorage target;
            target.setActive(true);
            lua["settings"] = target.getMutableSection(lua, "SettingsTest");
            lua["oldCharacterOnly"] = target.getMutableSection(lua, "OldCharacterOnly");
            lua.safe_script("settings:set('existing', 10); settings:set('newDefault', 99); "
                            "settings:set('flag', true)");
            target.captureBaseline();

            // Values written after the baseline represent the previously bound character.
            lua.safe_script("settings:set('leakedValue', 123); oldCharacterOnly:set('value', 5)");

            std::string error;
            ASSERT_TRUE(target.replaceFromFileOverBaseline(lua, characterAFile, error)) << error;
            EXPECT_EQ(get<int>(lua, "settings:get('existing')"), 42);
            EXPECT_EQ(get<int>(lua, "settings:get('newDefault')"), 99);
            EXPECT_FALSE(get<bool>(lua, "settings:get('flag')"));
            EXPECT_EQ(get<int>(lua, "settings:get('zero')"), 0);
            EXPECT_EQ(get<int>(lua, "#settings:get('empty')"), 0);
            EXPECT_EQ(get<int>(lua, "settings:get('unknownOldKey')"), 7);
            EXPECT_TRUE(get<bool>(lua, "settings:get('leakedValue') == nil"));
            EXPECT_TRUE(get<bool>(lua, "oldCharacterOnly:get('value') == nil"));

            // A character with no file gets the current defaults, not character A's values.
            ASSERT_TRUE(target.replaceFromFileOverBaseline(lua, tmpDir / "character-b-missing.bin", error)) << error;
            EXPECT_EQ(get<int>(lua, "settings:get('existing')"), 10);
            EXPECT_EQ(get<int>(lua, "settings:get('newDefault')"), 99);
            EXPECT_TRUE(get<bool>(lua, "settings:get('flag')"));
            EXPECT_TRUE(get<bool>(lua, "settings:get('unknownOldKey') == nil"));

            std::error_code ec;
            std::filesystem::remove_all(tmpDir, ec);
        });
    }

}
