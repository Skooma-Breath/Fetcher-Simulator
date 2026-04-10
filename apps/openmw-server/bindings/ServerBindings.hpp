#ifndef OPENMW_SERVER_SERVERBINDINGS_HPP
#define OPENMW_SERVER_SERVERBINDINGS_HPP

#include <sol/forward.hpp>

namespace LuaUtil
{
    class LuaStorage;
    class LuaView;
}
namespace mwmp { class LuaServerContext; }

namespace mwmp
{
    // Builds the mp package for server-side Lua sandboxes.
    sol::table initMpPackage(LuaUtil::LuaView& view, LuaServerContext* context, LuaUtil::LuaStorage* storage);
}

#endif // OPENMW_SERVER_SERVERBINDINGS_HPP
