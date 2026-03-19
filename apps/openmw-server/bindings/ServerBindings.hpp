#ifndef OPENMW_SERVER_SERVERBINDINGS_HPP
#define OPENMW_SERVER_SERVERBINDINGS_HPP

namespace sol  { class state; }
namespace mwmp { class MPServer; }

namespace mwmp
{
    // Registers the mp.* server-level functions into the Lua state.
    // Called once from ScriptEngine::registerCoreBindings().
    void registerServerBindings(sol::state& lua, MPServer* server);
}

#endif // OPENMW_SERVER_SERVERBINDINGS_HPP
