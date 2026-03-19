#include "ServerBindings.hpp"

#include <sol/sol.hpp>
#include <components/debug/debuglog.hpp>

#include "../Server.hpp"

namespace mwmp
{

void registerServerBindings(sol::state& lua, MPServer* server)
{
    // Create (or fetch) the global `mp` table.
    sol::table mp = lua["mp"].get_or_create<sol::table>();

    // ── Logging ──────────────────────────────────────────────────────────
    // mp.log(text) — routes through the OpenMW log system so script output
    // gets the same timestamp/level prefix as C++ output.
    mp.set_function("log", [](const std::string& msg)
    {
        Log(Debug::Info) << "[Script] " << msg;
    });

    // ── Messaging ────────────────────────────────────────────────────────
    // mp.broadcast(text) — send a chat message from "Server" to all players.
    mp.set_function("broadcast", [server](const std::string& text)
    {
        if (server) server->broadcastServerMessage(text);
    });

    // ── Player queries ───────────────────────────────────────────────────
    // mp.getPlayerCount() — number of fully connected (post-handshake) players.
    mp.set_function("getPlayerCount", [server]() -> int
    {
        return server ? server->getPlayerCount() : 0;
    });

    // mp.getPlayers() — returns a table of all ScriptPlayer usertypes.
    // Defined in PlayerBindings.cpp after the usertype is registered;
    // we leave a placeholder here that PlayerBindings will overwrite.
    // (PlayerBindings runs second in registerCoreBindings.)

    // ── Server info ──────────────────────────────────────────────────────
    // mp.getUptime() — seconds of real time since server start (float).
    mp.set_function("getUptime", [server]() -> double
    {
        return server ? server->getUptime() : 0.0;
    });

    // ── World time ───────────────────────────────────────────────────────
    // mp.getWorldTime() — current authoritative game hour (0..24, float).
    mp.set_function("getWorldTime", [server]() -> float
    {
        return server ? server->getWorldHour() : 0.f;
    });

    // mp.setWorldTime(hour) — override the server clock.
    mp.set_function("setWorldTime", [server](float hour)
    {
        if (server) server->setWorldHour(hour);
    });
}

} // namespace mwmp
