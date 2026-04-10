#ifndef OPENMW_SERVER_PLAYERBINDINGS_HPP
#define OPENMW_SERVER_PLAYERBINDINGS_HPP

#include <cstdint>
#include <string>

#include <sol/forward.hpp>

#include "../LuaServerContext.hpp"

namespace LuaUtil { class LuaView; }
namespace mwmp { class LuaServerContext; }

namespace mwmp
{

// ---------------------------------------------------------------------------
// ScriptPlayer — lightweight handle passed to Lua callbacks.
// Holds the player's guid + a back-pointer to the server. All methods
// look the player up by guid at call time, so a stale reference (player
// disconnected between the callback fire and the script executing) silently
// does nothing rather than crashing.
// ---------------------------------------------------------------------------
struct ScriptPlayer
{
    LuaPlayerSnapshot data;
    LuaServerContext* context = nullptr;
};

// ---------------------------------------------------------------------------
// Registers the Player usertype and attaches player-related helpers to the
// provided mp package table.
// ---------------------------------------------------------------------------
void initPlayerBindings(LuaUtil::LuaView& view, sol::table& mp, LuaServerContext* context);

} // namespace mwmp

#endif // OPENMW_SERVER_PLAYERBINDINGS_HPP
