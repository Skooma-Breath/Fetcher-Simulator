#ifndef OPENMW_MWMP_SYNC_REMOTEPLAYER_HPP
#define OPENMW_MWMP_SYNC_REMOTEPLAYER_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <components/openmw-mp/Base/BasePlayer.hpp>

#include "../../mwworld/ptr.hpp"

#include "Nameplate.hpp"

namespace mwmp
{
    // -----------------------------------------------------------------------
    // RemotePlayer — represents one other connected player in our game world.
    //
    // Lifecycle:
    //   1. Constructed when server sends PlayerBaseInfo for a new guid.
    //   2. update() is called every frame; spawns/moves the NPC in the world.
    //   3. The NPC is only visible when the remote player is in the same
    //      active cell as the local player.
    //   4. Destructor cleans up the world NPC.
    // -----------------------------------------------------------------------
    class RemotePlayer
    {
    public:
        explicit RemotePlayer(uint32_t guid, const std::string& name);
        ~RemotePlayer();

        // Non-copyable
        RemotePlayer(const RemotePlayer&) = delete;
        RemotePlayer& operator=(const RemotePlayer&) = delete;

        // Called every frame; drives interpolation and world placement
        void update(float dt);

        // --- Phase 6 packet handlers ---
        void onBaseInfoUpdate    (const BasePlayer& state);
        void onPositionUpdate    (const BasePlayer& state);
        void onEquipmentUpdate   (const BasePlayer& state);
        void onStatsDynamicUpdate(const BasePlayer& state);
        void onCellChange        (const BasePlayer& state);
        void onDeath             (const BasePlayer& state);
        void onResurrect         (const BasePlayer& state);

        // --- Phase 7 pre-work packet handlers ---

        // Unreliable per-frame: apply movement/action flags to CharacterController
        // so the vanilla animation system drives locomotion automatically.
        void onAnimFlagsUpdate   (const BasePlayer& state);

        // Reliable one-shot: explicitly trigger an animation group on the remote NPC.
        void onAnimPlay          (const BasePlayer& state);

        // Reliable: apply melee/ranged hit to target, trigger attack anim.
        void onAttack            (const BasePlayer& state);

        // Reliable: apply spell effects to target, trigger cast anim.
        void onCast              (const BasePlayer& state);

        // Cosmetic inventory delta: keep remote NPC ContainerStore consistent
        // with what they're carrying so equipment renders correctly.
        void onInventoryUpdate   (const BasePlayer& state);

        // Accessors
        uint32_t           getGuid()     const { return mGuid; }
        const std::string& getName()     const { return mName; }
        const BasePlayer&  getState()    const { return mState; }
        bool               isDead()      const { return mIsDead; }
        bool               isSpawned()   const { return mIsSpawned; }
        const MWWorld::Ptr& getNpcPtr()  const { return mNpcPtr; }

    private:
        // ---- world interaction ----
        void trySpawn();
        void despawnFromWorld();
        void ensureMechanicsRegistration();
        void applyInterpolationToWorld();
        void applyAnimationStateToActor();
        void applyDynamicStatsToActor();
        void applyEquipmentState(const BasePlayer& state, bool playSounds);
        void applyInventoryState(const BasePlayer& state, bool playSounds);

        // ---- interpolation ----
        void updateInterpolation(float dt);

        // ---- cell helpers ----
        bool isInSameCellAsLocalPlayer(bool quiet = false) const;

        uint32_t    mGuid;
        std::string mName;
        BasePlayer  mState;
        bool        mIsDead = false;

        // --- world NPC ---
        MWWorld::Ptr mNpcPtr;
        bool         mIsSpawned = false;
        bool         mMechanicsRegistered = false;
        bool         mEquipmentSoundReady = false;
        bool         mInventorySoundReady = false;
        std::unique_ptr<Nameplate> mNameplate;

        // --- trySpawn cooldown ---
        float mSpawnRetryTimer = 0.f;
        static constexpr float SPAWN_RETRY_RATE = 0.2f;

        // --- interpolation state ---
        struct InterpState
        {
            float cx = 0.f, cy = 0.f, cz = 0.f;
            float tx = 0.f, ty = 0.f, tz = 0.f;
            // Last position received from network — used to cap XY dead-reckoning
            // drift so a stale velocity can't walk the target off indefinitely.
            float lastRecvX = 0.f, lastRecvY = 0.f, lastRecvZ = 0.f;
            float crx = 0.f, cry = 0.f, crz = 0.f;
            float trx = 0.f, try_ = 0.f, trz = 0.f;
            float targetVz = 0.f;
            float yawDelta = 0.f;
            bool  hasTarget  = false;
            bool  hasSnapped = false;
        } mInterp;

        static constexpr float POS_INTERP_SPEED = 15.f;
        static constexpr float ROT_INTERP_SPEED = 20.f;

        // Throttled debug output for comparing interpolation speed to
        // CharacterController animation-rate decisions on the same remote actor.
        float mFootstepDebugTimer = 0.f;

        // Per-frame interpolation planar speed (units/s), broadcast to the
        // CharacterController via a user-value on the NPC base node so that
        // animation rate tracks actual movement instead of stats-based speed.
        float mInterpPlanarSpeed = 0.f;

        // --- anim flag state (last applied, for delta suppression) ---
        AnimFlags mLastAppliedAnimFlags;
        uint32_t mAppliedHitFlags = 0;

        // Edge-detect for MF_JUMP — trigger "jump"/"jump landing" anim once on
        // rising/falling edge.  We never write mPosition[2] for remote players
        // because that feeds handleJump() in PhysicsSystem which imparts a real
        // upward velocity impulse every frame and fights the Z interpolator.
        bool mWasJumping = false;
        bool mWasFlying  = false;
        float mJumpLandingTimer = 0.f;
        // True once the first position packet carrying a meaningful vz (>30 u/s) has
        // arrived since the jump rising edge.  Pseudo-gravity is suppressed until primed
        // so that a zero-seeded targetVz (from the EMA-lagged AnimFlags jumpVz field)
        // cannot immediately drag the arc downward before physics data arrives.
        bool mJumpArcPrimed = false;
        // Hysteresis for animation grouping (prevents "tripping" look)
        bool mIsStrafing = false;
        // Tracking age of last position packet for extrapolation braking
        float mTimeSinceLastPosUpdate = 0.f;
    };

    // -----------------------------------------------------------------------
    // PlayerList — owns all RemotePlayers, drives their update loop.
    // -----------------------------------------------------------------------
    class PlayerList
    {
    public:
        void addPlayer   (uint32_t guid, const std::string& name);
        void removePlayer(uint32_t guid);

        RemotePlayer* getPlayer(uint32_t guid);

        void updateAll(float dt);

        size_t count() const { return mPlayers.size(); }

    private:
        std::unordered_map<uint32_t, std::unique_ptr<RemotePlayer>> mPlayers;
    };

} // namespace mwmp

#endif // OPENMW_MWMP_SYNC_REMOTEPLAYER_HPP
