#include "PlayerBindings.hpp"

#include <components/lua/luastate.hpp>
#include <sol/sol.hpp>

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
    return p.data.name;
}

static std::string scriptPlayer_getCell(const ScriptPlayer& p)
{
    return p.data.cell;
}

static uint32_t scriptPlayer_getGuid(const ScriptPlayer& p)
{
    return p.data.guid;
}

static sol::table scriptPlayer_getPosition(const ScriptPlayer& p, sol::this_state ts)
{
    sol::state_view lua(ts);
    sol::table t = lua.create_table();
    t["x"] = p.data.x;
    t["y"] = p.data.y;
    t["z"] = p.data.z;
    t["rx"] = p.data.rx;
    t["ry"] = p.data.ry;
    t["rz"] = p.data.rz;
    return t;
}

static void scriptPlayer_sendMessage(const ScriptPlayer& p, const std::string& text)
{
    if (p.context) p.context->queueSendServerMessage(p.data.guid, text);
}

static void scriptPlayer_kick(const ScriptPlayer& p, const std::string& reason)
{
    if (p.context) p.context->queueKickClient(p.data.guid, reason);
}

static std::string scriptPlayer_getNickname(const ScriptPlayer& p)
{
    return p.data.nickname;
}

static std::string scriptPlayer_getRace(const ScriptPlayer& p)
{
    return p.data.race;
}

static bool scriptPlayer_getIsMale(const ScriptPlayer& p)
{
    return p.data.isMale;
}

static int scriptPlayer_getGender(const ScriptPlayer& p)
{
    return p.data.isMale ? 1 : 0;
}

static void scriptPlayer_setNickname(const ScriptPlayer& p, const std::string& nick)
{
    if (p.context) p.context->queueSetPlayerNickname(p.data.guid, nick);
}

static void scriptPlayer_setData(const ScriptPlayer& p,
                                  const std::string& key,
                                  const std::string& value)
{
    if (p.context) p.context->setPlayerData(p.data.guid, key, value);
}

static sol::object scriptPlayer_getData(const ScriptPlayer& p,
                                         const std::string& key,
                                         sol::this_state ts)
{
    if (p.context)
    {
        auto value = p.context->getPlayerData(p.data.guid, key);
        if (value)
            return sol::make_object(ts, *value);
    }
    return sol::nil;
}

// ---------------------------------------------------------------------------
void initPlayerBindings(LuaUtil::LuaView& view, sol::table& mp, LuaServerContext* context)
{
    sol::state_view& lua = view.sol();

    lua.new_usertype<ScriptPlayer>("Player",
        sol::no_constructor,

        "name",     sol::property(&scriptPlayer_getName),
        "guid",     sol::property(&scriptPlayer_getGuid),
        "cell",     sol::property(&scriptPlayer_getCell),
        "position", sol::property(&scriptPlayer_getPosition),
        "race",     sol::property(&scriptPlayer_getRace),
        "isMale",   sol::property(&scriptPlayer_getIsMale),
        "gender",   sol::property(&scriptPlayer_getGender),

        "sendMessage", &scriptPlayer_sendMessage,
        "kick",        &scriptPlayer_kick,
        "setData",       &scriptPlayer_setData,
        "getData",       &scriptPlayer_getData,
        "getNickname",   &scriptPlayer_getNickname,
        "setNickname",   &scriptPlayer_setNickname
    );

    mp.set_function("getPlayer", [context](uint32_t guid) -> sol::optional<ScriptPlayer>
    {
        if (!context) return sol::nullopt;
        auto player = context->getPlayer(guid);
        if (!player) return sol::nullopt;
        return ScriptPlayer{ std::move(*player), context };
    });

    mp.set_function("getPlayers", [context](sol::this_state ts) -> sol::table
    {
        sol::state_view lua(ts);
        sol::table t = lua.create_table();
        if (!context) return t;
        int i = 1;
        for (auto& player : context->getPlayers())
        {
            t[i++] = ScriptPlayer{ std::move(player), context };
        }
        return t;
    });
}

} // namespace mwmp
