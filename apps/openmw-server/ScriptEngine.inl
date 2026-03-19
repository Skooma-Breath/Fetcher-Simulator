// ScriptEngine.inl — template implementations for ScriptEngine::call().
// Included only by translation units that call ScriptEngine methods.
// Do NOT include this file directly; it is pulled in by ScriptEngine.hpp.

#pragma once

#include <sol/sol.hpp>
#include <components/debug/debuglog.hpp>

namespace mwmp
{

template<typename... Args>
void ScriptEngine::call(const std::string& fn, Args&&... args)
{
    sol::state& lua = *mLua;
    sol::protected_function func = lua[fn];
    if (!func.valid())
        return; // callback not defined — silently skip

    auto result = func(std::forward<Args>(args)...);
    if (!result.valid())
    {
        sol::error err = result;
        handleScriptError(fn, err.what());
    }
}

template<typename... Args>
void ScriptEngine::callWithReturn(const std::string& fn, bool& out, Args&&... args)
{
    sol::state& lua = *mLua;
    sol::protected_function func = lua[fn];
    if (!func.valid())
        return;

    auto result = func(std::forward<Args>(args)...);
    if (!result.valid())
    {
        sol::error err = result;
        handleScriptError(fn, err.what());
        return;
    }

    // Only override `out` if the script explicitly returned false.
    sol::optional<bool> ret = result;
    if (ret && *ret == false)
        out = false;
}

} // namespace mwmp
