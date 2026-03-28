#include "RemotePlayer.hpp"

#include <algorithm>
#include <cmath>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/misc/constants.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwbase/windowmanager.hpp"
#include "../../mwgui/mode.hpp"
#include "../../mwbase/mechanicsmanager.hpp"
#include "../../mwbase/rotationflags.hpp"
#include "../../mwworld/ptr.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/cell.hpp"
#include "../../mwworld/manualref.hpp"
#include "../../mwworld/class.hpp"
#include "../../mwworld/esmstore.hpp"
#include <components/sceneutil/positionattitudetransform.hpp>
#include "../../mwmechanics/movement.hpp"
#include "../../mwmechanics/creaturestats.hpp"
#include "../../mwmechanics/aisetting.hpp"
#include "../../mwmechanics/character.hpp"
#include "../../mwrender/animation.hpp"
#include "../../mwrender/blendmask.hpp"
#include "../../mwrender/animationpriority.hpp"
#include "../../mwworld/containerstore.hpp"
#include "../../mwworld/esmstore.hpp"
#include <components/esm3/loadnpc.hpp>

namespace mwmp
{

namespace {
    // Shortest-path lerp between two angles (radians).
    // Guarantees interpolation never takes the long way around 0/2π.
    inline float lerpAngle(float current, float target, float alpha)
    {
        constexpr float TWO_PI = 6.28318530718f;
        constexpr float PI     = 3.14159265359f;
        float diff = target - current;
        while (diff >  PI) diff -= TWO_PI;
        while (diff < -PI) diff += TWO_PI;
        return current + diff * alpha;
    }

}

// ---------------------------------------------------------------------------
RemotePlayer::RemotePlayer(uint32_t guid, const std::string& name)
    : mGuid(guid), mName(name)
{
    mState.guid = guid;
    mState.name = name;
    mState.animFlags.animFwd  = 0.f;
    mState.animFlags.animSide = 0.f;
    mLastAppliedAnimFlags.animFwd  = 0.f;
    mLastAppliedAnimFlags.animSide = 0.f;
    Log(Debug::Info) << "[MP] RemotePlayer created: " << name << " (guid=" << guid << ")";
}

RemotePlayer::~RemotePlayer()
{
    despawnFromWorld();
    Log(Debug::Info) << "[MP] RemotePlayer removed: " << mName;
}

// ---------------------------------------------------------------------------
void RemotePlayer::update(float dt)
{
    updateInterpolation(dt);

    if (mIsSpawned)
    {
        applyInterpolationToWorld();
        ensureMechanicsRegistration();
        applyAnimationStateToActor();

        // Suppress AI every frame — MechanicsManager::add() re-activates
        // the AI sequence, so a one-time clear() at spawn time isn't enough.
        // Without this the remote NPC will greet, wander, enter combat, etc.
        // Matches TES3MP DedicatedPlayer::update() suppression block exactly.
        if (mMechanicsRegistered)
        {
            MWMechanics::CreatureStats& cs = mNpcPtr.getClass().getCreatureStats(mNpcPtr);
            cs.getAiSequence().stopCombat();
            cs.setAttacked(false);
            cs.setAlarmed(false);
            cs.setAiSetting(MWMechanics::AiSetting::Alarm, 0);
            cs.setAiSetting(MWMechanics::AiSetting::Fight, 0);
            cs.setAiSetting(MWMechanics::AiSetting::Flee,  0);
            cs.setAiSetting(MWMechanics::AiSetting::Hello, 0);
        }

        // The OSG base node is attached to the scene graph one frame after
        // placeObject() returns, so the first trySpawn() attempt always misses
        // it. Retry attachment every frame until it appears — this is cheap
        // (one pointer check) and self-limiting once the nameplate is created.
        if (!mNameplate && !mNpcPtr.isEmpty())
        {
            if (auto* baseNode = mNpcPtr.getRefData().getBaseNode())
            {
                baseNode->setUserValue("mp_player_name", mName);
                mNameplate = std::make_unique<Nameplate>(baseNode, mName);
                Log(Debug::Info) << "[MP] RemotePlayer " << mName
                                 << ": nameplate attached (deferred)";
            }
        }
    }
    else
    {
        mSpawnRetryTimer += dt;
        if (mSpawnRetryTimer >= SPAWN_RETRY_RATE)
        {
            mSpawnRetryTimer = 0.f;
            trySpawn();
        }
    }
}

// ---------------------------------------------------------------------------
void RemotePlayer::ensureMechanicsRegistration()
{
    if (!mIsSpawned || mNpcPtr.isEmpty() || mMechanicsRegistered)
        return;

    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world)
        return;

    auto* baseNode = mNpcPtr.getRefData().getBaseNode();
    if (!baseNode)
        return;

    baseNode->setUserValue("mp_player_name", mName);

    MWRender::Animation* anim = world->getAnimation(mNpcPtr);
    if (!anim)
        return;

    MWBase::Environment::get().getMechanicsManager()->add(mNpcPtr);
    mMechanicsRegistered = true;

    Log(Debug::Info) << "[MP] RemotePlayer " << mName
                     << ": registered with MechanicsManager (deferred)";
}

// ---------------------------------------------------------------------------
void RemotePlayer::applyAnimationStateToActor()
{
    if (!mIsSpawned || mNpcPtr.isEmpty())
        return;

    const AnimFlags& f = mState.animFlags;

    MWMechanics::Movement& mov
        = mNpcPtr.getClass().getMovementSettings(mNpcPtr);

    // ── Dead-band clamp ────────────────────────────────────────────────────────
    // The idle breathing/sway animation produces sub-threshold world-space
    // velocity on the sender (~0.5–2 u/s). Even though PlayerSync::sendAnimFlags
    // gates on 5 u/s before projecting, small residuals can still arrive as
    // animFwd/animSide values in the 0.01–0.04 range after the scale step.
    // Passing those directly into mPosition[0/1] is enough for the vanilla CC
    // to start a partial leg stride that immediately snaps back — the visible
    // "tick" during idle. Clamping to exactly 0 below the dead-band ensures the
    // CC always sees a clean zero-input when the player is truly still.
    // Threshold 0.08: well above idle sway (max ~0.04) and well below the
    // slowest intentional sneak-strafe (~0.40 at 50 u/s / 120 u/s scale).
    constexpr float DEAD_BAND = 0.08f;
    const float effFwd  = (std::abs(f.animFwd)  > DEAD_BAND) ? f.animFwd  : 0.f;
    const float effSide = (std::abs(f.animSide) > DEAD_BAND) ? f.animSide : 0.f;

    mov.mPosition[1] = effFwd;
    mov.mPosition[0] = effSide;

    // ── Strafe hysteresis ──────────────────────────────────────────────────────
    // Problem: during A/D crouch-strafe in 3rd-person, move360.lua produces
    // a moderate diagonal (e.g. fwd=0.35, side=0.55). With a narrow hysteresis
    // band the old (enter=2.5x, exit=1.5x) mIsStrafing flips multiple times per
    // stride, causing the CC to restart the strafe anim mid-cycle — the "tripping"
    // effect. Widening to enter=2.0x (vanilla) / exit=4.0x gives a large dead zone
    // so a stride always completes before a state flip can occur.
    //
    // Pure-lateral guard: if fwd is at/below the dead-band but side is intentional,
    // lock into strafing regardless of the ratio.  Covers direct A/D where fwd
    // is genuinely 0 and the ratio test would need an infinite threshold.
    //
    // Full-stop reset: when both axes are zero, clear mIsStrafing so the next
    // movement direction starts fresh with no stale strafe-lock.
    const float absFwd  = std::abs(effFwd);
    const float absSide = std::abs(effSide);
    const bool bothZero = (absFwd < 0.001f && absSide < 0.001f);

    if (bothZero)
    {
        mIsStrafing = false;
    }
    else if (mIsStrafing)
    {
        // Require Fwd to be 4x larger than Side to BREAK strafing.
        if (absFwd > absSide * 4.0f)
            mIsStrafing = false;
    }
    else
    {
        // Enter strafing: Side >= 2x Fwd (vanilla), OR pure lateral (fwd dead-banded).
        if (absSide > absFwd * 2.0f || (absFwd < DEAD_BAND && absSide > DEAD_BAND))
            mIsStrafing = true;
    }
    mov.mIsStrafing = mIsStrafing;

    // Pass the interpolated yaw delta so standing turn animations trigger
    mov.mRotation[0] = 0.f;
    mov.mRotation[1] = 0.f;
    const bool isIdle = (effFwd == 0.f && effSide == 0.f);
    if (isIdle && std::abs(mInterp.yawDelta) > 0.005f)
        mov.mRotation[2] = mInterp.yawDelta;
    else
        mov.mRotation[2] = 0.f;

    // Jump animation — ported from TES3MP's DedicatedPlayer::setAnimFlags().
    //
    // The CharacterController's jump path requires the physics actor to report
    // !onGround so the "in the air" branch fires (character.cpp ~2229).
    // Actors::update() forces internal collision on every NPC each frame, so
    // the physics sim runs and traceDown() sets onGround=true every frame as
    // long as moveObject() keeps the NPC at ground level.  We break that by
    // calling world->setOnGround(false) on the rising edge and (true) on the
    // falling edge — directly writing Actor::mOnGround via the new World API.
    //
    // Flag_ForceJump on CreatureStats is the vanilla signal that makes the CC
    // set mPosition[2] = onground ? 1 : 0 each frame (character.cpp ~2028).
    // With onGround forced false that resolves to 0, so PhysicsSystem::handleJump()
    // never fires — no spurious fatigue drain, no upward impulse.  The CC enters
    // JumpState_InAir purely from the !onGround branch, exactly as locally.
    const bool isJumping = (f.movementFlags & AnimFlags::MF_JUMP) != 0;
    const bool isFlying  = (f.movementFlags & AnimFlags::MF_FLY)  != 0;

    // IMPORTANT: write Flag_ForceJump onto CreatureStats BEFORE calling
    // setOnGround().  On the rising edge (grounded→jumping), setOnGround(false)
    // makes the CC immediately see an actor that is airborne but has not yet had
    // its ForceJump flag set.  For that single frame the controller interprets
    // the state as "just fell and landed" and plays the landing thud/sound.
    // Committing the jump intent first means the CC always sees ForceJump=true
    // at the same time it sees !onGround, matching the local jump path exactly.
    // (These lines are moved up from below; the full stats block follows later.)
    {
        MWMechanics::CreatureStats& statsEarly = mNpcPtr.getClass().getCreatureStats(mNpcPtr);
        statsEarly.setMovementFlag(MWMechanics::CreatureStats::Flag_ForceJump, isJumping);
        statsEarly.setMovementFlag(MWMechanics::CreatureStats::Flag_ForceMoveJump, isJumping);
    }

    // While flying/levitating, keep the actor off the ground so the physics
    // engine doesn't snap them down and trigger the "stuck" knockout path.
    // When both jumping and flying are false, restore normal ground contact.
    if (isFlying && !isJumping)
    {
        MWBase::Environment::get().getWorld()->setOnGround(mNpcPtr, false);
    }
    else if (isJumping != mWasJumping)
    {
        if (!isJumping)
        {
            // Drain mFallHeight that mtphysics accumulated during the remote jump arc.
            // Without this the landing path in character.cpp gets a spuriously large
            // height value → fall damage → knockdown even when the sender landed safely.
            mNpcPtr.getClass().getCreatureStats(mNpcPtr).land(false);
        }
        MWBase::Environment::get().getWorld()->setOnGround(mNpcPtr, !isJumping);
        mWasJumping = isJumping;
    }
    else if (!isJumping && !isFlying && mWasFlying)
    {
        // Landing from levitation — restore ground contact and clear fall height.
        mNpcPtr.getClass().getCreatureStats(mNpcPtr).land(false);
        MWBase::Environment::get().getWorld()->setOnGround(mNpcPtr, true);
    }
    mWasFlying = isFlying;


    MWMechanics::CreatureStats& stats = mNpcPtr.getClass().getCreatureStats(mNpcPtr);
    // Flag_ForceJump / Flag_ForceMoveJump already set above (before setOnGround).
    // Re-applying here is harmless but kept for clarity as the canonical stats block.
    stats.setMovementFlag(MWMechanics::CreatureStats::Flag_ForceJump, isJumping);
    stats.setMovementFlag(MWMechanics::CreatureStats::Flag_ForceMoveJump, isJumping);
    stats.setMovementFlag(MWMechanics::CreatureStats::Flag_Run,
        (f.movementFlags & AnimFlags::MF_RUN) != 0);
    stats.setMovementFlag(MWMechanics::CreatureStats::Flag_Sneak,
        (f.movementFlags & AnimFlags::MF_SNEAK) != 0);
    stats.setAttackingOrSpell(
        (f.actionFlags & AnimFlags::AF_ATTACKING) != 0 || mState.attack.pressed);

    // ── Knockdown / knockout ───────────────────────────────────────────────────
    // Mirror the sender's live CharacterController hit-state so the receiver's
    // CC plays the correct anim group and recovers at the right time.
    //
    // MF_KNOCKED_OUT  → fatigue forced negative → CC enters looping KnockOut
    // MF_KNOCKED_DOWN → stats.setKnockedDown    → CC enters brief KnockDown
    // Neither flag    → both cleared; CC returns to normal locomotion
    //
    // When either flag is active we zero movement so the locomotion branch
    // of the CC doesn't fight the hit-state anim.
    const bool isKnockedOut  = (f.movementFlags & AnimFlags::MF_KNOCKED_OUT)  != 0;
    const bool isKnockedDown = (f.movementFlags & AnimFlags::MF_KNOCKED_DOWN) != 0;

    if (isKnockedOut || isKnockedDown)
    {
        // Zero movement so the CC's locomotion branch doesn't fight the hit anim
        mov.mPosition[0] = 0.f;
        mov.mPosition[1] = 0.f;
        mov.mIsStrafing  = false;

        stats.setKnockedDown(isKnockedDown && !isKnockedOut);

        if (isKnockedOut)
        {
            // Drive CC into looping KnockOut by holding fatigue negative.
            // DynamicStats sync never pushes fatigue back to the NPC's
            // CreatureStats, so this write has no conflict with stats sync.
            // When the flag clears, vanilla fatigue regen restores naturally.
            MWMechanics::DynamicStat<float> fat = stats.getFatigue();
            if (fat.getCurrent() >= 0.f)
            {
                fat.setCurrent(-1.f);
                stats.setFatigue(fat);
            }
        }
    }
    else
    {
        // Recovery: clear knockdown flag so CC exits hit-state naturally.
        stats.setKnockedDown(false);
        // Also restore fatigue we forced negative for MF_KNOCKED_OUT.
        // Vanilla regen alone is too slow (~seconds) — the CC won't exit
        // KnockOut state until fatigue >= 0, so explicitly restore it on
        // the first frame the flag is clear.
        MWMechanics::DynamicStat<float> fat = stats.getFatigue();
        if (fat.getCurrent() < 0.f)
        {
            fat.setCurrent(1.f);
            stats.setFatigue(fat);
        }
    }

    MWMechanics::DrawState ds = MWMechanics::DrawState::Nothing;
    if (f.actionFlags & AnimFlags::AF_WEAPON_DRAWN)
        ds = MWMechanics::DrawState::Weapon;
    else if (f.actionFlags & AnimFlags::AF_SPELL_READY)
        ds = MWMechanics::DrawState::Spell;
    stats.setDrawState(ds);
}

// ---------------------------------------------------------------------------
// Attempt to place the NPC in the world.
// We only spawn when we know the remote player's cell and they share
// the same active cell as the local player.
void RemotePlayer::trySpawn()
{
    // Don't attempt anything while the local client is still loading a cell.
    // The loading screen being present means the CellStore isn't fully
    // initialised yet — placeObject() during load risks crashes and the
    // isInSameCellAsLocalPlayer() check is meaningless anyway.
    if (MWBase::Environment::get().getWindowManager()->containsMode(MWGui::GM_Loading))
        return;

    if (mState.cell.cellName.empty() && !mState.cell.isExterior)
    {
        Log(Debug::Verbose) << "[MP] trySpawn(" << mName << "): no cell yet";
        return;
    }

    if (mState.position.pos[0] == 0.f
        && mState.position.pos[1] == 0.f
        && mState.position.pos[2] == 0.f)
    {
        Log(Debug::Verbose) << "[MP] trySpawn(" << mName << "): position is zero, waiting";
        return;
    }

    if (!isInSameCellAsLocalPlayer(/*quiet=*/true))
    {
        Log(Debug::Verbose) << "[MP] trySpawn(" << mName << "): not in same cell (remote='"
                            << mState.cell.cellName << "' ext=" << mState.cell.isExterior << ")";
        return;
    }

    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) { Log(Debug::Warning) << "[MP] trySpawn(" << mName << "): world null"; return; }

    MWWorld::Ptr localPlayer = world->getPlayerPtr();
    if (localPlayer.isEmpty()) { Log(Debug::Warning) << "[MP] trySpawn(" << mName << "): local player empty"; return; }
    if (!localPlayer.isInCell()) { Log(Debug::Warning) << "[MP] trySpawn(" << mName << "): local player not in cell"; return; }

    MWWorld::CellStore* cell = localPlayer.getCell();
    if (!cell) { Log(Debug::Warning) << "[MP] trySpawn(" << mName << "): local CellStore null"; return; }

    Log(Debug::Info) << "[MP] trySpawn(" << mName << "): attempting spawn at ("
                     << mState.position.pos[0] << ", " << mState.position.pos[1] << ", "
                     << mState.position.pos[2] << ") in '" << mState.cell.cellName << "'";

    // Always spawn at authoritative state position.
    // The interpolator will smoothly move from there each frame.
    ESM::Position pos{};
    pos.pos[0] = mState.position.pos[0];
    pos.pos[1] = mState.position.pos[1];
    pos.pos[2] = mState.position.pos[2];
    pos.rot[0] = 0.f;
    pos.rot[1] = 0.f;
    pos.rot[2] = mState.position.rot[2];

    // Snap interpolation to this position too
    mInterp.cx = pos.pos[0];
    mInterp.cy = pos.pos[1];
    mInterp.cz = pos.pos[2];
    mInterp.tx = pos.pos[0];
    mInterp.ty = pos.pos[1];
    mInterp.tz = pos.pos[2];

    try
    {
        // Build a unique NPC record for this remote player so they display their
        // own race/face/hair rather than the local player's appearance.
        // Strategy (mirrors TES3MP RecordHelper::createRecord / overrideRecord):
        //   1. Clone the "player" ESM::NPC record as an animation-compatible base.
        //   2. Overwrite appearance fields with what we received in PacketPlayerBaseInfo.
        //   3. Assign a deterministic unique ID based on this player's GUID so the
        //      record survives the lifetime of this RemotePlayer and is stable across
        //      re-spawns within the same session.
        //   4. Insert/update the record in ESMStore via overrideRecord(), then spawn.
        const std::string npcRecordId = "mp_remote_" + std::to_string(mGuid);

        // Copy the player template for animation skeleton/flags/class defaults
        const ESM::NPC* playerTemplate
            = world->getStore().get<ESM::NPC>().find(ESM::RefId::stringRefId("player"));
        ESM::NPC npcRecord = *playerTemplate;

        // Assign unique ID and override appearance from received base info
        npcRecord.mId   = ESM::RefId::stringRefId(npcRecordId);
        npcRecord.mName = mState.name;

        // Race / head / hair — only override if the server sent non-empty strings.
        // An empty string here means PacketPlayerBaseInfo hasn't arrived yet;
        // in that case fall back to the player template so we at least get a
        // valid NPC shape while waiting for the packet.
        if (!mState.race.empty())
            npcRecord.mRace = ESM::RefId::deserializeText(mState.race);
        if (!mState.headMesh.empty())
            npcRecord.mHead = ESM::RefId::deserializeText(mState.headMesh);
        if (!mState.hairMesh.empty())
            npcRecord.mHair = ESM::RefId::deserializeText(mState.hairMesh);

        // Sex flag — ESM::NPC::Female == 0x01; clear for male, set for female
        if (mState.isMale)
            npcRecord.mFlags &= ~static_cast<unsigned char>(ESM::NPC::Female);
        else
            npcRecord.mFlags |=  static_cast<unsigned char>(ESM::NPC::Female);

        // Insert (or update if already exists from a previous spawn) the record
        world->getStore().overrideRecord(npcRecord);

        Log(Debug::Info) << "[MP] RemotePlayer " << mName
                         << ": NPC record '" << npcRecordId << "'"
                         << " race='" << mState.race << "'"
                         << " head='" << mState.headMesh << "'"
                         << " hair='" << mState.hairMesh << "'"
                         << " male=" << mState.isMale;

        MWWorld::ManualRef ref(world->getStore(),
                               ESM::RefId::stringRefId(npcRecordId), 1);

        mNpcPtr = world->placeObject(ref.getPtr(), cell, pos);

        if (mNpcPtr.isEmpty())
        {
            Log(Debug::Warning) << "[MP] RemotePlayer " << mName
                                << ": placeObject returned empty Ptr";
            return;
        }

        // Clear all AI packages — remote players are driven by network, not AI
        mNpcPtr.getClass().getCreatureStats(mNpcPtr).getAiSequence().clear();

        // Mark this NPC as a network-driven remote player so that CharacterController
        // always takes the first-person movement code path (no TurnToMovementDirection
        // biped logic, no turn animations, instant smooth-movement response). Without
        // this the CC fights the anim flags we set in applyAnimationStateToActor and
        // produces spurious strafing and turn animations when the remote client is in
        // third-person (move360.lua rotates the input before it reaches mPosition).
        mNpcPtr.getClass().getCreatureStats(mNpcPtr).setMovementFlag(
            MWMechanics::CreatureStats::Flag_NetworkPlayerNpc, true);

        // Disable collision so remote players don't physically block the local player
        //world->setActorCollisionMode(mNpcPtr, false, false);


        mIsSpawned = true;
        mMechanicsRegistered = false;

        // Attach world-space nameplate above the NPC's head.
        // Also tag the base node with the player's network name so that
        // Npc::getName() can return it instead of the generic ESM record name.
        if (auto* baseNode = mNpcPtr.getRefData().getBaseNode())
        {
            baseNode->setUserValue("mp_player_name", mName);
            mNameplate = std::make_unique<Nameplate>(baseNode, mName);
        }
        else
            Log(Debug::Warning) << "[MP] RemotePlayer " << mName << ": no base node for nameplate";

        Log(Debug::Info) << "[MP] RemotePlayer " << mName << ": spawned in world at ("
                         << pos.pos[0] << ", " << pos.pos[1] << ", " << pos.pos[2] << ")";
    }
    catch (const std::exception& e)
    {
        Log(Debug::Warning) << "[MP] RemotePlayer " << mName
                            << ": spawn failed: " << e.what();
    }
}

// ---------------------------------------------------------------------------
void RemotePlayer::despawnFromWorld()
{
    if (!mIsSpawned) return;

    // Detach nameplate before deleting the NPC so the parent node is still valid
    mNameplate.reset();

    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (world && !mNpcPtr.isEmpty())
    {
        try
        {
            world->deleteObject(mNpcPtr);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[MP] RemotePlayer " << mName
                                << ": despawn error: " << e.what();
        }
    }

    mNpcPtr          = MWWorld::Ptr();
    mIsSpawned           = false;
    mMechanicsRegistered = false;
    mWasJumping          = false; // reset so re-spawn doesn't skip the first jump edge
    mSpawnRetryTimer     = SPAWN_RETRY_RATE; // attempt immediately on next update
    Log(Debug::Info) << "[MP] RemotePlayer " << mName << ": despawned from world";
}

// ---------------------------------------------------------------------------
// Called every frame when the NPC is in the world.
// Applies the current interpolated position/rotation.
void RemotePlayer::applyInterpolationToWorld()
{
    if (mNpcPtr.isEmpty())
    {
        Log(Debug::Warning) << "[MP] RemotePlayer " << mName << ": NPC Ptr became empty, clearing spawn flag";
        mIsSpawned = false;
        return;
    }

    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return;

    // Check if still in same cell — despawn if not.
    // Pass quiet=true to suppress the per-branch verbose log here;
    // trySpawn() will log the mismatch reason on the next retry.
    if (!isInSameCellAsLocalPlayer(true))
    {
        despawnFromWorld();
        return;
    }

    try
    {
        // Move to current interpolated position.
        // movePhysics=false: don't run physics simulation, just teleport.
        // moveToActive=false: don't force the cell to stay active.
        osg::Vec3f newPos(mInterp.cx, mInterp.cy, mInterp.cz);
        // Don't attempt to move until the OSG base node is attached (it arrives
        // one frame after placeObject returns). More critically: movePhysics=true
        // is required — the physics task scheduler runs UpdatePosition every frame
        // and resets the actor's scene node to frameData.mPosition (the physics sim
        // result). Without updating the physics actor position on every move, the
        // scheduler overwrites our setPosition call and the NPC appears frozen at
        // the spawn point. Rotation is unaffected because it lives on a separate
        // attitude node that the physics system never touches.
        if (!mNpcPtr.getRefData().getBaseNode())
            return;

        // Broadcast interpolation speed to CharacterController via user-value.
        // This lets the animation rate track the actual visual speed rather
        // than the stats-based speed which doesn't account for network interp.
        if (auto* bn = mNpcPtr.getRefData().getBaseNode())
            bn->setUserValue("mp_interp_speed", mInterpPlanarSpeed);

        MWWorld::Ptr moved = world->moveObject(mNpcPtr, newPos, /*movePhysics=*/true,
                                               /*moveToActive=*/true);
        // moveObject returns an updated Ptr when the cell changes; keep it if valid
        if (!moved.isEmpty())
            mNpcPtr = moved;



        // Apply rotation (Z = yaw is the important one for humanoids)
        world->rotateObject(mNpcPtr,
                            osg::Vec3f(mInterp.crx, mInterp.cry, mInterp.crz),
                            MWBase::RotationFlag_none);
    }
    catch (const std::exception& e)
    {
        Log(Debug::Warning) << "[MP] RemotePlayer " << mName
                            << ": applyInterpolation error: " << e.what();
        despawnFromWorld();
    }
}

// ---------------------------------------------------------------------------
void RemotePlayer::onBaseInfoUpdate(const BasePlayer& state)
{
    // Track whether any appearance field changed (requires re-spawn to rebuild NPC record)
    const bool appearanceChanged =
        (mState.race     != state.race)     ||
        (mState.headMesh != state.headMesh) ||
        (mState.hairMesh != state.hairMesh) ||
        (mState.isMale   != state.isMale);

    const bool nameChanged = (mState.name != state.name);

    if (!nameChanged && !appearanceChanged)
        return;

    // Always copy all base-info fields
    mState.name     = state.name;
    mState.race     = state.race;
    mState.headMesh = state.headMesh;
    mState.hairMesh = state.hairMesh;
    mState.isMale   = state.isMale;
    mState.scale    = state.scale;
    mName           = state.name;

    Log(Debug::Info) << "[MP] RemotePlayer " << mName
                     << ": base info updated";

    if (!mNpcPtr.isEmpty())
        if (auto* bn = mNpcPtr.getRefData().getBaseNode())
            bn->setUserValue("mp_player_name", mName);

    if (mNameplate)
        mNameplate->updateName(mName);

    // If appearance changed on an already-spawned actor we must despawn and
    // re-spawn so trySpawn() rebuilds the NPC record with the new race/head/hair.
    if (appearanceChanged && mIsSpawned)
    {
        Log(Debug::Info) << "[MP] RemotePlayer " << mName
                         << ": appearance changed — re-spawning for NPC record rebuild";
        despawnFromWorld();
    }
}

// ---------------------------------------------------------------------------
void RemotePlayer::onPositionUpdate(const BasePlayer& state)
{
    // NOTE: PacketPlayerPosition does NOT serialise the cell field.
    // The incoming state.cell is just a zeroed BasePlayer default.
    // Do NOT copy state.cell here — cell ownership belongs exclusively to
    // onCellChange().  Clobbering mState.cell here was the root cause of
    // the remote='' / spurious-despawn bug observed in testing.
    Log(Debug::Verbose) << "[MP] onPositionUpdate(" << mName << " guid=" << mGuid
                        << "): pos=(" << state.position.pos[0] << ","
                        << state.position.pos[1] << "," << state.position.pos[2] << ")"
                        << " knownCell='" << mState.cell.cellName << "'"
                        << " ext=" << mState.cell.isExterior
                        << " grid=(" << mState.cell.gridX << "," << mState.cell.gridY << ")";
    // intentionally NOT touching mState.cell
    mState.position = state.position;
    mState.velocity = state.velocity;

    // Set new interpolation target; record raw received XY for drift cap.
    // When the remote player is idle (both anim axes zero), suppress XY target
    // updates: the sender's move360.lua yawChange causes tiny body rotations at
    // standstill that induce micro-jitter in the sent position, which would make
    // the NPC slide back and forth each packet.  Z and rotation are always kept
    // so gravity/falling and standing-turn animations work correctly.
    const bool isIdleAnim = (mState.animFlags.animFwd  == 0.f)
                         && (mState.animFlags.animSide == 0.f);
    if (!isIdleAnim)
    {
        mInterp.lastRecvX = state.position.pos[0];
        mInterp.lastRecvY = state.position.pos[1];
        mInterp.tx        = state.position.pos[0];
        mInterp.ty        = state.position.pos[1];
    }
    mInterp.tz   = state.position.pos[2];
    mInterp.trx  = state.position.rot[0];
    mInterp.try_ = state.position.rot[1];
    mInterp.trz  = state.position.rot[2];
    mInterp.hasTarget = true;

    // Snap current pos to target on first update (avoid lerping from origin)
    if (!mInterp.hasSnapped)
    {
        mInterp.cx     = mInterp.tx;
        mInterp.cy     = mInterp.ty;
        mInterp.cz     = mInterp.tz;
        mInterp.crx    = mInterp.trx;
        mInterp.cry    = mInterp.try_;
        mInterp.crz    = mInterp.trz;
        mInterp.lastRecvX  = mInterp.tx;
        mInterp.lastRecvY  = mInterp.ty;
        mInterp.hasSnapped = true;
    }
}

// ---------------------------------------------------------------------------
void RemotePlayer::onEquipmentUpdate(const BasePlayer& state)
{
    mState.equipment = state.equipment;
    Log(Debug::Verbose) << "[MP] RemotePlayer " << mName << ": equipment updated";
    // Phase 3: update visible equipment on NPC
}

// ---------------------------------------------------------------------------
void RemotePlayer::onStatsDynamicUpdate(const BasePlayer& state)
{
    mState.dynamicStats = state.dynamicStats;
}

// ---------------------------------------------------------------------------
void RemotePlayer::onCellChange(const BasePlayer& state)
{
    const bool cellNameChanged = (mState.cell.cellName != state.cell.cellName);
    const bool exteriorChanged = (mState.cell.isExterior != state.cell.isExterior);
    const bool gridChanged     = (mState.cell.gridX != state.cell.gridX
                               || mState.cell.gridY != state.cell.gridY);

    const bool actuallyChanged = cellNameChanged || exteriorChanged || gridChanged;

    const std::string oldCell = mState.cell.cellName;
    mState.cell     = state.cell;
    mState.position = state.position;

    // Snap interpolation to new position so trySpawn has valid coordinates
    mInterp.cx  = state.position.pos[0];
    mInterp.cy  = state.position.pos[1];
    mInterp.cz  = state.position.pos[2];
    mInterp.crz = state.position.rot[2];
    mInterp.tx  = mInterp.cx;
    mInterp.ty  = mInterp.cy;
    mInterp.tz  = mInterp.cz;
    mInterp.trz = mInterp.crz;
    mInterp.lastRecvX  = mInterp.cx;
    mInterp.lastRecvY  = mInterp.cy;
    mInterp.hasTarget  = true;
    mInterp.hasSnapped = true;

    if (actuallyChanged)
    {
        Log(Debug::Info) << "[MP] RemotePlayer " << mName
                         << " cell: " << oldCell << " -> " << state.cell.cellName;

        // For adjacent exterior grid crossings, skip the despawn/respawn cycle.
        // Both cells are already loaded by OpenMW, so we can just leave the NPC
        // in the world and let the interpolator walk it across the border naturally.
        // Only despawn for: interior<->exterior transitions, or grid jumps > 1
        // (which indicate a teleport rather than a normal border crossing).
        bool skipDespawn = false;
        if (!exteriorChanged && mState.cell.isExterior && state.cell.isExterior)
        {
            const int dx = std::abs(state.cell.gridX - mState.cell.gridX);
            const int dy = std::abs(state.cell.gridY - mState.cell.gridY);
            if (dx <= Constants::CellGridRadius && dy <= Constants::CellGridRadius)
                skipDespawn = true;
        }

        if (!skipDespawn)
            despawnFromWorld(); // will respawn in new cell via trySpawn()
    }
}

// ---------------------------------------------------------------------------
void RemotePlayer::onDeath(const BasePlayer& /*state*/)
{
    mIsDead = true;
    Log(Debug::Info) << "[MP] RemotePlayer " << mName << " died";
    // Phase 3: play death animation
}

// ---------------------------------------------------------------------------
void RemotePlayer::onResurrect(const BasePlayer& /*state*/)
{
    mIsDead = false;
    Log(Debug::Info) << "[MP] RemotePlayer " << mName << " resurrected";
}

// ---------------------------------------------------------------------------
// movementType encoding:
//   -1=idle  0=fwd  1=back  2=left(strafe)  3=right(strafe)  4=strafe-l  5=strafe-r
//
// mPosition[1] = forward/back axis (+1 = forward, -1 = back)
// mPosition[0] = strafe axis       (+1 = right,   -1 = left)
// mPosition[2] = jump              ( 1 = jumping,   0 = grounded)
//
// Flag_Run / Flag_Sneak on CreatureStats drive CharacterController
// speed selection — they are independent of mPosition.
void RemotePlayer::onAnimFlagsUpdate(const BasePlayer& state)
{
    const AnimFlags& f = state.animFlags;

    // Delta suppress — skip if nothing changed since last application
    if (f.movementFlags == mLastAppliedAnimFlags.movementFlags
        && f.actionFlags  == mLastAppliedAnimFlags.actionFlags
        && f.animFwd      == mLastAppliedAnimFlags.animFwd
        && f.animSide     == mLastAppliedAnimFlags.animSide)
        return;

    mLastAppliedAnimFlags = f;
    mState.animFlags      = f;

    if (mIsSpawned && !mNpcPtr.isEmpty())
        applyAnimationStateToActor();
}

// ---------------------------------------------------------------------------
void RemotePlayer::onAnimPlay(const BasePlayer& state)
{
    if (!mIsSpawned || mNpcPtr.isEmpty()) return;

    mState.animPlay = state.animPlay;
    const AnimPlay& ap = state.animPlay;

    if (ap.groupName.empty()) return;

    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return;

    MWRender::Animation* anim = world->getAnimation(mNpcPtr);
    if (!anim)
    {
        Log(Debug::Warning) << "[MP] RemotePlayer " << mName
                            << ": onAnimPlay — no Animation for NPC";
        return;
    }

    anim->play(ap.groupName,
               MWRender::AnimPriority(ap.priority),
               MWRender::BlendMask_All,
               /*autodisable=*/false,
               /*speedmult=*/1.f,
               ap.startKey,
               ap.stopKey,
               /*startpoint=*/0.f,
               /*loops=*/static_cast<uint32_t>(std::max(0, ap.loops)));

    Log(Debug::Verbose) << "[MP] RemotePlayer " << mName
                        << ": onAnimPlay group='" << ap.groupName
                        << "' priority=" << ap.priority
                        << " loops=" << ap.loops;
}

// ---------------------------------------------------------------------------
void RemotePlayer::onAttack(const BasePlayer& state)
{
    if (!mIsSpawned || mNpcPtr.isEmpty()) return;

    mState.attack = state.attack;
    const Attack& atk = state.attack;

    Log(Debug::Info) << "[MP] RemotePlayer " << mName
                     << ": onAttack hit=" << atk.hit
                     << " block=" << atk.block
                     << " miss=" << atk.miss
                     << " type=" << atk.type
                     << " target='" << atk.target << "'"
                     << " targetMpNum=" << atk.targetMpNum;

    // Tell the CharacterController the actor is attacking so it transitions
    // into the correct attack animation group on its own.  We set the flag
    // to match the incoming pressed state so the controller sees both the
    // press (true) and the release (false) when the two packets arrive.
    MWMechanics::CreatureStats& stats
        = mNpcPtr.getClass().getCreatureStats(mNpcPtr);
    stats.setAttackingOrSpell(atk.pressed);

    // Phase 7E: resolve target Ptr and apply damage via MWMechanics combat.
}

// ---------------------------------------------------------------------------
void RemotePlayer::onCast(const BasePlayer& state)
{
    if (!mIsSpawned || mNpcPtr.isEmpty()) return;

    mState.castSpell = state.castSpell;
    const CastSpell& cs = state.castSpell;

    Log(Debug::Info) << "[MP] RemotePlayer " << mName
                     << ": onCast spellId='" << cs.spellId << "'"
                     << " success=" << cs.success
                     << " targetGuid=" << cs.targetGuid;

    // Play the standard spell-cast animation on the caster's NPC.
    // "spellcast" is the vanilla Morrowind animation group name for this.
    BasePlayer animState;
    animState.animPlay.groupName = "spellcast";
    animState.animPlay.priority  = 5;
    animState.animPlay.loops     = 0;
    animState.animPlay.startKey  = "start";
    animState.animPlay.stopKey   = "stop";
    onAnimPlay(animState);

    // Phase 7E: resolve target and apply spell effects via MWMechanics.
}

// ---------------------------------------------------------------------------
void RemotePlayer::onInventoryUpdate(const BasePlayer& state)
{
    if (!mIsSpawned || mNpcPtr.isEmpty()) return;

    mState.inventoryChanges = state.inventoryChanges;
    const auto& changes = state.inventoryChanges;

    MWWorld::ContainerStore& store
        = mNpcPtr.getClass().getContainerStore(mNpcPtr);

    using Action = BasePlayer::InventoryChanges::Action;

    switch (changes.action)
    {
        case Action::Set:
            store.clear();
            for (const auto& item : changes.items)
                store.add(ESM::RefId::stringRefId(item.refId), item.count);
            Log(Debug::Verbose) << "[MP] RemotePlayer " << mName
                                << ": inventory Set ("
                                << changes.items.size() << " items)";
            break;

        case Action::Add:
            for (const auto& item : changes.items)
                store.add(ESM::RefId::stringRefId(item.refId), item.count);
            Log(Debug::Verbose) << "[MP] RemotePlayer " << mName
                                << ": inventory Add ("
                                << changes.items.size() << " items)";
            break;

        case Action::Remove:
            for (const auto& item : changes.items)
                store.remove(ESM::RefId::stringRefId(item.refId), item.count);
            Log(Debug::Verbose) << "[MP] RemotePlayer " << mName
                                << ": inventory Remove ("
                                << changes.items.size() << " items)";
            break;
    }
}

// ---------------------------------------------------------------------------
// Linear-interpolate current position/rotation toward target each frame.
void RemotePlayer::updateInterpolation(float dt)
{
    if (!mInterp.hasTarget) return;

    const float prevCx = mInterp.cx;
    const float prevCy = mInterp.cy;
    const float prevCz = mInterp.cz;

    // Position: smooth, slightly laggy — hides network jitter
    // When the authoritative state is idle, increase the catch-up speed so the
    // remote actor smoothly but rapidly closes the final gap instead of coasting.
    const bool isAirborne = (mState.animFlags.movementFlags & AnimFlags::MF_JUMP) != 0;
    // Use a larger epsilon (0.1) for idle detection to decisively filter wiggles
    const bool isIdleGrounded = (std::abs(mState.animFlags.animFwd) < 0.1f && std::abs(mState.animFlags.animSide) < 0.1f) && !isAirborne;

    // Idle catch-up: faster XY convergence when standing still, but NOT during
    // a jump — the arc is a fast parabola and we don't want to fight it with
    // an inflated speed multiplier that makes the Z curve feel sluggish.
    float interpSpeed = POS_INTERP_SPEED;
    if (isIdleGrounded)
    {
        interpSpeed *= 2.5f; // 15 * 2.5 = 37.5 u/s snap-to-stop on ground
        Log(Debug::Info) << "[MPDBG] " << mName << " IdleGrounded speed=" << interpSpeed;
    }

    const float posAlpha = std::min(1.f, dt * interpSpeed);
    mInterp.cx += (mInterp.tx - mInterp.cx) * posAlpha;
    mInterp.cy += (mInterp.ty - mInterp.cy) * posAlpha;

    // Z-axis: fast interpolation for the parabolic jump arc.
    const float zAlpha = std::min(1.f, dt * 60.f);
    mInterp.cz += (mInterp.tz - mInterp.cz) * zAlpha;

    Log(Debug::Info) << "[MPDBG] " << mName << " Alpha pos=" << posAlpha << " z=" << zAlpha;

    // Idle ground snap: quickly close the last few units after movement stops
    // so the actor doesn't visibly coast to a halt.  Deliberately excluded
    // when airborne — during a jump the Z delta oscillates through small values
    // at packet boundaries, and snapping would cause the erratic zStep jitter
    // seen in logs (e.g. +3.8, -11.6, +8.3 in consecutive frames).
    if (isIdleGrounded)
    {
        const float dx = mInterp.tx - mInterp.cx;
        const float dy = mInterp.ty - mInterp.cy;
        const float dz = mInterp.tz - mInterp.cz;
        if (dx * dx + dy * dy < 25.f) // 5-unit XY radius
        {
            Log(Debug::Info) << "[MPDBG] " << mName << " XY-Snap dist=" << std::sqrt(dx*dx + dy*dy);
            mInterp.cx = mInterp.tx;
            mInterp.cy = mInterp.ty;
        }
        if (std::abs(dz) < 3.f)
        {
            Log(Debug::Info) << "[MPDBG] " << mName << " Z-Snap dz=" << dz;
            mInterp.cz = mInterp.tz;
        }
    }

    // Rotation: snappier — turns should feel responsive, not lag behind.
    // lerpAngle handles the 0/2π wrap so we always take the short path.
    const float rotAlpha = std::min(1.f, dt * ROT_INTERP_SPEED);
    const float oldCrz = mInterp.crz;
    mInterp.crx = lerpAngle(mInterp.crx, mInterp.trx,  rotAlpha);
    mInterp.cry = lerpAngle(mInterp.cry, mInterp.try_,  rotAlpha);
    mInterp.crz = lerpAngle(oldCrz, mInterp.trz,  rotAlpha);

    // Save the wrapped delta for CharacterController's standing turn calculation
    mInterp.yawDelta = mInterp.crz - oldCrz;
    while (mInterp.yawDelta > osg::PIf)  mInterp.yawDelta -= osg::PIf * 2.f;
    while (mInterp.yawDelta < -osg::PIf) mInterp.yawDelta += osg::PIf * 2.f;

    mFootstepDebugTimer = std::max(0.f, mFootstepDebugTimer - dt);
    const float remDx = mInterp.tx - mInterp.cx;
    const float remDy = mInterp.ty - mInterp.cy;
    const float remainingPlanar = std::sqrt(remDx * remDx + remDy * remDy);
    const float interpStepX = mInterp.cx - prevCx;
    const float interpStepY = mInterp.cy - prevCy;
    const float interpStepZ = mInterp.cz - prevCz;
    const float interpPlanarSpeed = dt > 0.f
        ? std::sqrt(interpStepX * interpStepX + interpStepY * interpStepY) / dt
        : 0.f;
    const float netPlanarSpeedRaw = std::sqrt(
        mState.velocity.linear[0] * mState.velocity.linear[0]
        + mState.velocity.linear[1] * mState.velocity.linear[1]);
    // Safety cap (450 u/s) to prevent massive network velocity spikes from causing jitter
    const float netPlanarSpeed = std::min(netPlanarSpeedRaw, 450.f);

    // Override the animation cadence using `netPlanarSpeed` from the sender to
    // eliminate choppy footstep pacing. Only fall back to `interpPlanarSpeed`
    // when idle (snapping to stop). Cap to 200 so huge spawns don't trigger the 10x cap.
    if (std::abs(mState.animFlags.animFwd) >= 0.1f || std::abs(mState.animFlags.animSide) >= 0.1f)
    {
        mInterpPlanarSpeed = netPlanarSpeed;
        Log(Debug::Info) << "[MPDBG] " << mName << " Cadence net=" << netPlanarSpeed;
    }
    else
    {
        mInterpPlanarSpeed = std::min(interpPlanarSpeed, 200.f);
        Log(Debug::Info) << "[MPDBG] " << mName << " Cadence interp=" << mInterpPlanarSpeed;
    }

    const bool movingOrSettling = (mState.animFlags.animFwd != 0.f || mState.animFlags.animSide != 0.f)
        || remainingPlanar > 1.f
        || std::abs(interpStepZ) > 0.1f;
    if (movingOrSettling && mFootstepDebugTimer <= 0.f)
    {
        Log(Debug::Info) << "[MPDBG] Interp " << mName
                         << " fwd=" << mState.animFlags.animFwd
                         << " side=" << mState.animFlags.animSide
                         << " run=" << ((mState.animFlags.movementFlags & AnimFlags::MF_RUN) != 0)
                         << " sneak=" << ((mState.animFlags.movementFlags & AnimFlags::MF_SNEAK) != 0)
                         << " netPlanar=" << netPlanarSpeed
                         << " interpPlanar=" << interpPlanarSpeed
                         << " remainingPlanar=" << remainingPlanar
                         << " posAlpha=" << posAlpha
                         << " zStep=" << interpStepZ
                         << " jump=" << ((mState.animFlags.movementFlags & AnimFlags::MF_JUMP) != 0);
        mFootstepDebugTimer = 0.25f;
    }

    // Extrapolate Z target using last known vertical velocity
    if (isAirborne && mState.velocity.linear[2] != 0.f)
        mInterp.tz += mState.velocity.linear[2] * dt;

    // XY dead reckoning: advance the target forward using the sender's last-known
    // world-space velocity so the interpolator is always chasing a predicted future
    // position rather than a stale past one.  This removes the systematic lag visible
    // at 20 Hz (remoteplanar residuals of 5-9 units at packet arrival) and reduces
    // correction snaps to near-zero at 30 Hz.
    //
    // Guards:
    //   - Not idle (fwd/side != 0): no movement, no prediction needed.
    //   - Not airborne: jump arc is handled by the Z path above; XY during a jump
    //     is nearly flat and the position packet cadence is sufficient.
    //   - Drift cap: limit total XY divergence from the last received position to
    //     MAX_EXTRAP_DIST so a stale velocity (player stopped, packet delayed, or
    //     direction changed) cannot walk the ghost arbitrarily far away.  The next
    //     position packet resets lastRecvX/Y and corrects the target instantly.
    if (!isAirborne && !isIdleGrounded)
    {
        const float vx = mState.velocity.linear[0];
        const float vy = mState.velocity.linear[1];
        if (vx != 0.f || vy != 0.f)
        {
            // Advance target
            mInterp.tx += vx * dt;
            mInterp.ty += vy * dt;

            // Cap drift from last received packet position.
            // 2 × max Morrowind run speed (~280 u/s) × 1 packet at 30 Hz ≈ 19 units.
            // Use 24 units as a comfortable ceiling that handles a single dropped packet.
            constexpr float MAX_EXTRAP_DIST = 24.f;
            const float driftX = mInterp.tx - mInterp.lastRecvX;
            const float driftY = mInterp.ty - mInterp.lastRecvY;
            const float driftSq = driftX * driftX + driftY * driftY;
            if (driftSq > MAX_EXTRAP_DIST * MAX_EXTRAP_DIST)
            {
                const float scale = MAX_EXTRAP_DIST / std::sqrt(driftSq);
                mInterp.tx = mInterp.lastRecvX + driftX * scale;
                mInterp.ty = mInterp.lastRecvY + driftY * scale;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Returns true if the remote player's last-known cell matches the local
// player's current active cell.
bool RemotePlayer::isInSameCellAsLocalPlayer(bool quiet) const
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return false;

    MWWorld::Ptr localPlayer = world->getPlayerPtr();
    if (localPlayer.isEmpty() || !localPlayer.isInCell())
        return false;

    const MWWorld::CellStore* localCellStore = localPlayer.getCell();
    if (!localCellStore) return false;

    const MWWorld::Cell* localCell = localCellStore->getCell();
    if (!localCell) return false;

    const bool remoteExt = mState.cell.isExterior;
    const bool localExt  = localCell->isExterior();
    if (remoteExt != localExt)
    {
        if (!quiet)
            Log(Debug::Verbose) << "[MP] isInSameCell(" << mName << ")=false:"
                                << " exterior mismatch remote=" << remoteExt
                                << " local=" << localExt;
        return false;
    }

    if (remoteExt)
    {
        // Despawn only when the remote player's cell is no longer loaded by the engine.
        // This is the maximum safe visibility distance — rendering an NPC in an
        // unloaded cell would crash. Grid-offset checks (CellGridRadius) were too
        // conservative; isCellActive is the correct predicate.
        const bool match = world->isExteriorCellActive(mState.cell.gridX, mState.cell.gridY);
        if (!match && !quiet)
            Log(Debug::Verbose) << "[MP] isInSameCell(" << mName << ")=false:"
                                << " remote cell (" << mState.cell.gridX << ","
                                << mState.cell.gridY << ") is no longer active";
        return match;
    }

    // Both interior — compare name
    const std::string localName = std::string(localCell->getNameId());
    const bool match = (mState.cell.cellName == localName);
    if (!match && !quiet)
        Log(Debug::Verbose) << "[MP] isInSameCell(" << mName << ")=false:"
                            << " name mismatch remote='" << mState.cell.cellName
                            << "' local='" << localName << "'";
    return match;
}

// ===========================================================================
// PlayerList
// ===========================================================================

void PlayerList::addPlayer(uint32_t guid, const std::string& name)
{
    if (mPlayers.count(guid))
    {
        Log(Debug::Warning) << "[MP] PlayerList::addPlayer: guid "
                            << guid << " already exists";
        return;
    }
    mPlayers.emplace(guid, std::make_unique<RemotePlayer>(guid, name));
}

void PlayerList::removePlayer(uint32_t guid)
{
    auto it = mPlayers.find(guid);
    if (it == mPlayers.end())
    {
        Log(Debug::Warning) << "[MP] PlayerList::removePlayer: guid "
                            << guid << " not found";
        return;
    }
    mPlayers.erase(it);
}

RemotePlayer* PlayerList::getPlayer(uint32_t guid)
{
    auto it = mPlayers.find(guid);
    return (it != mPlayers.end()) ? it->second.get() : nullptr;
}

void PlayerList::updateAll(float dt)
{
    for (auto& [guid, rp] : mPlayers)
        rp->update(dt);
}

} // namespace mwmp
