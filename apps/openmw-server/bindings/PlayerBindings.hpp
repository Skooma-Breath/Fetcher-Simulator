#ifndef OPENMW_SERVER_PLAYERBINDINGS_HPP
#define OPENMW_SERVER_PLAYERBINDINGS_HPP

#include <string>

namespace sol  { class state; }
namespace mwmp { class MPServer; }

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
    uint32_t  guid   = 0;
    MPServer* server = nullptr;
};

// ---------------------------------------------------------------------------
// Registers the Player usertype and mp.getPlayer() / mp.getPlayers()
// into the Lua state. Called once from ScriptEngine::registerCoreBindings().
// ---------------------------------------------------------------------------
void registerPlayerBindings(sol::state& lua, MPServer* server);

} // namespace mwmp

#endif // OPENMW_SERVER_PLAYERBINDINGS_HPP
