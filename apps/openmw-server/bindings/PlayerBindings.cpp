#include "PlayerBindings.hpp"

#include <sol/sol.hpp>
#include <components/debug/debuglog.hpp>

#include "../Server.hpp"

namespace mwmp
{

// ---------------------------------------------------------------------------
// Method implementations for ScriptPlayer.
// These are free functions rather than member functions so that the full
// Server.hpp definition (needed to call MPServer methods) is only required
// in this one translation unit.
// ---------------------------------------------------------------------------

static std::string scriptPlayer_getName(const ScriptPlayer& p)
{
    auto* c = p.server ? p.server->findClientByGuid(p.guid) : nullptr;
    return c ? c->name : "";
}

static std::string scriptPlayer_getCell(const ScriptPlayer& p)
{
    auto* c = p.server ? p.server->findClientByGuid(p.guid) : nullptr;
    return c ? c->player.cell.cellName : "";
}

static uint32_t scriptPlayer_getGuid(const ScriptPlayer& p)
{
    return p.guid;
}

static sol::table scriptPlayer_getPosition(const ScriptPlayer& p, sol::this_state ts)
{
    sol::state_view lua(ts);
    sol::table t = lua.create_table();
    auto* c = p.server ? p.server->findClientByGuid(p.guid) : nullptr;
    if (c)
    {
        t["x"] = c->player.position.pos[0];
        t["y"] = c->player.position.pos[1];
        t["z"] = c->player.position.pos[2];
    }
    else { t["x"] = 0.f; t["y"] = 0.f; t["z"] = 0.f; }
    return t;
}

static void scriptPlayer_sendMessage(const ScriptPlayer& p, const std::string& text)
{
    if (p.server) p.server->sendServerMessage(p.guid, text);
}

static void scriptPlayer_kick(const ScriptPlayer& p, const std::string& reason)
{
    if (p.server) p.server->kickClient(p.guid, reason);
}

static std::string scriptPlayer_getNickname(const ScriptPlayer& p)
{
    auto* c = p.server ? p.server->findClientByGuid(p.guid) : nullptr;
    return c ? c->nickname : "";
}

static void scriptPlayer_setNickname(const ScriptPlayer& p, const std::string& nick)
{
    if (p.server) p.server->setPlayerNickname(p.guid, nick);
}

static void scriptPlayer_setData(const ScriptPlayer& p,
                                  const std::string& key,
                                  const std::string& value)
{
    auto* c = p.server ? p.server->findClientByGuid(p.guid) : nullptr;
    if (c) c->scriptData[key] = value;
}

static sol::object scriptPlayer_getData(const ScriptPlayer& p,
                                         const std::string& key,
                                         sol::this_state ts)
{
    auto* c = p.server ? p.server->findClientByGuid(p.guid) : nullptr;
    if (c)
    {
        auto it = c->scriptData.find(key);
        if (it != c->scriptData.end())
            return sol::make_object(ts, it->second);
    }
    return sol::nil;
}

// ---------------------------------------------------------------------------
void registerPlayerBindings(sol::state& lua, MPServer* server)
{
    lua.new_usertype<ScriptPlayer>("Player",
        sol::no_constructor,

        "name",     sol::property(&scriptPlayer_getName),
        "guid",     sol::property(&scriptPlayer_getGuid),
        "cell",     sol::property(&scriptPlayer_getCell),
        "position", sol::property(&scriptPlayer_getPosition),

        "sendMessage", &scriptPlayer_sendMessage,
        "kick",        &scriptPlayer_kick,
        "setData",       &scriptPlayer_setData,
        "getData",       &scriptPlayer_getData,
        "getNickname",   &scriptPlayer_getNickname,
        "setNickname",   &scriptPlayer_setNickname
    );

    sol::table mp = lua["mp"].get_or_create<sol::table>();

    mp.set_function("getPlayer", [server](uint32_t guid) -> sol::optional<ScriptPlayer>
    {
        if (!server) return sol::nullopt;
        auto* c = server->findClientByGuid(guid);
        if (!c) return sol::nullopt;
        return ScriptPlayer{ guid, server };
    });

    mp.set_function("getPlayers", [server, &lua]() -> sol::table
    {
        sol::table t = lua.create_table();
        if (!server) return t;
        int i = 1;
        server->forEachPlayer([&](ConnectedClient& c)
        {
            t[i++] = ScriptPlayer{ c.guid, server };
        });
        return t;
    });
}

} // namespace mwmp
