#ifndef OPENMW_MWLUA_MUTATIONAUDIT_H
#define OPENMW_MWLUA_MUTATIONAUDIT_H

#include <string_view>

namespace MWWorld
{
    class Ptr;
}

namespace MWLua
{
    struct Context;

    // Records native Lua mutations that may need a multiplayer replication
    // adapter. It deliberately observes only; it never changes or blocks the
    // operation.
    void auditNativeMutation(const Context& context, std::string_view operation, const MWWorld::Ptr& target,
        std::string_view detail = {});
    void auditNativeMutation(const Context& context, std::string_view operation, std::string_view target,
        std::string_view detail = {});
}

#endif
