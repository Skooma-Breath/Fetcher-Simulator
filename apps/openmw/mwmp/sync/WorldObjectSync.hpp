#ifndef OPENMW_MWMP_SYNC_WORLDOBJECTSYNC_HPP
#define OPENMW_MWMP_SYNC_WORLDOBJECTSYNC_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <components/openmw-mp/Base/BaseObject.hpp>
#include <components/openmw-mp/Base/BaseStructs.hpp>
#include "../../mwworld/ptr.hpp"

namespace mwmp
{
    class NetworkClient;

    // -----------------------------------------------------------------------
    // WorldObjectSync — client-side registry for MP-placed world objects
    // and server-authoritative container inventories.
    //
    // Responsibilities:
    //   - Map mpNum → MWWorld::Ptr so ObjectMove/Delete can find objects fast.
    //   - Send ObjectPlace when the local player drops an item / places an object.
    //   - Apply incoming ObjectPlace / ObjectDelete / ObjectMove from server.
    //   - Send Container (Set) when player opens a container.
    //   - Apply incoming Container (Set/Add/Remove) to the local container UI.
    //   - Queue operations that arrive before the cell is loaded, retry in update().
    // -----------------------------------------------------------------------
    class WorldObjectSync
    {
    public:
        explicit WorldObjectSync(NetworkClient& client);

        // --- called from Main.cpp frame loop ---
        void update(float dt);

        // --- outbound: local player actions ---
        // Called when the local player drops/places an object in the world.
        void onLocalObjectPlaced(const MWWorld::Ptr& ptr, const std::string& refId, int count,
                                 const Position& pos, const std::string& cellId);

        // Called when the local player picks up or otherwise deletes an MP-placed object.
        void onLocalObjectDeleted(const MWWorld::Ptr& ptr);

        // Marks an MP world instance as being transferred into the returned
        // inventory Ptr. Its mpNum becomes the stack's stable instanceId.
        void onLocalObjectTaken(const MWWorld::Ptr& worldObject, const MWWorld::Ptr& inventoryObject);

        // Lua split() creates a disabled detached world Ptr before a later
        // moveInto() or teleport(). Preserve whether that detached object came
        // from the local player's inventory so a later world placement can use
        // the normal ObjectPlace route without requiring a mod-specific patch.
        void markLocalPlayerInventoryDetached(const MWWorld::Ptr& ptr);
        bool consumeLocalPlayerInventoryDetached(const MWWorld::Ptr& ptr);
        void forgetLocalPlayerInventoryDetached(const MWWorld::Ptr& ptr);

        // Called when the local player clicks the vanilla Dispose of Corpse button
        // for a dead actor corpse. Sends a dedicated CorpseDispose packet.
        void onLocalCorpseDisposed(const MWWorld::Ptr& ptr);

        // Suppress local-delete hooks during remote/server-applied deletions
        // so they are not echoed back as disposal requests.
        void setSuppressLocalDelete(bool suppress) { mSuppressLocalDelete = suppress; }
        bool isSuppressLocalDelete() const { return mSuppressLocalDelete; }

        // Called when the local player opens a container.
        // Sends action=Set with its current contents so the server can take authority.
        void onLocalContainerOpened(const std::string& cellId,
                                    const std::string& refId, uint32_t refNum,
                                    uint32_t mpNum);

        // Called when the local player modifies container contents (take/add).
        void onLocalContainerChanged(const std::string& cellId,
                                     const std::string& refId, uint32_t refNum, uint32_t mpNum,
                                     ContainerAction action,
                                     const std::vector<ContainerItem>& items);

        // --- inbound: packets from server ---
        void onServerObjectPlace (uint32_t mpNum, const std::string& refId,
                                  int count, const Position& pos,
                                  const std::string& cellId);

        void onServerObjectDelete(uint32_t mpNum, const std::string& cellId);

        void onServerObjectMove  (uint32_t mpNum, const std::string& cellId,
                                  const Position& pos);

        void onServerContainer   (const ContainerRecord& record, ContainerAction action);

        // --- lookup ---
        MWWorld::Ptr getObjectByMpNum(uint32_t mpNum) const;
        uint32_t getMpNumForObject(const MWWorld::Ptr& ptr) const;

    private:
        // ---- world helpers ----
        bool tryPlaceObject (uint32_t mpNum, const std::string& refId,
                             int count, const Position& pos,
                             const std::string& cellId);
        bool tryDeleteObject(uint32_t mpNum);
        bool tryMoveObject  (uint32_t mpNum, const Position& pos);
        bool tryApplyContainer(const ContainerRecord& record, ContainerAction action);
        void registerObject(uint32_t mpNum, const MWWorld::Ptr& ptr);
        void unregisterObject(uint32_t mpNum);

        NetworkClient& mClient;

        // mpNum → live world Ptr
        std::unordered_map<uint32_t, MWWorld::Ptr> mObjects;
        std::unordered_map<ESM::RefNum, uint32_t> mMpNumsByObjectId;

        // Pending operations that arrived before the target cell was loaded
        struct PendingPlace  { uint32_t mpNum; std::string refId; int count;
                               Position pos; std::string cellId; float timer; };
        struct PendingLocalPlace { MWWorld::Ptr ptr; std::string refId; int count; Position pos; std::string cellId; };
        struct PendingDelete { uint32_t mpNum; float timer; };
        struct PendingMove   { uint32_t mpNum; Position pos; float timer; };
        struct PendingContainer { ContainerRecord record; ContainerAction action; float timer; };

        std::vector<PendingPlace>     mPendingPlace;
        std::vector<PendingLocalPlace> mPendingLocalPlace;
        std::vector<PendingDelete>    mPendingDelete;
        std::vector<PendingMove>      mPendingMove;
        std::vector<PendingContainer> mPendingContainer;
        bool mSuppressLocalDelete = false;
        std::unordered_set<uint32_t> mPendingTakenMpNums;
        std::unordered_set<ESM::RefNum> mLocalPlayerInventoryDetached;

        static constexpr float RETRY_RATE = 0.25f;
    };

    // RAII guard that saves and restores the suppression state so it is
    // exception-safe and nesting-safe across multiple remote deletions.
    class ScopedLocalDeleteSuppression
    {
    public:
        explicit ScopedLocalDeleteSuppression(WorldObjectSync& sync)
            : mSync(sync), mPrevious(sync.isSuppressLocalDelete())
        {
            mSync.setSuppressLocalDelete(true);
        }
        ~ScopedLocalDeleteSuppression() { mSync.setSuppressLocalDelete(mPrevious); }
        ScopedLocalDeleteSuppression(const ScopedLocalDeleteSuppression&) = delete;
        ScopedLocalDeleteSuppression& operator=(const ScopedLocalDeleteSuppression&) = delete;

    private:
        WorldObjectSync& mSync;
        bool mPrevious;
    };

} // namespace mwmp

#endif // OPENMW_MWMP_SYNC_WORLDOBJECTSYNC_HPP
