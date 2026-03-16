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

        // Packet handlers — called by the Networking dispatcher
        void onPositionUpdate    (const BasePlayer& state);
        void onEquipmentUpdate   (const BasePlayer& state);
        void onStatsDynamicUpdate(const BasePlayer& state);
        void onCellChange        (const BasePlayer& state);
        void onDeath             (const BasePlayer& state);
        void onResurrect         (const BasePlayer& state);

        // Accessors
        uint32_t           getGuid()     const { return mGuid; }
        const std::string& getName()     const { return mName; }
        const BasePlayer&  getState()    const { return mState; }
        bool               isDead()      const { return mIsDead; }
        bool               isSpawned()   const { return mIsSpawned; }

    private:
        // ---- world interaction ----
        void trySpawn();
        void despawnFromWorld();
        void applyInterpolationToWorld();

        // ---- interpolation ----
        void updateInterpolation(float dt);

        // ---- cell helpers ----
        // quiet=true suppresses the verbose mismatch log (used from
        // applyInterpolationToWorld to avoid double-logging with trySpawn).
        bool isInSameCellAsLocalPlayer(bool quiet = false) const;

        uint32_t    mGuid;
        std::string mName;
        BasePlayer  mState;       // latest authoritative state from server
        bool        mIsDead = false;

        // --- world NPC ---
        MWWorld::Ptr mNpcPtr;
        bool         mIsSpawned = false;
        std::unique_ptr<Nameplate> mNameplate;

        // --- trySpawn cooldown ---
        // trySpawn() is called every frame until the NPC is placed.
        // Rate-limit it so we don't spam log/work every frame while waiting
        // for the remote player's first CellChange packet or a cell mismatch.
        float mSpawnRetryTimer = 0.f;
        static constexpr float SPAWN_RETRY_RATE = 0.2f; // seconds between attempts

        // --- interpolation state ---
        struct InterpState
        {
            float cx = 0.f, cy = 0.f, cz = 0.f;    // current rendered pos
            float tx = 0.f, ty = 0.f, tz = 0.f;    // target pos
            float crx = 0.f, cry = 0.f, crz = 0.f; // current rot
            float trx = 0.f, try_ = 0.f, trz = 0.f;// target rot
            bool  hasTarget       = false;
            bool  hasSnapped      = false;  // true after first snap
        } mInterp;

        // Lerp factors (per second). Position is smoothed to absorb jitter;
        // rotation is snappier so turns feel responsive.
        static constexpr float POS_INTERP_SPEED = 10.f;
        static constexpr float ROT_INTERP_SPEED = 20.f;
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
