#ifndef OPENMW_SERVER_OUTBOUNDQUEUE_HPP
#define OPENMW_SERVER_OUTBOUNDQUEUE_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <components/lua/serialization.hpp>
#include <components/openmw-mp/Base/BaseStructs.hpp>
#include <components/openmw-mp/Base/DynamicRecord.hpp>
#include <components/openmw-mp/Packets/Lua/PacketLuaStorage.hpp>

#include "PlayerMark.hpp"

namespace mwmp
{

enum class OutboundLuaActionType
{
    BroadcastServerMessage,
    BroadcastServerMessageToCell,
    SendServerMessage,
    RelayPlayerChat,
    PlaceObject,
    SpawnActor,
    RemoveActor,
    TeleportPlayer,
    UpsertPlayerMark,
    DeletePlayerMark,
    KickClient,
    SetPlayerNickname,
    SetWorldHour,
    BroadcastLuaEvent,
    BroadcastLuaEventToCell,
    SendLuaEvent,
    BroadcastLuaStorage,
    SendLuaStorage,
    GrantInventoryItem,
    RemovePlacedObject,
    RemoveGameObject,
    ResetCellState,
    UpsertDynamicRecord,
    RemoveDynamicRecord,
    SetDynamicRecordDependencies,
    RefreshCellGameSettings,
    RefreshPlayerGameSettings,
    RefreshAllGameSettings,
};

struct OutboundLuaAction
{
    OutboundLuaActionType type = OutboundLuaActionType::BroadcastServerMessage;
    uint32_t guid = 0;
    uint32_t mpNum = 0;
    float worldHour = 0.f;
    int itemCount = 0;
    bool recordPersistent = true;
    bool actorPersistent = true;
    uint32_t actorRefNum = 0;
    uint32_t actorMpNum = 0;
    Position position;
    PlayerMark playerMark;
    std::string text;
    std::string eventName;
    std::string cellId;
    std::string recordType;
    std::string recordId;
    std::string recordScope;
    std::vector<std::string> dependencyRecordIds;
    LuaUtil::BinaryData eventData;
    LuaUtil::BinaryData recordData;
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
