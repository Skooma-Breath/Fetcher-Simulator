#include "PlayerSync.hpp"

#include <cmath>
#include <cstring>

#include <components/debug/debuglog.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerPosition.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerCellChange.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerStatsDynamic.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerBaseInfo.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerEquipment.hpp>

#include "../network/Client.hpp"
#include "../network/Protocol.hpp"

// OpenMW world/player access
#include "../../mwbase/environment.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwworld/ptr.hpp"
#include "../../mwworld/class.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/cell.hpp"
#include "../../mwmechanics/creaturestats.hpp"
#include "../../mwworld/inventorystore.hpp"
#include "../../mwworld/player.hpp"
#include "../../mwworld/livecellref.hpp"
#include <components/esm3/loadnpc.hpp>
#include "../../mwmechanics/movement.hpp"
#include <components/openmw-mp/Packets/Player/PacketPlayerAnimFlags.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerAttack.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerCast.hpp>

namespace mwmp
{

PlayerSync::PlayerSync(NetworkClient& client, Protocol& protocol)
    : mClient(client), mProtocol(protocol)
{
}

// ---------------------------------------------------------------------------
void PlayerSync::setPlayer(const MWWorld::Ptr& /*player*/)
{
    // No-op: player readiness is detected each frame via getPlayerPtr()
}

// ---------------------------------------------------------------------------
void PlayerSync::forceFullSync()
{
    if (mLocal.guid == 0) return; // guid not yet assigned; will be called again after handshake
    // Pull latest cell/stats before sending — player ptr may not be set yet
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (world)
    {
        MWWorld::Ptr player = world->getPlayerPtr();
        if (!player.isEmpty())
        {
            // Snapshot position
            const auto& refData = player.getRefData();
            const auto& pos     = refData.getPosition();
            mLocal.position.pos[0] = pos.pos[0];
            mLocal.position.pos[1] = pos.pos[1];
            mLocal.position.pos[2] = pos.pos[2];
            mLocal.position.rot[0] = pos.rot[0];
            mLocal.position.rot[1] = pos.rot[1];
            mLocal.position.rot[2] = pos.rot[2];

            // Snapshot velocity (zero for initial full sync)
            mLocal.velocity.linear[0] = 0.f;
            mLocal.velocity.linear[1] = 0.f;
            mLocal.velocity.linear[2] = 0.f;

            // Snapshot appearance — read directly from the player's ESM::NPC record
            // so remote clients can construct a unique-looking NPC for this player.
            if (const auto* npcRef = player.get<ESM::NPC>())
            {
                const ESM::NPC* npcBase = npcRef->mBase;
                mLocal.race     = npcBase->mRace.serializeText();
                mLocal.headMesh = npcBase->mHead.serializeText();
                mLocal.hairMesh = npcBase->mHair.serializeText();
                mLocal.isMale   = npcBase->isMale();
            }

            // Snapshot cell
            if (const MWWorld::CellStore* cs = player.getCell())
            {
                const MWWorld::Cell* ci = cs->getCell();
                mLocal.cell.isExterior = ci->isExterior();
                mLocal.cell.cellName   = std::string(ci->getNameId());
                if (ci->isExterior())
                {
                    mLocal.cell.gridX = ci->getGridX();
                    mLocal.cell.gridY = ci->getGridY();
                }
            }
        }
    }

    sendBaseInfo();
    sendCellChange();
    sendEquipment();
    sendDynamicStats();
    mPositionTimer = POSITION_RATE; // force position send next tick
    snapshotPosition();
    snapshotCell();
    snapshotEquipment();
    snapshotDynamicStats();
    Log(Debug::Info) << "[MP] PlayerSync: full sync sent (guid=" << mLocal.guid << ")";
}

// ---------------------------------------------------------------------------
void PlayerSync::applyServerPositionCorrection(const BasePlayer& auth)
{
    // The server has told us we're in the wrong place.
    // In Phase 1 we just log it; Phase 2 will warp the player.
    Log(Debug::Warning) << "[MP] Server position correction received ("
                        << auth.position.pos[0] << ", "
                        << auth.position.pos[1] << ", "
                        << auth.position.pos[2] << ")";
}

// ---------------------------------------------------------------------------
void PlayerSync::update(float dt)
{
    if (!mClient.isConnected())
        return;

    // Pull live state from OpenMW
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return;

    MWWorld::Ptr player = world->getPlayerPtr();
    if (player.isEmpty()) return;

    // Don't send anything until we have a valid server-assigned guid.
    // forceFullSync() is called by Main.cpp from the CharacterData handler
    // once mLocal.guid is properly set.
    if (mLocal.guid == 0)
        return;

    // --- position / rotation / velocity ---
    const auto& refData  = player.getRefData();
    const auto& pos      = refData.getPosition();

    if (dt > 0.0001f)
    {
        mLocal.velocity.linear[0] = (pos.pos[0] - mLocal.position.pos[0]) / dt;
        mLocal.velocity.linear[1] = (pos.pos[1] - mLocal.position.pos[1]) / dt;
        mLocal.velocity.linear[2] = (pos.pos[2] - mLocal.position.pos[2]) / dt;
    }

    mLocal.position.pos[0] = pos.pos[0];
    mLocal.position.pos[1] = pos.pos[1];
    mLocal.position.pos[2] = pos.pos[2];
    mLocal.position.rot[0] = pos.rot[0];
    mLocal.position.rot[1] = pos.rot[1];
    mLocal.position.rot[2] = pos.rot[2];

    // --- dynamic stats ---
    const auto& cstats = player.getClass().getCreatureStats(player);
    mLocal.dynamicStats.health.base    = cstats.getHealth().getBase();
    mLocal.dynamicStats.health.current = cstats.getHealth().getCurrent();
    mLocal.dynamicStats.magicka.base   = cstats.getMagicka().getBase();
    mLocal.dynamicStats.magicka.current= cstats.getMagicka().getCurrent();
    mLocal.dynamicStats.fatigue.base   = cstats.getFatigue().getBase();
    mLocal.dynamicStats.fatigue.current= cstats.getFatigue().getCurrent();

    // --- cell ---
    if (const MWWorld::CellStore* cs = player.getCell())
    {
        const MWWorld::Cell* ci = cs->getCell();
        mLocal.cell.isExterior = ci->isExterior();
        mLocal.cell.cellName   = std::string(ci->getNameId());
        if (ci->isExterior())
        {
            mLocal.cell.gridX = ci->getGridX();
            mLocal.cell.gridY = ci->getGridY();
        }
    }

    tickPosition(dt);
    tickDynamicStats(dt);
    sendAnimFlags();
    sendAttack();
    sendCast();

    // --- on-change checks ---
    if (cellChanged())
    {
        snapshotCell();
        sendCellChange();
    }
    if (equipmentChanged())
    {
        snapshotEquipment();
        sendEquipment();
    }
}

// ---------------------------------------------------------------------------
// Position — unreliable, rate-limited
void PlayerSync::tickPosition(float dt)
{
    mPositionTimer += dt;
    if (mPositionTimer < POSITION_RATE)
        return;
    mPositionTimer = 0.f;

    if (!positionChanged())
        return;

    snapshotPosition();
    sendPosition();
}

// ---------------------------------------------------------------------------
// Dynamic stats — reliable, rate-limited
void PlayerSync::tickDynamicStats(float dt)
{
    mStatsTimer += dt;
    if (mStatsTimer < STATS_RATE)
        return;
    mStatsTimer = 0.f;

    if (!dynamicStatsChanged())
        return;

    snapshotDynamicStats();
    sendDynamicStats();
}

// ---------------------------------------------------------------------------
void PlayerSync::sendPosition()
{
    PacketPlayerPosition pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendUnreliable(pkt.encode(mSeqCounter++));
}

void PlayerSync::sendCellChange()
{
    PacketPlayerCellChange pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
    Log(Debug::Verbose) << "[MP] PlayerSync: cell change sent -> "
                        << mLocal.cell.cellName;
}

void PlayerSync::sendDynamicStats()
{
    PacketPlayerStatsDynamic pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
}

void PlayerSync::sendBaseInfo()
{
    PacketPlayerBaseInfo pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
}

void PlayerSync::sendEquipment()
{
    PacketPlayerEquipment pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
}

// ---------------------------------------------------------------------------
// sendAnimFlags — unreliable, per-frame, delta-suppressed.
//
// Reads movement vector and CreatureStats flags from the live player Ptr,
// encodes them into AnimFlags using the MF_* / AF_* constants from
// BaseStructs.hpp, and sends unreliably at the same cadence as position.
// No rate-limiting needed — the position timer already throttles the outer
// update() to 20 Hz and we delta-suppress here as well.
void PlayerSync::sendAnimFlags()
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return;
    MWWorld::Ptr player = world->getPlayerPtr();
    if (player.isEmpty()) return;

    const MWMechanics::Movement& mov
        = player.getClass().getMovementSettings(player);
    const MWMechanics::CreatureStats& stats
        = player.getClass().getCreatureStats(player);

    AnimFlags f{};

    // movementType: prefer raw local input when it is present, because that
    // preserves the intended forward/back/strafe direction exactly. Fall back
    // to real world-space displacement only when input reads idle at this
    // point in the frame.
    float fwd = mov.mPosition[1];
    float side = mov.mPosition[0];
    if (std::abs(fwd) < 0.1f && std::abs(side) < 0.1f && mHaveLastAnimSample)
    {
        const float dx = mLocal.position.pos[0] - mLastAnimSample.pos[0];
        const float dy = mLocal.position.pos[1] - mLastAnimSample.pos[1];

        // Use the previous sample yaw for this interval. Projecting the whole
        // displacement against the new yaw can misclassify movement while the
        // player is turning, which shows up as moonwalking on the remote side.
        const float yaw = mLastAnimSample.rot[2];
        const float sinYaw = std::sin(yaw);
        const float cosYaw = std::cos(yaw);

        // CharacterController converts local movement to world movement with
        // rotateVec2f(local, -yaw), so the inverse projection here must use
        // +yaw. Using the wrong sign flips direction classification for some
        // headings and produces apparent moonwalking remotely.
        side = dx * cosYaw - dy * sinYaw;
        fwd = dx * sinYaw + dy * cosYaw;
    }

    if      (fwd  >  0.1f) f.movementType = 0;  // forward
    else if (fwd  < -0.1f) f.movementType = 1;  // backward
    else if (side < -0.1f) f.movementType = 2;  // left
    else if (side >  0.1f) f.movementType = 3;  // right
    else                   f.movementType = -1; // idle

    // movementFlags bitmask
    if (stats.getMovementFlag(MWMechanics::CreatureStats::Flag_Run))
        f.movementFlags |= AnimFlags::MF_RUN;
    if (stats.getMovementFlag(MWMechanics::CreatureStats::Flag_Sneak))
        f.movementFlags |= AnimFlags::MF_SNEAK;
    // Use getPlayer().getJumping() — set by PhysicsSystem::handleJump() the same
    // frame the jump impulse fires, before the actor physically leaves the ground.
    // This matches when the local CharacterController starts the jump animation,
    // removing the ~50ms delay that came from waiting for !isOnGround().
    // Fallback: also catch free-falls (walking off a ledge) via !isOnGround(),
    // but exclude swimming and flying which are non-jump airborne states.
    const bool jumpingFlag = world->getPlayer().getJumping()
        || (!world->isOnGround(player) && !world->isSwimming(player) && !world->isFlying(player));
    if (jumpingFlag)
        f.movementFlags |= AnimFlags::MF_JUMP;

    if (mov.mIsStrafing)
        f.movementFlags |= AnimFlags::MF_STRAFING;

    // actionFlags bitmask
    if (stats.getAttackingOrSpell())
        f.actionFlags |= AnimFlags::AF_ATTACKING;
    if (stats.getDrawState() == MWMechanics::DrawState::Weapon)
        f.actionFlags |= AnimFlags::AF_WEAPON_DRAWN;
    if (stats.getDrawState() == MWMechanics::DrawState::Spell)
        f.actionFlags |= AnimFlags::AF_SPELL_READY;

    std::memcpy(mLastAnimSample.pos, mLocal.position.pos, sizeof(mLastAnimSample.pos));
    std::memcpy(mLastAnimSample.rot, mLocal.position.rot, sizeof(mLastAnimSample.rot));
    mHaveLastAnimSample = true;

    if (!animFlagsChanged() && f.movementFlags == mLastAnimFlags.movementFlags
        && f.actionFlags  == mLastAnimFlags.actionFlags
        && f.movementType == mLastAnimFlags.movementType)
        return;

    mLastAnimFlags    = f;
    mLocal.animFlags  = f;

    PacketPlayerAnimFlags pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendUnreliable(pkt.encode(mSeqCounter++));
}

// ---------------------------------------------------------------------------
// sendAttack — reliable, edge-triggered on pressed→true transition.
void PlayerSync::sendAttack()
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return;
    MWWorld::Ptr player = world->getPlayerPtr();
    if (player.isEmpty()) return;

    const MWMechanics::CreatureStats& stats
        = player.getClass().getCreatureStats(player);
    const bool pressed = stats.getAttackingOrSpell();

    // Only send on the rising edge (button press), not while held
    if (pressed == mLastAttackPressed) return;
    mLastAttackPressed = pressed;

    mLocal.attack.pressed = pressed;
    // target, hit, strength etc. are set by the combat system hooks
    // (Phase 7E); for now we relay the press/release so the animation fires.

    PacketPlayerAttack pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
}

// ---------------------------------------------------------------------------
// sendCast — reliable, edge-triggered when a spell fires.
void PlayerSync::sendCast()
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return;
    MWWorld::Ptr player = world->getPlayerPtr();
    if (player.isEmpty()) return;

    const MWMechanics::CreatureStats& stats
        = player.getClass().getCreatureStats(player);

    // Use the same AttackingOrSpell flag — the CharacterController sets it
    // for both melee and spell actions.  We distinguish via attack.type:
    // type 1 = magic (see BaseStructs Attack::type comment).
    // Phase 7E will wire into the actual MWMechanics spell-cast callback for
    // accurate spellId / target data.  For now we detect the flag edge.
    const bool casting = stats.getAttackingOrSpell()
        && (stats.getAttackType() == "cast"
            || stats.getAttackType() == "spellcast");

    if (casting == mLastCastingOrSpell) return;
    mLastCastingOrSpell = casting;
    if (!casting) return; // only send on the rising edge

    mLocal.castSpell.success = true; // optimistic — Phase 7E validates
    PacketPlayerCast pkt;
    pkt.setPlayer(&mLocal);
    mClient.sendReliable(pkt.encode(mSeqCounter++));
}

// ---------------------------------------------------------------------------
// Change detection helpers
bool PlayerSync::positionChanged() const
{
    constexpr float POS_THRESHOLD = 0.5f;
    constexpr float ROT_THRESHOLD = 0.02f; // ~1.1 degrees — catches mouse-look turns

    for (int i = 0; i < 3; ++i)
        if (std::abs(mLocal.position.pos[i] - mLastPos.pos[i]) > POS_THRESHOLD)
            return true;

    // Rotation: compare with wrap-around so the 0/2π boundary doesn't cause
    // false negatives when the player is facing near north/south.
    constexpr float TWO_PI = 6.28318530718f;
    constexpr float PI     = 3.14159265359f;
    for (int i = 0; i < 3; ++i)
    {
        float diff = mLocal.position.rot[i] - mLastPos.rot[i];
        // Normalise diff to [-π, π]
        while (diff >  PI) diff -= TWO_PI;
        while (diff < -PI) diff += TWO_PI;
        if (std::abs(diff) > ROT_THRESHOLD)
            return true;
    }
    return false;
}

bool PlayerSync::cellChanged() const
{
    return mLocal.cell.cellName  != mLastCell.cellName
        || mLocal.cell.isExterior != mLastCell.isExterior
        || mLocal.cell.gridX     != mLastCell.gx
        || mLocal.cell.gridY     != mLastCell.gy;
}

bool PlayerSync::equipmentChanged() const
{
    for (int i = 0; i < BasePlayer::NUM_EQUIPMENT_SLOTS; ++i)
    {
        const std::string& live = (i < (int)mLocal.equipment.size())
            ? mLocal.equipment[i].item.refId : "";
        if (live != mLastEquip[i])
            return true;
    }
    return false;
}

bool PlayerSync::dynamicStatsChanged() const
{
    constexpr float EPS = 0.01f;
    return std::abs(mLocal.dynamicStats.health.current  - mLastStats.hCur) > EPS
        || std::abs(mLocal.dynamicStats.magicka.current - mLastStats.mCur) > EPS
        || std::abs(mLocal.dynamicStats.fatigue.current - mLastStats.fCur) > EPS;
}

bool PlayerSync::animFlagsChanged() const
{
    return mLocal.animFlags.movementFlags != mLastAnimFlags.movementFlags
        || mLocal.animFlags.actionFlags   != mLastAnimFlags.actionFlags
        || mLocal.animFlags.movementType  != mLastAnimFlags.movementType;
}

// ---------------------------------------------------------------------------
void PlayerSync::snapshotPosition()
{
    std::memcpy(mLastPos.pos, mLocal.position.pos, sizeof(mLastPos.pos));
    std::memcpy(mLastPos.rot, mLocal.position.rot, sizeof(mLastPos.rot));
}
void PlayerSync::snapshotCell()
{
    mLastCell = { mLocal.cell.cellName, mLocal.cell.isExterior,
                  mLocal.cell.gridX, mLocal.cell.gridY };
}
void PlayerSync::snapshotEquipment()
{
    for (int i = 0; i < BasePlayer::NUM_EQUIPMENT_SLOTS; ++i)
        mLastEquip[i] = (i < (int)mLocal.equipment.size())
            ? mLocal.equipment[i].item.refId : "";
}
void PlayerSync::snapshotDynamicStats()
{
    mLastStats = { mLocal.dynamicStats.health.current,
                   mLocal.dynamicStats.magicka.current,
                   mLocal.dynamicStats.fatigue.current };
}

} // namespace mwmp
