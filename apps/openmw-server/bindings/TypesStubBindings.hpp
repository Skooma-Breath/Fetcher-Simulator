#ifndef OPENMW_SERVER_TYPESSTUBBINDINGS_HPP
#define OPENMW_SERVER_TYPESSTUBBINDINGS_HPP

#include <sol/forward.hpp>

namespace LuaUtil { class LuaView; }

namespace mwmp
{
    sol::table initTypesStubPackage(LuaUtil::LuaView& view);
}

#endif // OPENMW_SERVER_TYPESSTUBBINDINGS_HPP
