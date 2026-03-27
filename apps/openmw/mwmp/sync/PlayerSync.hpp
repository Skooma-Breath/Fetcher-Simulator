#ifndef OPENMW_MWMP_SYNC_PLAYERSYNC_HPP
#define OPENMW_MWMP_SYNC_PLAYERSYNC_HPP

#include <cstdint>

#include <components/openmw-mp/Base/BasePlayer.hpp>
#include <components/openmw-mp/NetworkMessages.hpp>

namespace MWWorld { class Ptr; }

namespace mwmp
{
    class NetworkClient;
    class Protocol;

    // -----------------------------------------------------------------------
    // PlayerSync — tracks the local player's state and sends delta updates
    // to the server at the appropriate frequency.
    //
    // High-frequency (unreliable, ~20 Hz): position, velocity, anim flags
    // On-change (reliable):               equipment, stats, cell change
    // Low-frequency (reliable, ~1 Hz):    dynamic stats (HP/MP/FP)
    // -----------------------------------------------------------------------
    class PlayerSync
    {
    public:
        PlayerSync(NetworkClient& client, Protocol& protocol);

        // Called every engine frame
        void update(float dt);

        // Bind to the actual player Ptr once the world is loaded
        void setPlayer(const MWWorld::Ptr& player);

        // Force-flush all state immediately (e.g. just after connect)
        void forceFullSync();

        // Server told us our position was wrong — snap to authoritative value
        void applyServerPositionCorrection(const BasePlayer& authoritative);

        // Accessors used by Networking dispatcher
        BasePlayer& localPlayer() { return mLocal; }

    private:
        // ---- per-frame checks ----
        void tickPosition(float dt);
        void tickDynamicStats(float dt);

        // ---- send helpers ----
        void sendPosition();
        void sendCellChange();
        void sendEquipment();
        void sendAnimFlags(float dt);
        void sendAttack();
        void sendCast();
        void sendDynamicStats();
        void sendBaseInfo();

        // ---- change detection ----
        bool positionChanged()    const;
        bool cellChanged()        const;
        bool equipmentChanged()   const;
        bool dynamicStatsChanged() const;
        bool animFlagsChanged()    const;

        // ---- utilities ----
        void snapshotPosition();
        void snapshotCell();
        void snapshotEquipment();
        void snapshotDynamicStats();

        NetworkClient& mClient;
        Protocol&      mProtocol;

        BasePlayer     mLocal;          // live mirror of local player state
        bool           mPlayerReady = false;

        // --- send-rate accumulators ---
        float mPositionTimer    = 0.f;
        float mStatsTimer       = 0.f;

        static constexpr float POSITION_RATE = 0.033f; // 30 Hz
        static constexpr float STATS_RATE    = 1.0f;   // 1 Hz

        // --- last-sent snapshots for delta detection ---
        struct PositionSnapshot { float pos[3]; float rot[3]; };
        PositionSnapshot mLastPos{};

        struct CellSnapshot { std::string cellName; bool isExterior; int gx; int gy; };
        CellSnapshot mLastCell{};

        // equipment slots as refIds for quick comparison
        std::array<std::string, BasePlayer::NUM_EQUIPMENT_SLOTS> mLastEquip{};

        struct StatsSnapshot { float hCur, mCur, fCur; };
        StatsSnapshot mLastStats{};

        AnimFlags mLastAnimFlags{};

        // Periodic full anim-flags resend — self-heals receiver stuck state
        // caused by UDP packet loss (delta-suppress means the corrective
        // packet never gets re-sent otherwise).
        float mAnimRefreshTimer = 0.f;
        static constexpr float ANIM_REFRESH_RATE = 2.0f;

        // Last attack pressed state — detect edge (false→true) for send
        bool mLastAttackPressed = false;
        bool mLastCastingOrSpell = false;

        uint32_t mSeqCounter = 0;
    };

} // namespace mwmp

#endif // OPENMW_MWMP_SYNC_PLAYERSYNC_HPP
