#include "WorldObjectSync.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm/position.hpp>
#include <components/openmw-mp/Packets/Object/PacketObjectPlace.hpp>
#include <components/openmw-mp/Packets/Object/PacketObjectDelete.hpp>
#include <components/openmw-mp/Packets/Object/PacketObjectMove.hpp>
#include <components/openmw-mp/Packets/Object/PacketContainer.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwworld/class.hpp"
#include "../../mwworld/ptr.hpp"
#include "../../mwworld/manualref.hpp"
#include "../../mwworld/esmstore.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/scene.hpp"
#include "../../mwworld/worldimp.hpp"
#include "../../mwworld/inventorystore.hpp"
#include "../../mwworld/containerstore.hpp"

#include "../network/Client.hpp"
#include "../Main.hpp"
#include "PlayerSync.hpp"

namespace mwmp
{

namespace
{
    bool samePosition(const Position& left, const Position& right)
    {
        constexpr float epsilon = 0.01f;
        for (int i = 0; i < 3; ++i)
        {
            if (std::abs(left.pos[i] - right.pos[i]) > epsilon)
                return false;
            if (std::abs(left.rot[i] - right.rot[i]) > epsilon)
                return false;
        }
        return true;
    }

    MWWorld::CellStore* findActiveCellById(MWWorld::World& world, const std::string& cellId)
    {
        auto& scene = world.getWorldScene();
        for (MWWorld::CellStore* store : scene.getActiveCells())
        {
            if (store == nullptr)
                continue;

            const MWWorld::Cell* cell = store->getCell();
            if (cell == nullptr)
                continue;

            if (cellId.rfind("EXT:", 0) == 0)
            {
                int gridX = 0;
                int gridY = 0;
                if (std::sscanf(cellId.c_str(), "EXT:%d,%d", &gridX, &gridY) != 2)
                    return nullptr;

                if (cell->isExterior() && cell->getGridX() == gridX && cell->getGridY() == gridY)
                    return store;
            }
            else if (!cell->isExterior() && std::string(cell->getNameId()) == cellId)
            {
                return store;
            }
        }

        return nullptr;
    }
}

WorldObjectSync::WorldObjectSync(NetworkClient& client)
    : mClient(client)
{
}

// ---------------------------------------------------------------------------
// update — retry queued operations that failed because the cell wasn't loaded
// ---------------------------------------------------------------------------
void WorldObjectSync::update(float dt)
{
    // --- pending places ---
    mPendingPlace.erase(
        std::remove_if(mPendingPlace.begin(), mPendingPlace.end(),
            [&](PendingPlace& p) -> bool {
                p.timer += dt;
                if (p.timer < RETRY_RATE) return false;
                p.timer = 0.f;
                return tryPlaceObject(p.mpNum, p.refId, p.count, p.pos, p.cellId);
            }),
        mPendingPlace.end());

    // --- pending deletes ---
    mPendingDelete.erase(
        std::remove_if(mPendingDelete.begin(), mPendingDelete.end(),
            [&](PendingDelete& p) -> bool {
                p.timer += dt;
                if (p.timer < RETRY_RATE) return false;
                p.timer = 0.f;
                return tryDeleteObject(p.mpNum);
            }),
        mPendingDelete.end());

    // --- pending moves ---
    mPendingMove.erase(
        std::remove_if(mPendingMove.begin(), mPendingMove.end(),
            [&](PendingMove& p) -> bool {
                p.timer += dt;
                if (p.timer < RETRY_RATE) return false;
                p.timer = 0.f;
                return tryMoveObject(p.mpNum, p.pos);
            }),
        mPendingMove.end());

    // --- pending container updates ---
    mPendingContainer.erase(
        std::remove_if(mPendingContainer.begin(), mPendingContainer.end(),
            [&](PendingContainer& p) -> bool {
                p.timer += dt;
                if (p.timer < RETRY_RATE) return false;
                p.timer = 0.f;
                return tryApplyContainer(p.record, p.action);
            }),
        mPendingContainer.end());
}

// ---------------------------------------------------------------------------
// Outbound — local player places an object
// ---------------------------------------------------------------------------
void WorldObjectSync::onLocalObjectPlaced(const MWWorld::Ptr& ptr, const std::string& refId, int count,
                                          const Position& pos,
                                          const std::string& cellId)
{
    PacketObjectPlace pkt;
    pkt.object.mpNum   = 0;         // server assigns
    pkt.object.refId   = refId;
    pkt.object.count   = count;
    pkt.object.position= pos;
    pkt.object.cellId  = cellId;
    mPendingLocalPlace.push_back({ptr, refId, count, pos, cellId});
    mClient.sendReliable(pkt.encode());
    Log(Debug::Verbose) << "[MP] WorldObjectSync: sent ObjectPlace refId=" << refId;
}

void WorldObjectSync::onLocalObjectDeleted(const MWWorld::Ptr& ptr)
{
    if (mSuppressLocalDelete || ptr.isEmpty() || !ptr.isInCell())
        return;

    uint32_t mpNum = 0;
    for (const auto& [candidateMpNum, objectPtr] : mObjects)
    {
        if (objectPtr == ptr)
        {
            mpNum = candidateMpNum;
            break;
        }
    }

    if (mpNum == 0)
        return;

    std::string cellId;
    if (const MWWorld::Cell* cell = ptr.getCell()->getCell())
    {
        if (cell->isExterior())
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "EXT:%d,%d", cell->getGridX(), cell->getGridY());
            cellId = buf;
        }
        else
            cellId = std::string(cell->getNameId());
    }

    PacketObjectDelete pkt;
    pkt.mpNum = mpNum;
    pkt.cellId = cellId;
    mClient.sendReliable(pkt.encode());
    Log(Debug::Verbose) << "[MP] WorldObjectSync: sent ObjectDelete mpNum=" << mpNum;
}

// ---------------------------------------------------------------------------
// Outbound — local player opens a container
// ---------------------------------------------------------------------------
void WorldObjectSync::onLocalContainerOpened(const std::string& cellId,
                                              const std::string& refId,
                                              uint32_t refNum, uint32_t mpNum)
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return;

    // Build a Set packet with the container's current full contents.
    PacketContainer pkt;
    pkt.container.cellId = cellId;
    pkt.container.refId  = refId;
    pkt.container.refNum = refNum;
    pkt.container.mpNum  = mpNum;
    pkt.mAction = static_cast<uint8_t>(ContainerAction::Set);

    // Walk active cells to find the container and snapshot its inventory.
    auto& scene = static_cast<MWWorld::World*>(world)->getWorldScene();
    for (MWWorld::CellStore* store : scene.getActiveCells())
    {
        bool found = false;
        store->forEach([&](MWWorld::Ptr ptr) -> bool
        {
            if (ptr.getType() != ESM::Container::sRecordId)
                return true;
            if (ptr.getCellRef().getRefId().toString() != refId)
                return true;
            if (refNum != 0 && ptr.getCellRef().getRefNum().mIndex != refNum)
                return true;

            auto& cstore = ptr.getClass().getContainerStore(ptr);
            for (auto it = cstore.begin(); it != cstore.end(); ++it)
            {
                ContainerItem ci;
                ci.refId  = it->getCellRef().getRefId().toString();
                ci.count  = it->getCellRef().getCount();
                ci.charge = static_cast<int>(it->getCellRef().getCharge());
                pkt.container.items.push_back(ci);
            }
            found = true;
            return false;
        });

        if (found)
            break;
    }

    mClient.sendReliable(pkt.encode());
    Log(Debug::Verbose) << "[MP] WorldObjectSync: sent Container(Set) refId=" << refId;
}

// ---------------------------------------------------------------------------
// Outbound — local player modifies container contents
// ---------------------------------------------------------------------------
void WorldObjectSync::onLocalContainerChanged(const std::string& cellId,
                                               const std::string& refId,
                                               uint32_t refNum,
                                               ContainerAction action,
                                               const std::vector<ContainerItem>& items)
{
    PacketContainer pkt;
    pkt.container.cellId  = cellId;
    pkt.container.refId   = refId;
    pkt.container.refNum  = refNum;
    pkt.container.items   = items;
    pkt.mAction = static_cast<uint8_t>(action);
    mClient.sendReliable(pkt.encode());
}

// ---------------------------------------------------------------------------
// Inbound — server tells us to place an object
// ---------------------------------------------------------------------------
void WorldObjectSync::onServerObjectPlace(uint32_t mpNum, const std::string& refId,
                                           int count, const Position& pos,
                                           const std::string& cellId)
{
    auto localIt = std::find_if(
        mPendingLocalPlace.begin(), mPendingLocalPlace.end(),
        [&](const PendingLocalPlace& pending)
        {
            return !pending.ptr.isEmpty()
                && pending.refId == refId
                && pending.count == count
                && pending.cellId == cellId
                && samePosition(pending.pos, pos);
        });
    if (localIt != mPendingLocalPlace.end())
    {
        mObjects[mpNum] = localIt->ptr;
        mPendingLocalPlace.erase(localIt);
        Log(Debug::Verbose) << "[MP] WorldObjectSync: registered local ObjectPlace mpNum=" << mpNum;
        return;
    }

    if (!tryPlaceObject(mpNum, refId, count, pos, cellId))
    {
        Log(Debug::Verbose) << "[MP] WorldObjectSync: queuing ObjectPlace mpNum=" << mpNum
                            << " refId=" << refId;
        mPendingPlace.push_back({mpNum, refId, count, pos, cellId, 0.f});
    }
}

// ---------------------------------------------------------------------------
void WorldObjectSync::onServerObjectDelete(uint32_t mpNum, const std::string& /*cellId*/)
{
    if (!tryDeleteObject(mpNum))
    {
        Log(Debug::Verbose) << "[MP] WorldObjectSync: queuing ObjectDelete mpNum=" << mpNum;
        mPendingDelete.push_back({mpNum, 0.f});
    }
}

// ---------------------------------------------------------------------------
void WorldObjectSync::onServerObjectMove(uint32_t mpNum, const std::string& /*cellId*/,
                                          const Position& pos)
{
    if (!tryMoveObject(mpNum, pos))
        mPendingMove.push_back({mpNum, pos, 0.f});
}

// ---------------------------------------------------------------------------
void WorldObjectSync::onServerContainer(const ContainerRecord& record, ContainerAction action)
{
    if (!tryApplyContainer(record, action))
    {
        Log(Debug::Verbose) << "[MP] WorldObjectSync: queuing Container refId=" << record.refId;
        mPendingContainer.push_back({record, action, 0.f});
    }
}

// ---------------------------------------------------------------------------
MWWorld::Ptr WorldObjectSync::getObjectByMpNum(uint32_t mpNum) const
{
    auto it = mObjects.find(mpNum);
    return (it != mObjects.end()) ? it->second : MWWorld::Ptr();
}

// ---------------------------------------------------------------------------
// tryPlaceObject — attempt to spawn the object in the world right now.
// Returns true on success (object is now registered in mObjects).
// ---------------------------------------------------------------------------
bool WorldObjectSync::tryPlaceObject(uint32_t mpNum, const std::string& refId,
                                      int count, const Position& pos,
                                      const std::string& cellId)
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return false;

    // Already placed (duplicate packet)
    if (mObjects.count(mpNum)) return true;

    // Find the ESM record
    const MWWorld::ESMStore& store = world->getStore();
    MWWorld::ManualRef ref(store, ESM::RefId::stringRefId(refId), count);
    if (ref.getPtr().isEmpty())
    {
        Log(Debug::Warning) << "[MP] WorldObjectSync: unknown refId '" << refId << "'";
        return false;
    }

    // Place into the target active cell from the packet. This matters for
    // exterior grids where the observer's current cell may differ from the
    // object's actual destination cell.
    auto* worldImpl = static_cast<MWWorld::World*>(world);
    MWWorld::CellStore* cell = findActiveCellById(*worldImpl, cellId);
    if (!cell) return false;

    ESM::Position esmPos;
    for (int i = 0; i < 3; ++i) esmPos.pos[i] = pos.pos[i];
    for (int i = 0; i < 3; ++i) esmPos.rot[i] = pos.rot[i];

    MWWorld::Ptr placed = world->placeObject(ref.getPtr(), cell, esmPos);
    if (placed.isEmpty())
    {
        Log(Debug::Warning) << "[MP] WorldObjectSync: placeObject failed for " << refId;
        return false;
    }

    mObjects[mpNum] = placed;
    Log(Debug::Info) << "[MP] WorldObjectSync: placed refId=" << refId
                     << " mpNum=" << mpNum;
    return true;
}

// ---------------------------------------------------------------------------
bool WorldObjectSync::tryDeleteObject(uint32_t mpNum)
{
    auto it = mObjects.find(mpNum);
    if (it == mObjects.end()) return false;   // not yet placed — keep pending

    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return false;

    if (!it->second.isEmpty())
    {
        mSuppressLocalDelete = true;
        world->deleteObject(it->second);
        mSuppressLocalDelete = false;
        Log(Debug::Info) << "[MP] WorldObjectSync: deleted mpNum=" << mpNum;
    }
    mObjects.erase(it);
    return true;
}

// ---------------------------------------------------------------------------
bool WorldObjectSync::tryMoveObject(uint32_t mpNum, const Position& pos)
{
    auto it = mObjects.find(mpNum);
    if (it == mObjects.end() || it->second.isEmpty()) return false;

    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return false;

    osg::Vec3f osgPos(pos.pos[0], pos.pos[1], pos.pos[2]);
    osg::Vec3f osgRot(pos.rot[0], pos.rot[1], pos.rot[2]);

    it->second = world->moveObject(it->second, osgPos);
    world->rotateObject(it->second, osgRot);
    return true;
}

// ---------------------------------------------------------------------------
bool WorldObjectSync::tryApplyContainer(const ContainerRecord& record, ContainerAction action)
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return false;

    auto& scene = static_cast<MWWorld::World*>(world)->getWorldScene();
    for (MWWorld::CellStore* store : scene.getActiveCells())
    {
        MWWorld::Ptr target;
        store->forEach([&](MWWorld::Ptr ptr) -> bool
        {
            if (ptr.getType() != ESM::Container::sRecordId)
                return true;
            if (ptr.getCellRef().getRefId().toString() != record.refId)
                return true;
            if (record.refNum != 0 && ptr.getCellRef().getRefNum().mIndex != record.refNum)
                return true;

            target = ptr;
            return false;
        });

        if (target.isEmpty())
            continue;

        auto& cstore = target.getClass().getContainerStore(target);
        const MWWorld::ESMStore& esmStore = world->getStore();

        if (action == ContainerAction::Set)
        {
            cstore.clear();
            for (const auto& ci : record.items)
            {
                MWWorld::ManualRef ref(esmStore, ESM::RefId::stringRefId(ci.refId), ci.count);
                if (!ref.getPtr().isEmpty())
                    cstore.add(ref.getPtr(), ci.count);
            }
        }
        else if (action == ContainerAction::Add)
        {
            for (const auto& ci : record.items)
            {
                MWWorld::ManualRef ref(esmStore, ESM::RefId::stringRefId(ci.refId), ci.count);
                if (!ref.getPtr().isEmpty())
                    cstore.add(ref.getPtr(), ci.count);
            }
        }
        else if (action == ContainerAction::Remove)
        {
            for (const auto& ci : record.items)
                cstore.remove(ESM::RefId::stringRefId(ci.refId), ci.count);
        }

        Log(Debug::Info) << "[MP] WorldObjectSync: applied Container action="
                         << static_cast<int>(action)
                         << " refId=" << record.refId;
        return true;
    }
    return false;
}

} // namespace mwmp
