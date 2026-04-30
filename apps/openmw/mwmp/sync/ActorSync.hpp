#ifndef OPENMW_MWMP_SYNC_ACTORSYNC_HPP
#define OPENMW_MWMP_SYNC_ACTORSYNC_HPP

#include <string>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <osg/Vec3f>
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

        // Must be called when the local player fully disconnects from a server
        // (e.g. returns to main menu).  Clears all per-session cell/actor
        // tracking so stale Ptr references from the previous game session are
        // never dereferenced after the world has been torn down.
        void resetSessionState();

        void onAuthorityUpdate(const ActorList& list);

        void onActorListUpdate(const ActorList& list);
        void onActorPositionUpdate(const ActorList& list);
        void onActorAnimFlagsUpdate(const ActorList& list);
        void onActorAnimPlay(const ActorList& list);
        void onActorAttack(const ActorList& list);
        void onActorCast(const ActorList& list);
        void onActorDeath(const ActorList& list);
        void onActorEquipment(const ActorList& list);
        void onActorStatsDynamic(const ActorList& list);
        void onActorAI(const ActorList& list);
        void onActorCombatRequest(const ActorList& list);

        bool hasAuthority(const std::string& cellId) const;
        uint32_t getActorMpNum(const MWWorld::Ptr& ptr) const;
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
        };

        struct ActorRuntime
        {
            BaseActor state;
            Position smoothedPosition;
            bool hasSmoothedPosition = false;
            std::deque<BufferedSnapshot> snapshots;
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
            // When true, the death came from a real-time ActorDeath packet (not an
            // ActorList load), so the death animation should play from the start.
            bool deathFromRealtimePacket = false;
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
            // Briefly suppress authoritative lower-body group sync so local hit/attack
            // transitions can finish without being immediately overwritten.
            float animGroupHoldTimer = 0.f;
            // Log-dedup: true once the steady-state "reused binding" message has
            // been emitted for the current boundActor.  Reset whenever a new
            // binding is established so the first confirmation is always logged.
            bool bindingLogged = false;
        };

        struct CellRuntime
        {
            ActorList latest;
            std::unordered_map<std::string, ActorRuntime> actors;
            float positionSendTimer = 0.f;
            bool initialListSent = false;
            std::string outboundCellId;
            // Log-dedup: mpNums already reported via "authority mapped actor".
            // Cleared whenever outboundCellId changes so re-mappings are logged.
            std::unordered_set<uint32_t> authorityLoggedMpNums;
        };

        void queueSnapshot(ActorRuntime& actor, const BaseActor& state, const ActorList& list);
        void mergeActorState(ActorRuntime& actor, const BaseActor& state, bool includeTransform);
        void advanceSmoothing(ActorRuntime& actor, float dt);
        void sendAuthoritativeActorUpdates(const std::string& cellId, CellRuntime& cell, float dt);
        bool shouldAcceptSnapshot(CellRuntime& cell, const ActorList& list, const char* packetName);
        bool resolveActorBinding(const std::string& cellId, ActorRuntime& actor);
        void applyBootstrapDeathState(ActorRuntime& actor);
        void applyBoundActorState(ActorRuntime& actor);
        void rememberServerSpawnedActor(const std::string& cellId, const MWWorld::Ptr& ptr, uint32_t mpNum);
        void forgetServerSpawnedActor(const std::string& cellId, const MWWorld::Ptr& ptr, uint32_t mpNum);
        uint32_t mappedMpNumForPtr(const std::string& cellId, const MWWorld::Ptr& ptr) const;

        NetworkClient& mClient;
        std::unordered_map<std::string, CellRuntime> mCells;
        std::unordered_map<std::string, bool>        mAuthority;
        std::unordered_map<std::string, uint32_t>    mMpNumsByLocalActor;
        std::unordered_map<uint32_t, MWWorld::Ptr>   mServerSpawnedActorsByMpNum;
    };

} // namespace mwmp
#endif // OPENMW_MWMP_SYNC_ACTORSYNC_HPP
