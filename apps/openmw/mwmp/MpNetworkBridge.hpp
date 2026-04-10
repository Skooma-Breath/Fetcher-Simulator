#ifndef OPENMW_MWMP_MPNETWORKBRIDGE_HPP
#define OPENMW_MWMP_MPNETWORKBRIDGE_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <components/lua/serialization.hpp>
#include <components/openmw-mp/Packets/Lua/PacketLuaStorage.hpp>
#include <sol/forward.hpp>

namespace MWLua
{
    struct Context;
}

namespace MWBase
{
    class LuaManager;
}

namespace mwmp
{
    class NetworkClient;

    class MpNetworkBridge
    {
    public:
        struct LuaEvent
        {
            uint32_t pid = 0;
            std::string eventName;
            LuaUtil::BinaryData eventData;
        };

        void queueInbound(LuaEvent event);
        void queueStorage(LuaStorageAction action, std::string section, std::vector<LuaStorageEntry> entries);
        void queueOutbound(std::string eventName, LuaUtil::BinaryData eventData);
        void processIncoming(MWBase::LuaManager& luaManager);
        void drainOutgoing(NetworkClient& client);

    private:
        struct LuaStorageUpdate
        {
            LuaStorageAction action = LuaStorageAction::Delta;
            std::string section;
            std::vector<LuaStorageEntry> entries;
        };

        std::mutex mInboundMutex;
        std::mutex mStorageMutex;
        std::mutex mOutboundMutex;
        std::vector<LuaEvent> mInboundEvents;
        std::vector<LuaStorageUpdate> mStorageUpdates;
        std::vector<LuaEvent> mOutboundEvents;
    };

    sol::table initClientMpPackage(const MWLua::Context& context);
}

#endif // OPENMW_MWMP_MPNETWORKBRIDGE_HPP
