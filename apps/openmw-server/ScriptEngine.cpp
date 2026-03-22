#include "ScriptEngine.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>

// Pull in full sol3 only in this TU and the bindings/*.cpp files.
#include <sol/sol.hpp>

#include <components/debug/debuglog.hpp>

// Binding registration — forward-declared here, defined in bindings/*.cpp
namespace mwmp
{
    void registerServerBindings(sol::state& lua, MPServer* server);
    void registerPlayerBindings(sol::state& lua, MPServer* server);
}

namespace mwmp
{

ScriptEngine::ScriptEngine(MPServer* server)
    : mLua(new sol::state()), mServer(server)
{
    mLua->open_libraries(
        sol::lib::base,
        sol::lib::string,
        sol::lib::table,
        sol::lib::math,
        sol::lib::io,        // for file access in scripts
        sol::lib::os         // for os.time() etc.
    );

    registerCoreBindings();
}

ScriptEngine::~ScriptEngine()
{
    delete mLua;
}

void ScriptEngine::registerCoreBindings()
{
    registerServerBindings(*mLua, mServer);
    registerPlayerBindings(*mLua, mServer);
}

void ScriptEngine::loadScriptsFrom(const std::filesystem::path& dir)
{
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir))
    {
        Log(Debug::Warning) << "[ScriptEngine] Script directory not found: " << dir;
        return;
    }

    // Collect *.lua files and sort alphabetically for deterministic load order.
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".lua")
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    for (const auto& path : files)
    {
        auto result = mLua->safe_script_file(path.string(),
                                              sol::script_pass_on_error);
        if (!result.valid())
        {
            sol::error err = result;
            handleScriptError(path.filename().string(), err.what());
        }
        else
        {
            mLoadedScripts.push_back(path.filename().string());
            Log(Debug::Info) << "[ScriptEngine] Loaded: " << path.filename();
        }
    }
}

void ScriptEngine::handleScriptError(const std::string& context,
                                      const std::string& msg)
{
    Log(Debug::Warning) << "[ScriptError] " << context << ": " << msg;
}

std::string ScriptEngine::getString(const std::string& tableName,
                                     const std::string& key,
                                     const std::string& defaultVal) const
{
    auto t = mLua->get<sol::optional<sol::table>>(tableName);
    if (!t) return defaultVal;
    auto v = (*t).get<sol::optional<std::string>>(key);
    return v ? *v : defaultVal;
}

} // namespace mwmp
