#include "storage.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <components/debug/debuglog.hpp>

#include "luastate.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sol
{
    template <>
    struct is_automagical<LuaUtil::LuaStorage::SectionView> : std::false_type
    {
    };
}

namespace LuaUtil
{
    namespace
    {
        std::filesystem::path makeTemporaryStoragePath(const std::filesystem::path& path)
        {
            static std::atomic<std::uint64_t> sequence{ 0 };
#ifdef _WIN32
            const auto processId = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
            const auto processId = static_cast<std::uint64_t>(getpid());
#endif
            return path.parent_path()
                / (path.filename().string() + ".tmp-" + std::to_string(processId) + "-"
                    + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        }

        void replaceStorageFile(
            const std::filesystem::path& temporaryPath, const std::filesystem::path& destinationPath)
        {
#ifdef _WIN32
            if (!MoveFileExW(temporaryPath.c_str(), destinationPath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                throw std::system_error(
                    static_cast<int>(GetLastError()), std::system_category(), "Unable to replace Lua storage file");
            }
#else
            std::filesystem::rename(temporaryPath, destinationPath);
#endif
        }
    }

    LuaStorage::Value LuaStorage::Section::sEmpty;

    void LuaStorage::registerLifeTime(LuaUtil::LuaView& view, sol::table& res)
    {
        res["LIFE_TIME"] = LuaUtil::makeStrictReadOnly(tableFromPairs<std::string_view, Section::LifeTime>(view.sol(),
            {
                { "Persistent", Section::LifeTime::Persistent },
                { "GameSession", Section::LifeTime::GameSession },
                { "Temporary", Section::LifeTime::Temporary },
            }));
    }

    sol::object LuaStorage::Value::getCopy(lua_State* state) const
    {
        return deserialize(state, mSerializedValue);
    }

    sol::object LuaStorage::Value::getReadOnly(lua_State* state) const
    {
        if (mReadOnlyValue == sol::nil && !mSerializedValue.empty())
            mReadOnlyValue = sol::main_object(deserialize(state, mSerializedValue, nullptr, true));
        return mReadOnlyValue;
    }

    const LuaStorage::Value& LuaStorage::Section::get(std::string_view key) const
    {
        checkIfActive();
        auto it = mValues.find(key);
        if (it != mValues.end())
            return it->second;
        else
            return sEmpty;
    }

    void LuaStorage::Section::runCallbacks(sol::optional<std::string_view> changedKey)
    {
        mStorage->mRunningCallbacks.insert(this);
        mCallbacks.erase(std::remove_if(mCallbacks.begin(), mCallbacks.end(),
                             [&](const Callback& callback) {
                                 bool valid = callback.isValid();
                                 if (valid)
                                     callback.tryCall(mSectionName, changedKey);
                                 return !valid;
                             }),
            mCallbacks.end());
        mMenuScriptsCallbacks.erase(std::remove_if(mMenuScriptsCallbacks.begin(), mMenuScriptsCallbacks.end(),
                                        [&](const Callback& callback) {
                                            bool valid = callback.isValid();
                                            if (valid)
                                                callback.tryCall(mSectionName, changedKey);
                                            return !valid;
                                        }),
            mMenuScriptsCallbacks.end());
        mStorage->mRunningCallbacks.erase(this);
    }

    void LuaStorage::Section::throwIfCallbackRecursionIsTooDeep()
    {
        if (mStorage->mRunningCallbacks.count(this) > 0)
            throw std::runtime_error(
                "Storage handler shouldn't change the storage section it handles (leads to an infinite recursion)");
        if (mStorage->mRunningCallbacks.size() > 10)
            throw std::runtime_error(
                "Too many subscribe callbacks triggering in a chain, likely an infinite recursion");
    }

    void LuaStorage::Section::set(std::string_view key, const sol::object& value)
    {
        checkIfActive();
        throwIfCallbackRecursionIsTooDeep();
        if (value != sol::nil)
            mValues[std::string(key)] = Value(value);
        else
        {
            auto it = mValues.find(key);
            if (it != mValues.end())
                mValues.erase(it);
        }
        if (mStorage->mListener)
            mStorage->mListener->valueChanged(mSectionName, key, value);
        runCallbacks(key);
    }

    void LuaStorage::Section::setAll(const sol::optional<sol::table>& values)
    {
        checkIfActive();
        throwIfCallbackRecursionIsTooDeep();
        mValues.clear();
        if (values)
        {
            for (const auto& [k, v] : *values)
                mValues[cast<std::string>(k)] = Value(v);
        }
        if (mStorage->mListener)
            mStorage->mListener->sectionReplaced(mSectionName, values);
        runCallbacks(sol::nullopt);
    }

    sol::table LuaStorage::Section::asTable(lua_State* state)
    {
        checkIfActive();
        sol::table res(state, sol::create);
        for (const auto& [k, v] : mValues)
            res[k] = v.getCopy(state);
        return res;
    }

    void LuaStorage::initLuaBindings(LuaUtil::LuaView& view)
    {
        sol::usertype<SectionView> sview = view.sol().new_usertype<SectionView>("Section");
        sview["get"] = [](sol::this_state s, const SectionView& section, std::string_view key) {
            return section.mSection->get(key).getReadOnly(s);
        };
        sview["getCopy"] = [](sol::this_state s, const SectionView& section, std::string_view key) {
            return section.mSection->get(key).getCopy(s);
        };
        sview["asTable"]
            = [](sol::this_state lua, const SectionView& section) { return section.mSection->asTable(lua); };
        sview["subscribe"] = [](const SectionView& section, const sol::table& callback) {
            std::vector<Callback>& callbacks
                = section.mForMenuScripts ? section.mSection->mMenuScriptsCallbacks : section.mSection->mCallbacks;
            if (!callbacks.empty() && callbacks.size() == callbacks.capacity())
            {
                callbacks.erase(
                    std::remove_if(callbacks.begin(), callbacks.end(), [&](const Callback& c) { return !c.isValid(); }),
                    callbacks.end());
            }
            callbacks.push_back(Callback::fromLua(callback));
        };
        sview["reset"] = [](const SectionView& section, const sol::optional<sol::table>& newValues) {
            if (section.mReadOnly)
                throw std::runtime_error("Access to storage is read only");
            section.mSection->setAll(newValues);
        };
        sview["removeOnExit"] = [](const SectionView& section) {
            if (section.mReadOnly)
                throw std::runtime_error("Access to storage is read only");
            section.mSection->mLifeTime = Section::Temporary;
        };
        sview["setLifeTime"] = [](const SectionView& section, Section::LifeTime lifeTime) {
            if (section.mReadOnly)
                throw std::runtime_error("Access to storage is read only");
            section.mSection->mLifeTime = lifeTime;
        };
        sview["set"] = [](const SectionView& section, std::string_view key, const sol::object& value) {
            if (section.mReadOnly)
                throw std::runtime_error("Access to storage is read only");
            section.mSection->set(key, value);
        };
    }

    sol::table LuaStorage::initGlobalPackage(LuaUtil::LuaView& view, LuaStorage* globalStorage)
    {
        sol::table res(view.sol(), sol::create);
        registerLifeTime(view, res);

        res["globalSection"] = [globalStorage](sol::this_state lua, std::string_view section) {
            return globalStorage->getMutableSection(lua, section);
        };
        res["allGlobalSections"] = [globalStorage](sol::this_state lua) { return globalStorage->getAllSections(lua); };
        return LuaUtil::makeReadOnly(res);
    }

    sol::table LuaStorage::initLocalPackage(LuaUtil::LuaView& view, LuaStorage* globalStorage)
    {
        sol::table res(view.sol(), sol::create);
        registerLifeTime(view, res);

        res["globalSection"] = [globalStorage](sol::this_state lua, std::string_view section) {
            return globalStorage->getReadOnlySection(lua, section);
        };
        return LuaUtil::makeReadOnly(res);
    }

    sol::table LuaStorage::initPlayerPackage(
        LuaUtil::LuaView& view, LuaStorage* globalStorage, LuaStorage* playerStorage)
    {
        sol::table res(view.sol(), sol::create);
        registerLifeTime(view, res);

        res["globalSection"] = [globalStorage](sol::this_state lua, std::string_view section) {
            return globalStorage->getReadOnlySection(lua, section);
        };
        res["playerSection"] = [playerStorage](sol::this_state lua, std::string_view section) {
            return playerStorage->getMutableSection(lua, section);
        };
        res["allPlayerSections"] = [playerStorage](sol::this_state lua) { return playerStorage->getAllSections(lua); };
        return LuaUtil::makeReadOnly(res);
    }

    sol::table LuaStorage::initMenuPackage(LuaUtil::LuaView& view, LuaStorage* globalStorage, LuaStorage* playerStorage)
    {
        sol::table res(view.sol(), sol::create);
        registerLifeTime(view, res);

        res["playerSection"] = [playerStorage](sol::this_state lua, std::string_view section) {
            return playerStorage->getMutableSection(lua, section, /*forMenuScripts=*/true);
        };
        res["globalSection"] = [globalStorage](sol::this_state lua, std::string_view section) {
            return globalStorage->getReadOnlySection(lua, section);
        };
        res["allPlayerSections"] = [playerStorage](sol::this_state lua) { return playerStorage->getAllSections(lua); };
        return LuaUtil::makeReadOnly(res);
    }

    sol::table LuaStorage::initLoadPackage(LuaUtil::LuaView& view, LuaStorage* playerStorage)
    {
        sol::table res(view.sol(), sol::create);
        registerLifeTime(view, res);

        res["playerSection"] = [playerStorage](sol::this_state lua, std::string_view section) {
            return playerStorage->getReadOnlySection(lua, section);
        };
        return LuaUtil::makeReadOnly(res);
    }

    void LuaStorage::clearTemporaryAndRemoveCallbacks()
    {
        auto it = mData.begin();
        while (it != mData.end())
        {
            it->second->mCallbacks.clear();
            // Note that we don't clear menu callbacks for permanent sections
            // because starting/loading a game doesn't reset menu scripts.
            if (it->second->mLifeTime == Section::Temporary)
            {
                it->second->mMenuScriptsCallbacks.clear();
                it->second->mValues.clear();
                it = mData.erase(it);
            }
            else
                ++it;
        }
    }

    void LuaStorage::load(lua_State* state, const std::filesystem::path& path)
    {
        assert(mData.empty()); // Shouldn't be used before loading
        try
        {
            std::uintmax_t fileSize = std::filesystem::file_size(path);
            Log(Debug::Info) << "Loading Lua storage \"" << path << "\" (" << fileSize << " bytes)";
            if (fileSize == 0)
                throw std::runtime_error("Storage file has zero length");

            std::ifstream fin(path, std::fstream::binary);
            std::string serializedData((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
            sol::table data = deserialize(state, serializedData);
            for (const auto& [sectionName, sectionTable] : data)
            {
                const std::shared_ptr<Section>& section = getSection(cast<std::string_view>(sectionName));
                for (const auto& [key, value] : cast<sol::table>(sectionTable))
                    section->set(cast<std::string_view>(key), value);
            }
        }
        catch (std::exception& e)
        {
            Log(Debug::Error) << "Cannot read \"" << path << "\": " << e.what();
        }
    }

    bool LuaStorage::replaceFromFile(lua_State* state, const std::filesystem::path& path, std::string& error, bool clearMissingSections)
    {
        error.clear();
        std::vector<std::pair<std::string, sol::table>> loadedSections;
        try
        {
            if (std::filesystem::exists(path))
            {
                const std::uintmax_t fileSize = std::filesystem::file_size(path);
                Log(Debug::Info) << "Loading Lua storage \"" << path << "\" (" << fileSize << " bytes)";
                if (fileSize == 0)
                    throw std::runtime_error("Storage file has zero length");

                std::ifstream fin(path, std::fstream::binary);
                if (!fin)
                    throw std::runtime_error("Unable to open storage file");
                std::string serializedData((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
                sol::table data = deserialize(state, serializedData);
                for (const auto& [sectionName, sectionTable] : data)
                {
                    loadedSections.emplace_back(
                        cast<std::string>(sectionName), cast<sol::table>(sectionTable));
                }
            }

            std::set<std::string, std::less<>> loadedNames;
            for (const auto& [sectionName, _] : loadedSections)
                loadedNames.insert(sectionName);

            if (clearMissingSections)
            {
                for (const auto& [sectionName, section] : mData)
                {
                    if (!loadedNames.contains(sectionName))
                        section->setAll(sol::nullopt);
                }
            }
            for (const auto& [sectionName, values] : loadedSections)
                getSection(sectionName)->setAll(values);
            return true;
        }
        catch (const std::exception& e)
        {
            error = e.what();
            Log(Debug::Error) << "Cannot replace Lua storage from \"" << path << "\": " << error;
            return false;
        }
    }

    void LuaStorage::save(lua_State* state, const std::filesystem::path& path) const
    {
        sol::table data(state, sol::create);
        for (const auto& [sectionName, section] : mData)
        {
            if (section->mLifeTime == Section::Persistent && !section->mValues.empty())
                data[sectionName] = section->asTable(state);
        }
        std::string serializedData = serialize(data);
        Log(Debug::Info) << "Saving Lua storage \"" << path << "\" (" << serializedData.size() << " bytes)";
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path());
        const std::filesystem::path temporaryPath = makeTemporaryStoragePath(path);
        try
        {
            std::ofstream fout(temporaryPath, std::fstream::binary | std::fstream::trunc);
            fout.exceptions(std::ios::badbit | std::ios::failbit);
            fout.write(serializedData.data(), serializedData.size());
            fout.flush();
            fout.close();
            replaceStorageFile(temporaryPath, path);
        }
        catch (...)
        {
            std::error_code ec;
            std::filesystem::remove(temporaryPath, ec);
            throw;
        }
    }

    std::vector<LuaStorage::SerializedValue> LuaStorage::getSerializedValues() const
    {
        checkIfActive();

        std::vector<SerializedValue> values;
        for (const auto& [sectionName, section] : mData)
        {
            for (const auto& [key, value] : section->mValues)
                values.push_back({ std::string(sectionName), key, value.serialized() });
        }
        return values;
    }

    const std::shared_ptr<LuaStorage::Section>& LuaStorage::getSection(std::string_view sectionName)
    {
        checkIfActive();
        auto it = mData.find(sectionName);
        if (it != mData.end())
            return it->second;
        auto section = std::make_shared<Section>(this, std::string(sectionName));
        sectionName = section->mSectionName;
        auto [newIt, _] = mData.emplace(sectionName, std::move(section));
        return newIt->second;
    }

    sol::object LuaStorage::getSection(
        lua_State* state, std::string_view sectionName, bool readOnly, bool forMenuScripts)
    {
        checkIfActive();
        const std::shared_ptr<Section>& section = getSection(sectionName);
        return sol::make_object<SectionView>(state, SectionView{ section, readOnly, forMenuScripts });
    }

    sol::table LuaStorage::getAllSections(lua_State* state, bool readOnly)
    {
        checkIfActive();
        sol::table res(state, sol::create);
        for (const auto& [sectionName, _] : mData)
            res[sectionName] = getSection(state, sectionName, readOnly);
        return res;
    }

}
