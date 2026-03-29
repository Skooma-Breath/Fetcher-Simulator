#include "PlayerSync.hpp"

#include <cmath>
#include <cstring>

#include <components/debug/debuglog.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerPosition.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerCellChange.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerStatsDynamic.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerBaseInfo.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerEquipment.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>

#include "../network/Client.hpp"
#include "../network/Protocol.hpp"

// OpenMW world/player access
#include "../../mwbase/environment.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwbase/inputmanager.hpp"
#include "../../mwrender/camera.hpp"
#include "../../mwinput/actions.hpp"
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

            // NEW: Record current position as 'last' to prevent massive velocity spike on next update
            mLocal.position.pos[0] = pos.pos[0];
            mLocal.position.pos[1] = pos.pos[1];
            mLocal.position.pos[2] = pos.pos[2];
            mLocal.position.rot[0] = pos.rot[0];
            mLocal.position.rot[1] = pos.rot[1];
            mLocal.position.rot[2] = pos.rot[2];

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
    sendAnimFlags(dt);
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
// sendAnimFlags — per-frame, delta-suppressed; edge packets sent reliably.
//
// Normal flow: unreliable send whenever any flag or axis crosses the 0.05
// delta threshold.  Three special cases are ALWAYS sent reliably:
//
//   MF_JUMP rising  edge (0→1): ensures receiver enters JumpState_InAir
//     even if the first in-air unreliable packet is dropped.
//   MF_JUMP falling edge (1→0): the critical one for standstill landings.
//     After a standstill jump the player is perfectly still, so all
//     subsequent packets are identical — delta suppression fires nothing.
//     The receiver stays frozen in JumpState_InAir / ForceJump=true until
//     the 5-second force-refresh fires and the landing anim never plays.
//     Sending the falling edge reliably guarantees the CC makes the
//     JumpState_InAir → JumpState_Landing transition on the receiver.
//   Movement idle↔moving edge (any stance): a quick tap produces exactly
//     1 start and 1 stop packet; UDP loss of either means the remote CC
//     never transitions out of idle — no leg wiggle / walk anim.  Sending
//     both boundaries reliably fixes this for walk, run, and sneak alike.
void PlayerSync::sendAnimFlags(float dt)
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

    // animFwd/animSide / MF_STRAFING: classify from world-space velocity projected
    // into body-local axes.
    //
    // WHY NOT mPosition[0/1]:
    //   In 3rd-person mode the engine Lua script (move360.lua) rotates the raw
    //   WASD input by (cameraYaw − bodyYaw) before writing it back into
    //   self.controls.sideMovement / controls.movement.  While the body is
    //   catching up (TurnToMovementDirection), pressing W alone produces a large
    //   sideMovement — we would encode strafe. Always use world velocity instead.
    //
    // WHY world velocity:
    //   mLocal.velocity.linear is computed from successive world-space positions
    //   and is completely camera-mode-agnostic.
    //
    // PROJECTION YAW — always body yaw:
    //   Camera yaw was tried but causes a flip when Tab-orbiting while walking:
    //   the camera rotates 180° while world-space velocity (W still held) stays
    //   forward → camera-relative projection gives fwd=-1 → remote NPC walks
    //   backward while the local player is still moving forward.
    //
    //   Body yaw is now safe for strafe: the isNetworkNpc guard in character.cpp
    //   prevents the CC from overwriting mIsStrafing each frame, so the receiver's
    //   hysteresis (enter=2x, exit=4x) holds through any body-camera lag.  The
    //   initial strafe entry fires cleanly (lag≈0 when A/D is first pressed).
    //
    // WASD GATE:
    //   Tab orbit with no movement keys induces body displacement (~18–35 u/s)
    //   above the velocity gate via move360.lua yawChange.  Zero axes when no
    //   movement key is held so camera orbit never triggers a walk/strafe anim.
    //   For sneak, a short latch extends the gate after key-up so quick taps
    //   (press+release between two sendAnimFlags calls) still encode velocity.
    //
    // VELOCITY GATE:
    //   15 u/s in 3rd-person kills yawChange micro-velocity from camera pans.
    //   5 u/s for 1st-person where the artifact is absent.
    // Detect jump / airborne state up front so movement encoding can preserve
    // mid-air momentum after the key is released. Without this, a running jump
    // that releases W mid-flight sends fwd/side=0 after the 100 ms tap latch
    // expires even while planar speed is still ~200 u/s, making the remote actor
    // freeze in place until the next input packet.
    const bool jumpRequested = world->getPlayer().getJumping();
    const bool physicallyAirborne = !world->isOnGround(player)
        && !world->isSwimming(player)
        && !world->isFlying(player);
    int ccJumpState = -1;
    int ccJumpVisual = 0;
    bool hasCcJumpState = false;
    bool hasCcJumpVisual = false;
    if (auto* baseNode = player.getRefData().getBaseNode())
    {
        hasCcJumpState = baseNode->getUserValue("mp_cc_jump_state", ccJumpState);
        hasCcJumpVisual = baseNode->getUserValue("mp_cc_jump_visual", ccJumpVisual);
    }
    const bool ccJumpInAir = hasCcJumpState && ccJumpState == 1;
    const bool ccJumpVisualPulse = hasCcJumpVisual && ccJumpVisual != 0;
    if (ccJumpVisualPulse)
        mCcJumpVisualLatch = CC_JUMP_VISUAL_LATCH_TIME;
    else if (mCcJumpVisualLatch > 0.f)
        mCcJumpVisualLatch = std::max(0.f, mCcJumpVisualLatch - dt);
    const bool ccJumpVisualLatched = mCcJumpVisualLatch > 0.f;
    if (jumpRequested)
        mJumpGraceTimer = JUMP_GRACE_TIME;
    else if (mJumpGraceTimer > 0.f)
        mJumpGraceTimer = std::max(0.f, mJumpGraceTimer - dt);

    float fwd = 0.f, side = 0.f;
    bool wallBlocked = false;
    bool rawHasMovementInput = false;
    float planarSpeed = 0.f;
    float velocityGate = 5.f;
    float blockedMoveSpeed = 0.f;
    {
        // WASD gate — discard projection when no movement key is held.
        // Purpose: prevent camera-orbit (move360.lua yawChange ~18–35 u/s) from
        // triggering walk/strafe anims when the player is only rotating the view.
        bool kFwd = false, kBack = false, kLeft = false, kRight = false;
        if (auto inputMgr = MWBase::Environment::get().getInputManager())
        {
            kFwd  = inputMgr->actionIsActive(MWInput::A_MoveForward);
            kBack = inputMgr->actionIsActive(MWInput::A_MoveBackward);
            kLeft = inputMgr->actionIsActive(MWInput::A_MoveLeft);
            kRight= inputMgr->actionIsActive(MWInput::A_MoveRight);
            rawHasMovementInput = kFwd || kBack || kLeft || kRight;
        }

        // Movement tap latch: keep the WASD gate open for a short window after the
        // last real key press (any stance — walk, run, or sneak).
        //
        // Problem: a quick directional tap (press+release between two sendAnimFlags
        // calls, ~16 ms at 60 fps) leaves rawHasMovementInput==false by the time we
        // run.  The WASD gate fires and zeroes fwd/side even though the body still has
        // full momentum.  This applies to all stances, not only sneak — slow walking
        // "inching" and quick non-sneak taps suffer the same blackout.
        //
        // Fix: on any real key press, charge the latch for MOVE_LATCH_TIME.  While the
        // latch is live, keep the projection active so velocity naturally decays to zero
        // rather than being gate-zeroed.  The camera-orbit false-trigger is harmless
        // because the latch only opens after a genuine key press.
        if (rawHasMovementInput)
            mMoveLatch = MOVE_LATCH_TIME;
        else if (mMoveLatch > 0.f)
            mMoveLatch -= dt;

        const float vx  = mLocal.velocity.linear[0];
        const float vy  = mLocal.velocity.linear[1];
        const float spd = std::sqrt(vx * vx + vy * vy);
        planarSpeed = spd;

        const bool is3rdPerson = world->getCamera() && !world->isFirstPerson();
        // velocityGate: still used to filter camera-orbit body displacement when the
        // movement latch is live but no real key is held (Tab-orbit produces ~18-35 u/s).
        velocityGate = is3rdPerson ? 15.0f : 5.0f;

        // Preserve airborne momentum even after key release.
        // Locally, the jump continues to travel forward under inertia; encoding
        // idle mid-flight makes the remote switch to pure fall and then snap to
        // the next non-idle position. Camera-orbit false positives are not a
        // concern here because this path is only active while truly airborne.
        const bool preserveAirMomentum = (hasCcJumpState ? ccJumpInAir : (jumpRequested || physicallyAirborne))
            && spd > velocityGate;
        const bool hasMovementInput = rawHasMovementInput || (mMoveLatch > 0.f) || preserveAirMomentum;

        // Wall-block detection: compare physics speed against the actor's real
        // expected pace for the current movement state, not hardcoded 50/120 values.
        //
        // This fixes the crouch regression from the previous hardcoded threshold:
        // a slow sneaking player might legitimately top out around 25 u/s, so
        // comparing against a fixed 40 u/s tagged ordinary sneak-walk as "blocked"
        // every frame. Using getMaxSpeed(player) keeps the threshold aligned with
        // the sender's actual stats, stance, encumbrance, and settings.
        const float expectedMoveSpeed = player.getClass().getMaxSpeed(player);
        blockedMoveSpeed = expectedMoveSpeed;
        const float wallSpeedThreshold = std::max(velocityGate, expectedMoveSpeed * 0.8f);
        wallBlocked = rawHasMovementInput
            && expectedMoveSpeed > velocityGate
            && spd < wallSpeedThreshold;

        if (hasMovementInput && spd > velocityGate && !wallBlocked)
        {
            // Always project onto body yaw — immune to camera-orbit direction flips.
            // Divide by spd (not walkSpeed) so the output is a unit-direction vector
            // in body-local space. wallBlocked already catches sub-walkspeed cases
            // (wall contact, accel ramp) so any speed reaching here is >= 80% of walk.
            const float projYaw = mLocal.position.rot[2];
            const float sinYaw = std::sin(projYaw);
            const float cosYaw = std::cos(projYaw);

            float rawSide = (vx * cosYaw - vy * sinYaw);
            float rawFwd  = (vx * sinYaw + vy * cosYaw);

            side = rawSide / spd;  // unit-direction component
            fwd  = rawFwd  / spd;
        }
        else if (rawHasMovementInput)
        {
            // Wall-blocked (or below velocity gate with keys held).
            // DO NOT use mov.mPosition[0/1] here — in 3rd-person move360 mode,
            // mPosition is computed from world-space velocity / camera direction by
            // Lua, so it collapses toward zero whenever the physics collision kills
            // the velocity (e.g. pressing into a wall). The remote NPC would see
            // fwd=0/side=0 and play idle animation despite keys being held.
            //
            // Instead, derive axes directly from which keys are physically pressed.
            // This is completely immune to physics, Lua, and camera-mode artifacts:
            // the sender's intent (key held = moving) is always transmitted correctly.
            fwd = kFwd ? 1.f : (kBack ? -1.f : 0.f);
            side = kRight ? 1.f : (kLeft ? -1.f : 0.f);
        }
    }

    // Send raw body-relative axes — no classification on the sender.
    // The receiver sets mPosition[0/1] directly so the remote CC runs its own
    // strafe/forward/backward logic, identical to a local 1st-person player.
    // Values are already normalised to [-1,1] from the velocity projection above.
    const float verticalSpeed = std::abs(mLocal.velocity.linear[2]);
    const bool stalledCornerJump = physicallyAirborne
        && rawHasMovementInput
        && wallBlocked
        && planarSpeed < velocityGate
        && verticalSpeed < 10.f;
    if (stalledCornerJump)
        mJumpStallTimer += dt;
    else
        mJumpStallTimer = 0.f;

    const bool suppressJumpForCornerStall = (mJumpGraceTimer <= 0.f)
        && (mJumpStallTimer >= JUMP_STALL_TIME);
    const bool cornerPinnedJump = rawHasMovementInput
        && wallBlocked
        && planarSpeed < velocityGate
        && verticalSpeed < 25.f;
    const bool jumpingFlag = (hasCcJumpState || hasCcJumpVisual)
        ? (ccJumpInAir || ccJumpVisualLatched)
        : ((jumpRequested || physicallyAirborne)
            && !cornerPinnedJump
            && !suppressJumpForCornerStall);

    f.animFwd  = fwd;
    f.animSide = side;
    f.blockedMoveSpeed = wallBlocked ? blockedMoveSpeed : 0.f;

    // movementFlags bitmask
    if (stats.getMovementFlag(MWMechanics::CreatureStats::Flag_Run))
        f.movementFlags |= AnimFlags::MF_RUN;
    if (stats.getMovementFlag(MWMechanics::CreatureStats::Flag_Sneak))
        f.movementFlags |= AnimFlags::MF_SNEAK;
    if (wallBlocked)
        f.movementFlags |= AnimFlags::MF_WALL_BLOCKED;

    {
        const float spd = std::sqrt(mLocal.velocity.linear[0]*mLocal.velocity.linear[0] + mLocal.velocity.linear[1]*mLocal.velocity.linear[1]);
        if (spd > 0.1f)
        {
            Log(Debug::Info) << "[MPDBG] Sender Flags: fwd=" << f.animFwd << " side=" << f.animSide 
                             << " spd=" << spd << " sneak=" << ((f.movementFlags & AnimFlags::MF_SNEAK) != 0)
                             << " wallBlocked=" << ((f.movementFlags & AnimFlags::MF_WALL_BLOCKED) != 0)
                             << " blockedSpeed=" << f.blockedMoveSpeed
                              << " ccJump=" << ccJumpState
                             << " ccJumpVisual=" << ccJumpVisual
                             << " ccJumpLatch=" << mCcJumpVisualLatch
                             << " vz=" << mLocal.velocity.linear[2]
                             << " mode=" << (int)world->getCamera()->getMode();
        }
    }
    // Use getPlayer().getJumping() — set by PhysicsSystem::handleJump() the same
    // frame the jump impulse fires, before the actor physically leaves the ground.
    // This matches when the local CharacterController starts the jump animation,
    // removing the ~50ms delay that came from waiting for !isOnGround().
    // Fallback: also catch free-falls (walking off a ledge) via !isOnGround(),
    // but exclude swimming and flying which are non-jump airborne states.
    if (jumpingFlag)
        f.movementFlags |= AnimFlags::MF_JUMP;

    // Levitation / flying — keep the remote NPC off the ground while airborne.
    // Without this, the receiver's physics resets onGround=true every frame and
    // the NPC gets shoved into the ground geometry, triggering the engine's
    // "actor stuck" knockout path.
    if (world->isFlying(player))
        f.movementFlags |= AnimFlags::MF_FLY;

    // MF_STRAFING is derived on the receiver from animFwd/animSide directly,
    // mirroring the vanilla CC criterion — no need to encode it from the sender.

    // Knockdown / knockout — sustained hit-states driven by vanilla CC.
    // Encoding them in the regular unreliable stream means they self-clear
    // as soon as the sender recovers, without any separate handshake.
    if (stats.getKnockedDown())
        f.movementFlags |= AnimFlags::MF_KNOCKED_DOWN;
    // Knockout condition matches character.cpp: fatigue < 0 OR base == 0.
    if (stats.getFatigue().getCurrent() < 0.f || stats.getFatigue().getBase() == 0.f)
        f.movementFlags |= AnimFlags::MF_KNOCKED_OUT;

    // actionFlags bitmask
    if (stats.getAttackingOrSpell())
        f.actionFlags |= AnimFlags::AF_ATTACKING;
    if (stats.getDrawState() == MWMechanics::DrawState::Weapon)
        f.actionFlags |= AnimFlags::AF_WEAPON_DRAWN;
    if (stats.getDrawState() == MWMechanics::DrawState::Spell)
        f.actionFlags |= AnimFlags::AF_SPELL_READY;

    // ── Jump edge detection ────────────────────────────────────────────────
    // Must run BEFORE the delta-suppression early-return so edges are never
    // silently dropped, even when the only thing that changed is the jump bit.
    const bool wasJumping = (mLastAnimFlags.movementFlags & AnimFlags::MF_JUMP) != 0;
    const bool nowJumping = (f.movementFlags             & AnimFlags::MF_JUMP) != 0;
    const bool jumpEdge   = (wasJumping != nowJumping);
    if (jumpEdge)
        Log(Debug::Info) << "[MP] AnimFlags MF_JUMP "
                         << (nowJumping ? "0->1 (rising)" : "1->0 (falling)")
                         << " -- sending reliably";

    // ── Movement edge detection ────────────────────────────────────────────────
    // Any idle↔moving boundary packet is promoted to reliable, regardless of
    // stance.  The same UDP-loss race that was fixed for sneak also affects
    // upright walking and running: a quick tap produces exactly 1 start and 1
    // stop packet; if either is dropped the remote CC never sees the motion.
    //
    // Removing the `isSneak &&` guard makes the fix universal.  In-between
    // packets during sustained movement remain unreliable — no bandwidth
    // regression.  "Moving" is defined as either axis being non-zero after
    // the velocity gate and WASD gate above; exactly 0.f means truly idle.
    const bool wasMoving = (mLastAnimFlags.animFwd != 0.f || mLastAnimFlags.animSide != 0.f);
    const bool nowMoving = (f.animFwd != 0.f || f.animSide != 0.f);
    const bool moveEdge  = (wasMoving != nowMoving);
    if (moveEdge)
        Log(Debug::Info) << "[MP] AnimFlags move edge "
                         << (nowMoving ? "idle->moving" : "moving->idle")
                         << " sneak=" << ((f.movementFlags & AnimFlags::MF_SNEAK) != 0)
                         << " fwd=" << f.animFwd << " side=" << f.animSide
                         << " -- sending reliably";

    // Periodic refresh: force send every ANIM_REFRESH_RATE seconds even with
    // no delta, so a UDP-lost packet can't permanently strand the receiver.
    mAnimRefreshTimer += dt;
    const bool forceRefresh = (mAnimRefreshTimer >= ANIM_REFRESH_RATE);
    if (forceRefresh)
        mAnimRefreshTimer = 0.f;

    // Float comparison for delta-suppression: treat change < 0.05 as no-change
    // to avoid sending for tiny float noise while still catching real moves.
    // Jump edges and movement edges bypass this guard — they must always be delivered.
    if (!forceRefresh && !jumpEdge && !moveEdge
        && f.movementFlags == mLastAnimFlags.movementFlags
        && f.actionFlags   == mLastAnimFlags.actionFlags
        && std::abs(f.animFwd  - mLastAnimFlags.animFwd)  < 0.05f
        && std::abs(f.animSide - mLastAnimFlags.animSide) < 0.05f
        && std::abs(f.blockedMoveSpeed - mLastAnimFlags.blockedMoveSpeed) < 1.f)
        return;

    mLastAnimFlags   = f;
    mLocal.animFlags = f;

    PacketPlayerAnimFlags pkt;
    pkt.setPlayer(&mLocal);
    // Jump-state transitions and movement edges (idle↔moving) are reliable;
    // all other anim changes are unreliable.
    if (jumpEdge || moveEdge)
        mClient.sendReliable(pkt.encode(mSeqCounter++));
    else
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
        || std::abs(mLocal.animFlags.animFwd  - mLastAnimFlags.animFwd)  >= 0.05f
        || std::abs(mLocal.animFlags.animSide - mLastAnimFlags.animSide) >= 0.05f
        || std::abs(mLocal.animFlags.blockedMoveSpeed - mLastAnimFlags.blockedMoveSpeed) >= 1.f;
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
