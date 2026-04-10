#ifndef OPENMW_SERVER_OUTBOUNDQUEUE_HPP
#define OPENMW_SERVER_OUTBOUNDQUEUE_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <components/lua/serialization.hpp>
#include <components/openmw-mp/Packets/Lua/PacketLuaStorage.hpp>

namespace mwmp
{

enum class OutboundLuaActionType
{
    BroadcastServerMessage,
    BroadcastServerMessageToCell,
    SendServerMessage,
    RelayPlayerChat,
    KickClient,
    SetPlayerNickname,
    SetWorldHour,
    BroadcastLuaEvent,
    BroadcastLuaEventToCell,
    SendLuaEvent,
    BroadcastLuaStorage,
    SendLuaStorage,
};

struct OutboundLuaAction
{
    OutboundLuaActionType type = OutboundLuaActionType::BroadcastServerMessage;
    uint32_t guid = 0;
    float worldHour = 0.f;
    std::string text;
    std::string eventName;
    std::string cellId;
    LuaUtil::BinaryData eventData;
    LuaStorageAction storageAction = LuaStorageAction::Delta;
    std::string storageSection;
    std::vector<LuaStorageEntry> storageEntries;
};

class OutboundQueue
{
public:
    void push(OutboundLuaAction action)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mPending.push_back(std::move(action));
    }

    std::vector<OutboundLuaAction> takeAll()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<OutboundLuaAction> out;
        out.swap(mPending);
        return out;
    }

private:
    std::mutex mMutex;
    std::vector<OutboundLuaAction> mPending;
};

} // namespace mwmp

#endif // OPENMW_SERVER_OUTBOUNDQUEUE_HPP
