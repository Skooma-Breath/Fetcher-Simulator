#ifndef OPENMW_MWMP_SYNC_ACTORSYNC_HPP
#define OPENMW_MWMP_SYNC_ACTORSYNC_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <osg/Vec3f>
#include <components/esm3/aisequence.hpp>
#include <components/openmw-mp/Base/ActorSyncProtocol.hpp>
#include <components/openmw-mp/Base/BaseActor.hpp>

#include "../../mwworld/ptr.hpp"

namespace MWBase
{
    class World;
}

namespace mwmp
{
    class NetworkClient;

    // Manages authority and synchronisation of NPC/creature actors.
    // Full implementation in Phase 4 (actor sync).
    class ActorSync
    {
    public:
        explicit ActorSync(NetworkClient& client);

        void update(float dt);
        // Called after World::update so actors inserted while loading a cell
        // can receive bootstrap presentation before the first render traversal.
        void updateLoadedCellBootstrapVisuals();

        // Must be called when the local player fully disconnects from a server
        // (e.g. returns to main menu).  Clears all per-session cell/actor
        // tracking so stale Ptr references from the previous game session are
        // never dereferenced after the world has been torn down.
        void resetSessionState();

        void onAuthorityUpdate(const ActorList& list);

        void onActorIdentityUpdate(const ActorIdentityList& list);
        void onActorListUpdate(const ActorList& list);
        void onActorPositionUpdate(const ActorList& list);
        void onActorPositionV2Update(const ActorPositionV2List& list);
        void onActorPresentationV2Update(const ActorPresentationV2List& list);
        void onActorAnimFlagsUpdate(const ActorList& list);
        void onActorAnimPlay(const ActorList& list);
        void onActorAttack(const ActorList& list);
        void onActorAttackV2(const ActorAttackV2List& list);
        void onActorCast(const ActorList& list);
        void onActorDeath(const ActorList& list);
        void onActorEquipment(const ActorList& list);
        void onActorStatsDynamic(const ActorList& list);
        void onActorAI(const ActorList& list);
        void onActorCombatRequest(const ActorList& list);
        void onActorCellChange(const ActorList& list);

        bool hasAuthority(const std::string& cellId) const;
        bool hasAuthorityForMpNum(uint32_t mpNum, const std::string& cellId) const;
        bool hasAuthorityForObject(const MWWorld::Ptr& ptr) const;
        std::string getActorAuthorityCellId(const MWWorld::Ptr& ptr) const;
        uint32_t getActorMpNum(const MWWorld::Ptr& ptr) const;
        uint32_t getActorCanonicalRefNum(const MWWorld::Ptr& ptr) const;
        MWWorld::Ptr getActorByCanonicalRefNum(uint32_t refNum) const;
        MWWorld::Ptr getActorByMpNum(uint32_t mpNum) const;
        void sendCombatRequest(const MWWorld::Ptr& victim, float damage, bool healthDamage, bool knocked,
            const osg::Vec3f& hitPos, int attackType, float attackStrength);
        void sendNpcPlayerDamage(uint32_t victimGuid, float damage, bool healthDamage, bool isDead, int attackType,
            const MWWorld::Ptr& npcAttacker);
        void notifyNpcCast(const MWWorld::Ptr& npc, const std::string& spellId, const std::string& castAnim, const MWWorld::Ptr& target, bool release);

    private:
        struct BufferedSnapshot
        {
            Position position;
            Velocity velocity;
            uint32_t sequence = 0;
            uint64_t serverTimestamp = 0;
            bool isMoving = false;
            bool isAttackingOrCasting = false;
            bool hasWeaponDrawn = false;
            bool hasSpellReadied = false;
            uint32_t movementFlags = 0;
            float animFwd = 0.f;
            float animSide = 0.f;
            std::string currentAnimGroup;
            float currentAnimCompletion = -1.f;
        };

        struct ActorRuntime
        {
            BaseActor state;
            Position smoothedPosition;
            bool hasSmoothedPosition = false;
            Position stationaryHoldPosition;
            bool hasStationaryHoldPosition = false;
            Position stationaryReleasePosition;
            float stationaryReleaseTimer = 0.f;
            std::deque<BufferedSnapshot> snapshots;
            double interpolationRenderTimestamp = 0.0;
            float latestSnapshotAge = 0.f;
            uint64_t lastServerTimestamp = 0;
            uint64_t lastPresentationServerTimestamp = 0;
            uint64_t lastClientSnapshotReceiveTimeMs = 0;
            bool hasInterpolationRenderTimestamp = false;
            MWWorld::Ptr boundActor;
            bool pendingAnimPlay = false;
            bool pendingAttack = false;
            bool pendingCast = false;
            // Set to true when a cast-start (release=false) packet is consumed so
            // the cast-release handler knows the wind-up animation is already playing
            // and the bolt should fire immediately rather than after another countdown.
            bool castStartReceived = false;
            bool lastAttackingOrCasting = false;
            bool lastWeaponVisible = false;
            // Track whether death has already been applied to this actor so that
            // re-entering a cell with dead actors skips to the final death pose
            // instead of replaying the death animation from scratch.
            bool deathAlreadyApplied = false;
            // A bootstrapped corpse is hidden until the frame after its final
            // death pose is installed, so the bind pose cannot flash on the
            // first rendered cell frame.
            uint64_t bootstrapDeathRevealUpdate = 0;
            // When true, the death came from a real-time ActorDeath packet (not an
            // ActorList load), so the death animation should play from the start.
            bool deathFromRealtimePacket = false;
            // Set once this runtime has received a live state in the current
            // session. Used to distinguish a fresh death transition from a dead
            // actor loaded during bootstrap/relog.
            bool observedLiveSinceBinding = false;
            // A v2 presentation or stats packet can report "dead" before the
            // reliable ActorDeath/dead baseline with the synced animation arrives.
            // While this is true, keep the actor alive visually and wait for the
            // death animation source instead of applying a final corpse pose.
            bool pendingRealtimeDeathReplay = false;
            // Non-authority side: short grace for synced special idles. This filters
            // one-frame empty/base-idle gaps from the authority, but lets sustained
            // authority stop/clear states end the remote dance instead of holding it
            // forever.
            float syncedSpecialIdleClearTimer = 0.f;
            // Non-authority side: increments when a reliable synced special-idle
            // start/group event is accepted. CharacterController uses this to
            // replay the same group exactly once when authoritative phase arrives
            // after identity/bootstrap state already selected it.
            uint32_t syncedIdleRevision = 0;
            // Pending magic bolt launch — delayed so bolt appears at end of cast
            // animation rather than immediately when the cast packet arrives.
            float pendingBoltTimer = -1.f;
            std::string pendingBoltSpellId;
            // Last animation group we told the animation system to play on this NPC.
            // Used to detect changes so we only call play()/disable() when the group
            // actually transitions, instead of hammering the animation system every frame.
            std::string lastAppliedAnimGroup;
            // Authority side: true once a death packet with a valid death anim
            // group has been sent for this actor. Prevents duplicate sends and
            // allows deferred send when playRandomDeath() hasn't run yet.
            bool deathPacketSent = false;
            // Non-authority side: remember the last synced death animation group so
            // late or duplicate death packets cannot overwrite the first chosen pose.
            std::string appliedDeathAnimGroup;
            // Non-authority side: remember the last synced hit-state bitfield so we
            // only re-trigger knockout/knockdown/recovery transitions on edges.
            uint32_t lastAppliedHitFlags = 0;
            // Non-authority side: remember the last synced attack press state so we
            // can mirror both press and release without replaying the wind-up every frame.
            bool lastAttackPressed = false;
            uint32_t nextAttackEventId = 1;
            uint32_t lastReceivedAttackEventId = 0;
            uint32_t nextDeathEventId = 1;
            uint32_t lastReceivedDeathEventId = 0;
            // Briefly suppress authoritative lower-body group sync so local hit/attack
            // transitions can finish without being immediately overwritten.
            float animGroupHoldTimer = 0.f;
            // Authority side: debounce one-frame AI/pathing stalls so the wire
            // stream does not alternate walk/stop while the actor is still
            // visibly chasing.
            float authorityLocomotionStopTimer = 0.f;
            // Authority side: special idle groups can briefly disappear from
            // animation sampling at loop/key transitions. Hold the last reliable
            // group briefly so receivers see a stable event stream instead of
            // clear/restart jitter.
            float authorityReliableAnimGroupLostTimer = 0.f;
            // Authority side: debounce special-idle group changes. Dancer and
            // idle loops can expose brief one-frame alternate groups at loop
            // boundaries; only stable group changes become network events.
            std::string authorityPendingReliableAnimGroup;
            float authorityPendingReliableAnimGroupTimer = 0.f;
            // Authority side: short grace after this runtime is promoted from a
            // remote puppet. Keeps the already-synced idle phase alive while
            // local AI packages resume, avoiding a visible phase jump on the
            // client that just lost authority.
            float authoritySyncedIdleHandoffTimer = 0.f;
            // Temporary targeted diagnostics: trace the canonical and physical
            // transforms for a few frames after a reliable cell handoff.
            // Non-authority side: live AI package stack captured before this
            // runtime becomes a pure network puppet. If authority later moves to
            // this client, restore the real package stack instead of promoting an
            // actor with all AI permanently cleared.
            std::shared_ptr<ESM::AiSequence::AiSequence> savedPuppetAiSequence;
            // Non-authority attack replay holds. Attack packets are reliable events,
            // while presentation/position are delayed streams; these short holds keep
            // the visible weapon and stop pose stable through the replayed swing.
            float attackDrawHoldTimer = 0.f;
            float attackLocomotionHoldTimer = 0.f;
            // Non-authority side: keep the synchronized attack control pressed
            // briefly, then release it so CharacterController owns the complete
            // wind-up/hit/follow-through transition.
            float attackControlReleaseTimer = 0.f;
            // Authority side: seconds since this actor was last included in an
            // ActorPositionV2 packet. Dense-cell scheduling uses this to avoid
            // starving active actors behind a large high-priority set.
            float timeSinceLastPositionSend = 0.f;
            bool lastSentPresentationValid = false;
            bool lastSentIsMoving = false;
            bool lastSentAttackingOrCasting = false;
            bool lastSentWeaponDrawn = false;
            bool lastSentSpellReadied = false;
            bool lastSentDead = false;
            uint16_t lastSentMovementFlags = 0;
            int8_t lastSentAnimFwd = 0;
            int8_t lastSentAnimSide = 0;
            uint8_t lastSentPresentationFlags = 0;
            std::string lastSentAnimGroup;
            float presentationSendTimer = 0.f;
            // Log-dedup: true once the steady-state "reused binding" message has
            // been emitted for the current boundActor.  Reset whenever a new
            // binding is established so the first confirmation is always logged.
            bool bindingLogged = false;
            // Missing dynamic records and temporarily unavailable templates can
            // make server-spawned binding fail. Back off per actor instead of
            // retrying and logging on every incoming snapshot.
            uint64_t nextBindingRetryTimeMs = 0;
            uint64_t lastBindingFailureLogTimeMs = 0;
            uint32_t bindingFailureCount = 0;
            uint32_t suppressedBindingFailureLogs = 0;
            ActorInstanceId actorNetId = 0;
            bool hasAuthoritativeTransform = false;
            bool hasAuthoritativeEquipment = false;
            bool waitingForFreshCellBootstrap = false;
            bool rebaseOnNextAuthoritativeSnapshot = false;
            bool smoothFreshBootstrapCorrection = false;
            uint64_t freshCellBootstrapMinServerTimestamp = 0;
            std::string pendingCellChangeDestination;
            float pendingCellChangeRetryTimer = 0.f;
            std::string previousCellChangeCellId;
            float cellChangeReverseGuardTimer = 0.f;
            bool cellChangeReverseGuardLogged = false;
        };

        struct CellRuntime
        {
            ActorList latest;
            std::unordered_map<std::string, ActorRuntime> actors;
            float positionSendTimer = 0.f;
            float positionDiagnosticsTimer = 0.f;
            std::size_t positionSendCursor = 0;
            std::size_t priorityPositionSendCursor = 0;
            uint32_t latestPositionSequence = 0;
            uint32_t latestReliableSequence = 0;
            bool initialListSent = false;
            std::string outboundCellId;
            std::unordered_set<std::string> lastSentActorListKeys;
            // Log-dedup: mpNums already reported via "authority mapped actor".
            // Cleared whenever outboundCellId changes so re-mappings are logged.
            std::unordered_set<uint32_t> authorityLoggedMpNums;
            // Log-dedup: stale generated spawner record ids that were found as
            // vanilla actors in local content and intentionally not exported.
            std::unordered_set<std::string> authoritySkippedUnmanagedSpawners;
        };

        void queueSnapshot(ActorRuntime& actor, const BaseActor& state, const ActorList& list);
        void mergeActorState(ActorRuntime& actor, const BaseActor& state, bool includeTransform);
        void updateLocalCellBootstrapState();
        void markActorsNeedFreshCellBootstrap(const std::string& oldCellId, const std::string& newCellId);
        void completeFreshCellBootstrap(ActorRuntime& actor, const char* source, uint64_t serverTimestamp);
        bool shouldHideForFreshCellBootstrap(const ActorRuntime& actor) const;
        bool shouldReplayDeadBaselineAsRealtime(const ActorRuntime& actor, const BaseActor& state) const;
        void markDeadBaselineState(ActorRuntime& actor, const BaseActor& state, bool replayRealtime);
        ActorInstanceId actorNetIdForActorState(const BaseActor& actor) const;
        ActorInstanceId actorNetIdForPtr(const std::string& cellId, const MWWorld::Ptr& ptr) const;
        void rememberActorNetId(ActorInstanceId actorNetId, const BaseActor& actor);
        void indexActorNetId(ActorInstanceId actorNetId, const std::string& oldCellId, const std::string& newCellId);
        ActorRuntime* findPrimaryActorRuntime(const BaseActor& actor);
        bool hasAuthorityForActor(ActorInstanceId actorNetId, const std::string& cellId) const;
        ActorRuntime& runtimeForPacketActor(const std::string& cellId, CellRuntime& cell, const BaseActor& actor);
        MWWorld::Ptr resolvePacketActorBinding(const std::string& packetCellId, CellRuntime& cell,
            const BaseActor& actor, const char* packetName);
        void logWatchedBorderActor(const char* event, const std::string& packetCellId, const BaseActor& packetActor,
            const ActorRuntime* runtime, const MWWorld::Ptr& resolvedPtr, const char* source) const;
        void advanceSmoothing(ActorRuntime& actor, float dt);
        void fastForwardRuntimeToLatestSnapshot(ActorRuntime& actor, const char* reason, uint64_t eventTimestamp);
        void sendAuthoritativeActorUpdates(const std::string& cellId, CellRuntime& cell, float dt);
        bool shouldAcceptSnapshot(CellRuntime& cell, const ActorList& list, const char* packetName,
            bool isPositionSnapshot = false);
        bool resolveActorBinding(const std::string& cellId, ActorRuntime& actor, bool forceCanonicalCell = false);
        void applyBootstrapDeathState(ActorRuntime& actor);
        void applyBoundActorState(ActorRuntime& actor);
        void rememberServerSpawnedActor(const std::string& cellId, const MWWorld::Ptr& ptr, uint32_t mpNum);
        void forgetServerSpawnedActor(const std::string& cellId, const MWWorld::Ptr& ptr, uint32_t mpNum);
        void forgetServerSpawnedActorPtrMappings(const MWWorld::Ptr& ptr, uint32_t mpNum);
        uint32_t mappedMpNumForPtr(const std::string& cellId, const MWWorld::Ptr& ptr) const;
        bool isStaleServerSpawnedActorUpdate(uint32_t mpNum, uint64_t serverTimestamp) const;
        void rememberServerSpawnedActorTimestamp(uint32_t mpNum, uint64_t serverTimestamp);
        bool takeServerSpawnedRuntimeFromOtherCell(const std::string& targetCellId, const BaseActor& actorState,
            uint64_t serverTimestamp, ActorRuntime& runtime);

        NetworkClient& mClient;
        std::unordered_map<std::string, CellRuntime> mCells;
        std::unordered_map<std::string, bool>        mAuthority;
        std::unordered_map<std::string, uint32_t>    mMpNumsByLocalActor;
        std::unordered_map<uint32_t, MWWorld::Ptr>   mServerSpawnedActorsByMpNum;
        std::unordered_map<uint32_t, uint64_t>       mServerSpawnedActorLastTimestamps;
        // A placed leveled-creature reference can resolve to a different base
        // actor on each client. Map the authoritative vanilla actor identity to
        // the observer-side replacement so later snapshots and authority
        // handoffs continue to use the original refNum.
        std::unordered_map<ActorInstanceId, MWWorld::Ptr>   mReconciledVanillaActorsByNetId;
        // Complete identity snapshots communicate a placed leveled-list
        // Chance None result by omitting the spawner's canonical refNum. Keep
        // that decision across cell unload/reload and authority handoff so a
        // newly loaded local RNG roll cannot become a client-only actor.
        std::unordered_map<std::string, std::unordered_set<ActorInstanceId>>
            mChanceNoneLeveledSpawnersByCell;
        std::unordered_map<ActorInstanceId, ActorRuntime>   mActorsByNetId;
        std::unordered_map<ActorInstanceId, uint32_t>       mActorAuthorityGuids;
        std::unordered_set<ActorInstanceId> mPendingPresentationSampleRequests;
        std::unordered_map<std::string, std::unordered_set<ActorInstanceId>> mCellActorIds;
        std::unordered_map<std::string, ActorInstanceId>    mActorNetIdsByKey;
        std::string mLastLocalCellId;
        bool mHaveLastLocalCellId = false;
        uint64_t mUpdateSerial = 0;
        uint64_t mActorV2DiagnosticsLastLogMs = 0;
        std::size_t mActorV2SnapshotsWindow = 0;
        std::size_t mActorV2InvalidActorIdWindow = 0;
        std::size_t mActorV2MissingIdentityWindow = 0;
        std::size_t mActorV2StaleWindow = 0;
        std::size_t mActorV2DeadLiveSuppressedWindow = 0;
        std::size_t mActorV2ProvisionalAuthoritySuppressedWindow = 0;
        std::size_t mActorV2PositionMovingWindow = 0;
        std::size_t mActorV2PositionAttackingWindow = 0;
        std::size_t mActorV2PositionWeaponDrawnWindow = 0;
        std::size_t mActorV2IdentityTransformPreservedWindow = 0;
        std::size_t mActorV2IdentityZeroTransformSkippedWindow = 0;
        std::size_t mActorV2PresentationSentWindow = 0;
        std::size_t mActorV2PresentationAppliedWindow = 0;
        std::size_t mActorV2PresentationInvalidActorIdWindow = 0;
        std::size_t mActorV2PresentationMissingIdentityWindow = 0;
        std::size_t mActorV2PresentationStaleWindow = 0;
        std::size_t mActorV2PresentationDeadLiveSuppressedWindow = 0;
        std::size_t mActorV2PresentationStopForcedWindow = 0;
        std::size_t mActorV2PresentationGroupChangedWindow = 0;
        std::size_t mActorV2AttackSentWindow = 0;
        std::size_t mActorV2AttackAppliedWindow = 0;
        std::size_t mActorV2AttackInvalidActorIdWindow = 0;
        std::size_t mActorV2AttackMissingIdentityWindow = 0;
        std::size_t mActorV2AttackDuplicateWindow = 0;
        std::size_t mActorV2AttackDeadSuppressedWindow = 0;
        std::unordered_map<ActorInstanceId, std::size_t> mActorV2MissingIdentityByNetIdWindow;
    };

} // namespace mwmp
#endif // OPENMW_MWMP_SYNC_ACTORSYNC_HPP
