#include "ObjectSync.hpp"

#include <algorithm>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/openmw-mp/Packets/Object/PacketDoorState.hpp>
#include <components/esm3/loaddoor.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/soundmanager.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwworld/cellreflist.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/doorstate.hpp"
#include "../../mwworld/livecellref.hpp"
#include "../../mwworld/ptr.hpp"
#include "../../mwworld/scene.hpp"
#include "../../mwworld/worldimp.hpp"

#include "../network/Client.hpp"
#include "../Main.hpp"
#include "../sync/PlayerSync.hpp"

namespace mwmp
{

ObjectSync::ObjectSync(NetworkClient& client)
    : mClient(client)
{
}

// ---------------------------------------------------------------------------
void ObjectSync::onDoorStateChanged(const std::string& cellId,
                                    const std::string& refId,
                                    uint32_t           refNum,
                                    bool               isOpen,
                                    bool               isLocked,
                                    int                lockLevel)
{
    DoorEntry entry;
    entry.cellId    = cellId;
    entry.refId     = refId;
    entry.refNum    = refNum;
    entry.isOpen    = isOpen;
    entry.isLocked  = isLocked;
    entry.lockLevel = lockLevel;

    mOutgoingDoors.push_back({ cellId, std::move(entry) });
}

// ---------------------------------------------------------------------------
void ObjectSync::flushOutgoingDoorStates()
{
    if (mOutgoingDoors.empty())
        return;

    std::vector<OutgoingDoor> outgoing;
    outgoing.swap(mOutgoingDoors);

    for (const auto& door : outgoing)
    {
        PacketDoorState pkt;
        pkt.authorGuid = Main::get().getPlayerSync().localPlayer().guid;
        pkt.cellId = door.cellId;
        pkt.doors.push_back(door.entry);

        mClient.sendReliable(pkt.encode());
    }
}

// ---------------------------------------------------------------------------
bool ObjectSync::tryApplyDoorState(const std::string& refId,
                                   uint32_t           refNum,
                                   bool               isOpen)
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return false;

    const MWWorld::DoorState targetState = isOpen
        ? MWWorld::DoorState::Opening
        : MWWorld::DoorState::Closing;

    auto& scene = static_cast<MWWorld::World*>(world)->getWorldScene();
    for (MWWorld::CellStore* store : scene.getActiveCells())
    {
        for (const auto& liveRef : store->getReadOnlyDoors().mList)
        {
            if (liveRef.mRef.getRefId().toString() != refId) continue;
            if (refNum != 0 && liveRef.mRef.getRefNum().mIndex != refNum) continue;

            MWWorld::Ptr doorPtr(
                const_cast<MWWorld::LiveCellRefBase*>(
                    static_cast<const MWWorld::LiveCellRefBase*>(&liveRef)),
                store);

            if (MWBase::SoundManager* sound = MWBase::Environment::get().getSoundManager())
            {
                const ESM::RefId& soundId = isOpen ? liveRef.mBase->mOpenSound : liveRef.mBase->mCloseSound;
                if (!soundId.empty())
                    sound->playSound3D(doorPtr, soundId, 1.0f, 1.0f);
            }

            world->activateDoor(doorPtr, targetState);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
void ObjectSync::onServerDoorState(const std::string& cellId,
                                   const std::string& refId,
                                   uint32_t           refNum,
                                   bool               isOpen)
{
    if (!tryApplyDoorState(refId, refNum, isOpen))
    {
        // Cell not loaded yet (e.g. catch-up packet arrived during loading).
        // Queue for retry in update().
        Log(Debug::Verbose) << "[MP] ObjectSync: queuing door state for retry: " << refId;
        mPendingDoors.push_back({ cellId, refId, refNum, isOpen, 0.f });
    }
}

// ---------------------------------------------------------------------------
void ObjectSync::update(float dt)
{
    flushOutgoingDoorStates();

    if (mPendingDoors.empty()) return;

    static constexpr float RETRY_RATE = 0.2f; // retry every 200ms

    mPendingDoors.erase(
        std::remove_if(mPendingDoors.begin(), mPendingDoors.end(),
            [&](PendingDoor& pd) -> bool
            {
                pd.retryTimer += dt;
                if (pd.retryTimer < RETRY_RATE) return false;
                pd.retryTimer = 0.f;
                if (tryApplyDoorState(pd.refId, pd.refNum, pd.isOpen))
                    return true; // applied — remove from pending
                return false;   // still not found — keep retrying
            }),
        mPendingDoors.end());
}

} // namespace mwmp
