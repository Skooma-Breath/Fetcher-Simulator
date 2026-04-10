#include "ServerBindings.hpp"

#include <components/lua/serialization.hpp>
#include <components/lua/storage.hpp>
#include <components/lua/luastate.hpp>
#include <sol/sol.hpp>
#include <components/debug/debuglog.hpp>

#include "PlayerBindings.hpp"
#include "../LuaServerContext.hpp"

namespace mwmp
{

sol::table initMpPackage(LuaUtil::LuaView& view, LuaServerContext* context, LuaUtil::LuaStorage* storage)
{
    sol::table mp = view.newTable();

    initPlayerBindings(view, mp, context);

    // ── Logging ──────────────────────────────────────────────────────────
    // mp.log(text) — routes through the OpenMW log system so script output
    // gets the same timestamp/level prefix as C++ output.
    mp.set_function("log", [](const std::string& msg)
    {
        Log(Debug::Info) << "[Script] " << msg;
    });

    // ── Messaging ────────────────────────────────────────────────────────
    // mp.broadcast(text) — send a chat message from "Server" to all players.
    mp.set_function("broadcast", sol::overload(
        [context](const std::string& text)
        {
            if (context) context->queueBroadcastServerMessage(text);
        },
        [context](const std::string& eventName, const sol::table& data)
        {
            if (context) context->queueBroadcastLuaEvent(eventName, LuaUtil::serialize(data));
        }));

    mp.set_function("broadcastToCell", sol::overload(
        [context](const std::string& cellId, const std::string& text)
        {
            if (context) context->queueBroadcastServerMessageToCell(cellId, text);
        },
        [context](const std::string& cellId, const std::string& eventName, const sol::table& data)
        {
            if (context) context->queueBroadcastLuaEventToCell(cellId, eventName, LuaUtil::serialize(data));
        }));

    mp.set_function("send", [context](uint32_t guid, const std::string& eventName, const sol::table& data)
    {
        if (context) context->queueSendLuaEvent(guid, eventName, LuaUtil::serialize(data));
    });

    mp.set_function("kick", [context](uint32_t guid, const std::string& reason)
    {
        if (context) context->queueKickClient(guid, reason);
    });

    mp.set_function("isServer", []() -> bool
    {
        return true;
    });

    // ── Player queries ───────────────────────────────────────────────────
    // mp.getPlayerCount() — number of fully connected (post-handshake) players.
    mp.set_function("getPlayerCount", [context]() -> int
    {
        return context ? context->getPlayerCount() : 0;
    });

    // mp.getPlayers() — returns a table of all ScriptPlayer usertypes.
    // Defined in PlayerBindings.cpp after the usertype is registered;
    // we leave a placeholder here that PlayerBindings will overwrite.
    // (PlayerBindings runs second in registerCoreBindings.)

    // ── Server info ──────────────────────────────────────────────────────
    // mp.getUptime() — seconds of real time since server start (float).
    mp.set_function("getUptime", [context]() -> double
    {
        return context ? context->getUptime() : 0.0;
    });

    // ── World time ───────────────────────────────────────────────────────
    // mp.getWorldTime() — current authoritative game hour (0..24, float).
    mp.set_function("getWorldTime", [context]() -> float
    {
        return context ? context->getWorldHour() : 0.f;
    });

    mp.set_function("getTime", [context]() -> float
    {
        return context ? context->getWorldHour() : 0.f;
    });

    // mp.setWorldTime(hour) — override the server clock.
    mp.set_function("setWorldTime", [context](float hour)
    {
        if (context) context->queueSetWorldHour(hour);
    });

    mp.set_function("setTime", [context](float hour)
    {
        if (context) context->queueSetWorldHour(hour);
    });

    // mp.relayChat(guid, text) — relay a player-authored chat message with
    // the server-authoritative display name for the sender.
    mp.set_function("relayChat", [context](uint32_t guid, const std::string& text)
    {
        if (context) context->queueRelayPlayerChat(guid, text);
    });

    if (storage)
        mp["storage"] = LuaUtil::LuaStorage::initGlobalPackage(view, storage);

    return mp;
}

} // namespace mwmp
