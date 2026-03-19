#ifndef OPENMW_SERVER_SCRIPTENGINE_HPP
#define OPENMW_SERVER_SCRIPTENGINE_HPP

#include <filesystem>
#include <string>
#include <vector>

// Forward-declare sol::state and MPServer so this header stays lightweight.
namespace sol { class state; }
namespace mwmp { class MPServer; }

namespace mwmp
{

class ScriptEngine
{
public:
    explicit ScriptEngine(MPServer* server);
    ~ScriptEngine();

    // Load all *.lua files from a directory (alphabetical order).
    void loadScriptsFrom(const std::filesystem::path& dir);

    // Fire a named callback across all loaded scripts.
    // Errors are caught, logged, and never propagated — one broken script
    // never kills the server.
    template<typename... Args>
    void call(const std::string& fn, Args&&... args);

    // Variant for callbacks that can return a bool (e.g. OnPlayerSendMessage).
    // Sets `out` to true if the script returns true, leaves it unchanged otherwise.
    template<typename... Args>
    void callWithReturn(const std::string& fn, bool& out, Args&&... args);

private:
    // sol::state is non-copyable and heavy — owned by pointer so this header
    // doesn't pull in sol/sol.hpp everywhere.
    sol::state* mLua    = nullptr;
    MPServer*   mServer = nullptr;

    std::vector<std::string> mLoadedScripts;

    void registerCoreBindings();
    void handleScriptError(const std::string& context, const std::string& msg);
};

} // namespace mwmp

// Template definitions must be visible at the call site, but we still want
// to keep sol3 headers out of Server.hpp / main.cpp.  Include the impl file
// only from translation units that actually call ScriptEngine::call().
#include "ScriptEngine.inl"

#endif // OPENMW_SERVER_SCRIPTENGINE_HPP
