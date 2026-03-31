#include "RemotePlayer.hpp"

#include <algorithm>
#include <cmath>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/misc/constants.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/mechanicsmanager.hpp"
#include "../../mwbase/rotationflags.hpp"
#include "../../mwbase/windowmanager.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwgui/mode.hpp"
#include "../../mwmechanics/aisetting.hpp"
#include "../../mwmechanics/character.hpp"
#include "../../mwmechanics/creaturestats.hpp"
#include "../../mwmechanics/movement.hpp"
#include "../../mwrender/animation.hpp"
#include "../../mwrender/animationpriority.hpp"
#include "../../mwrender/blendmask.hpp"
#include "../../mwworld/cell.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/class.hpp"
#include "../../mwworld/containerstore.hpp"
#include "../../mwworld/esmstore.hpp"
#include "../../mwworld/manualref.hpp"
#include "../../mwworld/ptr.hpp"
#include <components/esm3/loadnpc.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>

namespace mwmp
{

    namespace
    {
        // Shortest-path lerp between two angles (radians).
        // Guarantees interpolation never takes the long way around 0/2π.
        inline float lerpAngle(float current, float target, float alpha)
        {
            constexpr float TWO_PI = 6.28318530718f;
            constexpr float PI = 3.14159265359f;
            float diff = target - current;
            while (diff > PI)
                diff -= TWO_PI;
            while (diff < -PI)
                diff += TWO_PI;
            return current + diff * alpha;
        }

    }

    // ---------------------------------------------------------------------------
    RemotePlayer::RemotePlayer(uint32_t guid, const std::string& name)
        : mGuid(guid)
        , mName(name)
    {
        mState.guid = guid;
        mState.name = name;
        mState.animFlags.animFwd = 0.f;
        mState.animFlags.animSide = 0.f;
        mLastAppliedAnimFlags.animFwd = 0.f;
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
        mJumpLandingTimer = std::max(0.f, mJumpLandingTimer - dt);

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
                cs.setAiSetting(MWMechanics::AiSetting::Flee, 0);
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
                    Log(Debug::Info) << "[MP] RemotePlayer " << mName << ": nameplate attached (deferred)";
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

        Log(Debug::Info) << "[MP] RemotePlayer " << mName << ": registered with MechanicsManager (deferred)";
    }

    // ---------------------------------------------------------------------------
    void RemotePlayer::applyAnimationStateToActor()
    {
        if (!mIsSpawned || mNpcPtr.isEmpty())
            return;

        const AnimFlags& f = mState.animFlags;

        MWMechanics::Movement& mov = mNpcPtr.getClass().getMovementSettings(mNpcPtr);

        // Pass anim axes directly — no dead-band clamping needed.
        // The sender's velocity gate (5/15 u/s) and WASD gate already ensure that
        // sub-threshold idle sway never produces a non-zero fwd/side value, so
        // a receiver-side clamp is redundant and was always a no-op at 0.00.
        const float effFwd = f.animFwd;
        const float effSide = f.animSide;

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
        const float absFwd = std::abs(effFwd);
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
            if (absSide > absFwd * 2.0f || (absFwd == 0.f && absSide > 0.f))
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
        bool isJumping = (f.movementFlags & AnimFlags::MF_JUMP) != 0;
        const bool isFlying = (f.movementFlags & AnimFlags::MF_FLY) != 0;

        // Force a grounded state for a brief window after a jump ends to ensure the
        // CharacterController exits the JumpState_InAir loop during rapid jump spam (bunny hopping).
        // Since network packets can batch the falling edge (jump=0) and the immediate
        // rising edge (jump=1) of a rapid re-jump into the same frame, the CC would normally
        // never see the onGround=true frame and get permanently stuck in the fall animation.
        // We also set user-value "mp_force_grounded" to bypass traceDown air gap checks so the
        // engine evaluates the landing transition even if the visual model is still high up.
        auto* baseNode = mNpcPtr.getRefData().getBaseNode();
        if (mJumpLandingTimer > 0.f)
        {
            isJumping = false;
            if (baseNode)
                baseNode->setUserValue("mp_force_grounded", true);
        }
        else if (baseNode)
        {
            baseNode->setUserValue("mp_force_grounded", false);
        }

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

        // Levitation — fix the remote NPC showing the fall animation while levitating.
        //
        // world->isFlying() is what character.cpp queries each frame to decide whether
        // to skip the jump/fall code path and play the walk animation instead.  It
        // checks for the Levitate magic effect magnitude > 0 on the actor's stats.
        // Remote NPCs have no active spells, so isFlying() always returns false for
        // them — the CC sees (!onGround && !flying) and locks into JumpState_InAir /
        // fall animation for the entire levitation.
        //
        // Fix: write an "mp_fly" user-value on the NPC's base node every frame.
        // character.cpp reads this flag right after world->isFlying() and OR's it in,
        // making the CC treat this NPC as flying regardless of its magic effect list.
        //
        // WHY NOT magic-effect injection:
        //   Actors::update() calls adjustMagicEffects() (via updateActor()) in its
        //   first per-actor loop, which rebuilds the effect magnitudes from active
        //   spells and wipes anything we injected — all before the second loop where
        //   ctrl.update() / world->isFlying() actually runs.  Edge-triggered injection
        //   fires once but is guaranteed dead by the time the CC checks.  Continuous
        //   injection every frame still loses the race because RemotePlayer::update()
        //   runs before Actors::update(), so the wipe always happens last.
        //   The base-node flag survives the full frame because OSG user-values are
        //   never touched by the mechanics pipeline.
        if (auto* bn = mNpcPtr.getRefData().getBaseNode())
            bn->setUserValue("mp_fly", isFlying);

        // Keep the actor physically off the ground while levitating or jumping so the
        // physics engine doesn't snap them down and trigger the "stuck" knockout path.
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

        // Flag_ForceJump / Flag_ForceMoveJump already set above (before setOnGround).
        // Re-applying here is harmless but kept for clarity as the canonical stats block.
        MWMechanics::CreatureStats& stats = mNpcPtr.getClass().getCreatureStats(mNpcPtr);
        stats.setMovementFlag(MWMechanics::CreatureStats::Flag_ForceJump, isJumping);
        stats.setMovementFlag(MWMechanics::CreatureStats::Flag_ForceMoveJump, isJumping);
        stats.setMovementFlag(MWMechanics::CreatureStats::Flag_Run, (f.movementFlags & AnimFlags::MF_RUN) != 0);
        stats.setMovementFlag(MWMechanics::CreatureStats::Flag_Sneak, (f.movementFlags & AnimFlags::MF_SNEAK) != 0);
        stats.setAttackingOrSpell((f.actionFlags & AnimFlags::AF_ATTACKING) != 0 || mState.attack.pressed);

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
        const bool isKnockedOut = (f.movementFlags & AnimFlags::MF_KNOCKED_OUT) != 0;
        const bool isKnockedDown = (f.movementFlags & AnimFlags::MF_KNOCKED_DOWN) != 0;

        if (isKnockedOut || isKnockedDown)
        {
            // Zero movement so the CC's locomotion branch doesn't fight the hit anim
            mov.mPosition[0] = 0.f;
            mov.mPosition[1] = 0.f;
            mov.mIsStrafing = false;

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

        if (mState.position.pos[0] == 0.f && mState.position.pos[1] == 0.f && mState.position.pos[2] == 0.f)
        {
            Log(Debug::Verbose) << "[MP] trySpawn(" << mName << "): position is zero, waiting";
            return;
        }

        if (!isInSameCellAsLocalPlayer(/*quiet=*/true))
        {
            Log(Debug::Verbose) << "[MP] trySpawn(" << mName << "): not in same cell (remote='" << mState.cell.cellName
                                << "' ext=" << mState.cell.isExterior << ")";
            return;
        }

        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (!world)
        {
            Log(Debug::Warning) << "[MP] trySpawn(" << mName << "): world null";
            return;
        }

        MWWorld::Ptr localPlayer = world->getPlayerPtr();
        if (localPlayer.isEmpty())
        {
            Log(Debug::Warning) << "[MP] trySpawn(" << mName << "): local player empty";
            return;
        }
        if (!localPlayer.isInCell())
        {
            Log(Debug::Warning) << "[MP] trySpawn(" << mName << "): local player not in cell";
            return;
        }

        MWWorld::CellStore* cell = localPlayer.getCell();
        if (!cell)
        {
            Log(Debug::Warning) << "[MP] trySpawn(" << mName << "): local CellStore null";
            return;
        }

        Log(Debug::Info) << "[MP] trySpawn(" << mName << "): attempting spawn at (" << mState.position.pos[0] << ", "
                         << mState.position.pos[1] << ", " << mState.position.pos[2] << ") in '" << mState.cell.cellName
                         << "'";

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
            const ESM::NPC* playerTemplate = world->getStore().get<ESM::NPC>().find(ESM::RefId::stringRefId("player"));
            ESM::NPC npcRecord = *playerTemplate;

            // Assign unique ID and override appearance from received base info
            npcRecord.mId = ESM::RefId::stringRefId(npcRecordId);
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
                npcRecord.mFlags |= static_cast<unsigned char>(ESM::NPC::Female);

            // Insert (or update if already exists from a previous spawn) the record
            world->getStore().overrideRecord(npcRecord);

            Log(Debug::Info) << "[MP] RemotePlayer " << mName << ": NPC record '" << npcRecordId << "'"
                             << " race='" << mState.race << "'"
                             << " head='" << mState.headMesh << "'"
                             << " hair='" << mState.hairMesh << "'"
                             << " male=" << mState.isMale;

            MWWorld::ManualRef ref(world->getStore(), ESM::RefId::stringRefId(npcRecordId), 1);

            mNpcPtr = world->placeObject(ref.getPtr(), cell, pos);

            if (mNpcPtr.isEmpty())
            {
                Log(Debug::Warning) << "[MP] RemotePlayer " << mName << ": placeObject returned empty Ptr";
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

            // Disable physics collision for remote NPCs. Their position is fully
            // authority-controlled by the interpolator (world->moveObject every frame),
            // so geometry collision is counterproductive: the CC's locomotion (fwd=1.0
            // → ~2 u/frame) would push the NPC into walls, physics springs it back, and
            // the interpolator snaps it forward again — a visible oscillation at any wall
            // the sender is standing against. With collision off the CC can push through
            // geometry harmlessly and the interpolator is the sole position authority.
            // Floors are safe: the interpolator always positions the NPC at the correct Z.
            // Tradeoff: local player can walk through remote players (acceptable for now).
            // world->setActorCollisionMode(mNpcPtr, false, false);

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

            Log(Debug::Info) << "[MP] RemotePlayer " << mName << ": spawned in world at (" << pos.pos[0] << ", "
                             << pos.pos[1] << ", " << pos.pos[2] << ")";
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[MP] RemotePlayer " << mName << ": spawn failed: " << e.what();
        }
    }

    // ---------------------------------------------------------------------------
    void RemotePlayer::despawnFromWorld()
    {
        if (!mIsSpawned)
            return;

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
                Log(Debug::Warning) << "[MP] RemotePlayer " << mName << ": despawn error: " << e.what();
            }
        }

        mNpcPtr = MWWorld::Ptr();
        mIsSpawned = false;
        mMechanicsRegistered = false;
        mWasJumping = false; // reset so re-spawn doesn't skip the first jump edge
        mSpawnRetryTimer = SPAWN_RETRY_RATE; // attempt immediately on next update
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
        if (!world)
            return;

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
            world->rotateObject(mNpcPtr, osg::Vec3f(mInterp.crx, mInterp.cry, mInterp.crz), MWBase::RotationFlag_none);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[MP] RemotePlayer " << mName << ": applyInterpolation error: " << e.what();
            despawnFromWorld();
        }
    }

    // ---------------------------------------------------------------------------
    void RemotePlayer::onBaseInfoUpdate(const BasePlayer& state)
    {
        // Track whether any appearance field changed (requires re-spawn to rebuild NPC record)
        const bool appearanceChanged = (mState.race != state.race) || (mState.headMesh != state.headMesh)
            || (mState.hairMesh != state.hairMesh) || (mState.isMale != state.isMale);

        const bool nameChanged = (mState.name != state.name);

        if (!nameChanged && !appearanceChanged)
            return;

        // Always copy all base-info fields
        mState.name = state.name;
        mState.race = state.race;
        mState.headMesh = state.headMesh;
        mState.hairMesh = state.hairMesh;
        mState.isMale = state.isMale;
        mState.scale = state.scale;
        mName = state.name;

        Log(Debug::Info) << "[MP] RemotePlayer " << mName << ": base info updated";

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
        Log(Debug::Verbose) << "[MP] onPositionUpdate(" << mName << " guid=" << mGuid << "): pos=("
                            << state.position.pos[0] << "," << state.position.pos[1] << "," << state.position.pos[2]
                            << ")"
                            << " knownCell='" << mState.cell.cellName << "'"
                            << " ext=" << mState.cell.isExterior << " grid=(" << mState.cell.gridX << ","
                            << mState.cell.gridY << ")";
        // intentionally NOT touching mState.cell
        mState.position = state.position;
        mState.velocity = state.velocity;

        // --- Explicit Teleport Snap ---
        // If the sender explicitly flagged this movement as a teleport (coc, scripts, doors),
        // we hard-snap all interpolation state immediately. There is no need for 
        // distance heuristics or smoothing in this path.
        if (state.position.isTeleporting)
        {
            mInterp.cx = state.position.pos[0];
            mInterp.cy = state.position.pos[1];
            mInterp.cz = state.position.pos[2];
            mInterp.crx = state.position.rot[0];
            mInterp.cry = state.position.rot[1];
            mInterp.crz = state.position.rot[2];
            mInterp.tx = mInterp.cx;
            mInterp.ty = mInterp.cy;
            mInterp.tz = mInterp.cz;
            mInterp.trx = mInterp.crx;
            mInterp.try_ = mInterp.cry;
            mInterp.trz = mInterp.crz;
            mInterp.lastRecvX = mInterp.cx;
            mInterp.lastRecvY = mInterp.cy;
            mInterp.lastRecvZ = mInterp.cz;
            mInterp.hasTarget = true;
            mInterp.hasSnapped = true;
            Log(Debug::Info) << "[MP] RemotePlayer " << mName << " teleported: hard-snapping.";
            return;
        }

        // Set new interpolation target; record raw received XY for drift cap.
        // When the remote player is idle (both anim axes zero), suppress XY target
        // updates: the sender's move360.lua yawChange causes tiny body rotations at
        // standstill that induce micro-jitter in the sent position, which would make
        // the NPC slide back and forth each packet.  Z and rotation are always kept
        // so gravity/falling and standing-turn animations work correctly.
        const bool isIdleAnim = (mState.animFlags.animFwd == 0.f) && (mState.animFlags.animSide == 0.f);
        const bool isAirborneState
            = ((mState.animFlags.movementFlags & AnimFlags::MF_JUMP) != 0) || std::abs(state.velocity.linear[2]) > 0.1f;
        if (!isIdleAnim || isAirborneState)
        {
            mInterp.lastRecvX = state.position.pos[0];
            mInterp.lastRecvY = state.position.pos[1];
            mInterp.tx = state.position.pos[0];
            mInterp.ty = state.position.pos[1];
        }
        mInterp.lastRecvZ = state.position.pos[2];
        mInterp.tz = state.position.pos[2];
        mInterp.trx = state.position.rot[0];
        mInterp.try_ = state.position.rot[1];
        mInterp.trz = state.position.rot[2];
        mInterp.targetVz = state.velocity.linear[2];
        mInterp.hasTarget = true;

        // Prime the jump arc dead-reckoning once we see a real airborne vz.
        // The AnimFlags jumpVz field is always ≈ 0 at the rising edge (EMA lag),
        // so gravity is suspended (mJumpArcPrimed=false) until this path fires.
        // The threshold of 30 u/s filters out stair/slope noise while catching
        // any genuine jump trajectory (normal jumps start at ~100–200 u/s).
        const bool currentlyJumping = (mState.animFlags.movementFlags & AnimFlags::MF_JUMP) != 0;
        if (currentlyJumping && !mJumpArcPrimed && std::abs(state.velocity.linear[2]) > 30.f)
        {
            mJumpArcPrimed = true;
            mInterp.targetVz = state.velocity.linear[2];
            // Snap cz directly to the sender's current Z so the NPC appears at the correct
            // airborne height immediately instead of slowly lerping up from the ground.
            // The un-primed window held tz at ground level (currentVz=0), so cz is still
            // near ground while tz is now at the sender's real airborne position.
            // Without this snap, we would produce visible upward acceleration through geometry.
            mInterp.cz = mInterp.tz;
            Log(Debug::Verbose) << "[MP] RemotePlayer " << mName << ": jump arc primed — cz snapped to tz=" << mInterp.tz
                                << " vz=" << state.velocity.linear[2];
        }

        // Snap current pos to target on first update (avoid lerping from origin)
        if (!mInterp.hasSnapped)
        {
            mInterp.cx = mInterp.tx;
            mInterp.cy = mInterp.ty;
            mInterp.cz = mInterp.tz;
            mInterp.crx = mInterp.trx;
            mInterp.cry = mInterp.try_;
            mInterp.crz = mInterp.trz;
            mInterp.lastRecvX = mInterp.tx;
            mInterp.lastRecvY = mInterp.ty;
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
        const bool gridChanged = (mState.cell.gridX != state.cell.gridX || mState.cell.gridY != state.cell.gridY);

        const bool actuallyChanged = cellNameChanged || exteriorChanged || gridChanged;

        const std::string oldCell = mState.cell.cellName;
        mState.cell = state.cell;
        mState.position = state.position;

        // Snap interpolation to new position so trySpawn has valid coordinates
        mInterp.cx = state.position.pos[0];
        mInterp.cy = state.position.pos[1];
        mInterp.cz = state.position.pos[2];
        mInterp.crz = state.position.rot[2];
        mInterp.tx = mInterp.cx;
        mInterp.ty = mInterp.cy;
        mInterp.tz = mInterp.cz;
        mInterp.trz = mInterp.crz;
        mInterp.lastRecvX = mInterp.cx;
        mInterp.lastRecvY = mInterp.cy;
        mInterp.lastRecvZ = mInterp.cz;
        mInterp.hasTarget = true;
        mInterp.hasSnapped = true;

        if (actuallyChanged)
        {
            Log(Debug::Info) << "[MP] RemotePlayer " << mName << " cell: " << oldCell << " -> " << state.cell.cellName;

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

        const bool wasJumping = (mLastAppliedAnimFlags.movementFlags & AnimFlags::MF_JUMP) != 0;
        const bool isJumping = (f.movementFlags & AnimFlags::MF_JUMP) != 0;

        // Rising edge: suspend pseudo-gravity until a position packet with real vz arrives.
        //
        // WHY NOT use f.jumpVz here:
        //   jumpVz is captured from mSmoothedVz at the moment the CC jump visual fires.
        //   That moment is 1-2 frames BEFORE the physics impulse propagates to a position
        //   delta, so the EMA hasn't seen the jump yet — jumpVz is always ≈ 0 for
        //   straight-up jumps where the player was standing still beforehand.
        //   Setting targetVz = 0 and then running pseudo-gravity makes the NPC fall
        //   immediately while the sender is actually going UP.
        //
        // SOLUTION: set mJumpArcPrimed = false on the rising edge. updateInterpolation
        //   skips pseudo-gravity while unprimed and just holds tz at the last received Z.
        //   onPositionUpdate primes the arc (mJumpArcPrimed = true) the moment the first
        //   position packet arrives with |vz| > 30 u/s — that packet has the real launch
        //   velocity and sets mInterp.targetVz before gravity runs.
        if (!wasJumping && isJumping)
        {
            mJumpArcPrimed = false;
            mInterp.targetVz = 0.f; // will be overwritten by first real position packet
            Log(Debug::Info) << "[MP] RemotePlayer " << mName
                             << ": jump rising edge — arc suspended, awaiting position vz";
        }

        if (wasJumping && !isJumping)
        {
            mJumpArcPrimed = false; // reset for next jump

            // Jump ended, snap Z to target to prevent feet clipping into the floor
            mInterp.cz = mInterp.tz;
            // Force the CC to see at least a brief grounded state to break fall-loop
            mJumpLandingTimer = 0.05f;
        }

        // Delta suppress — skip if nothing changed since last application
        if (f.movementFlags == mLastAppliedAnimFlags.movementFlags && f.actionFlags == mLastAppliedAnimFlags.actionFlags
            && f.animFwd == mLastAppliedAnimFlags.animFwd && f.animSide == mLastAppliedAnimFlags.animSide
            && f.blockedMoveSpeed == mLastAppliedAnimFlags.blockedMoveSpeed)
            return;

        mLastAppliedAnimFlags = f;
        mState.animFlags = f;

        if (mIsSpawned && !mNpcPtr.isEmpty())
            applyAnimationStateToActor();
    }

    // ---------------------------------------------------------------------------
    void RemotePlayer::onAnimPlay(const BasePlayer& state)
    {
        if (!mIsSpawned || mNpcPtr.isEmpty())
            return;

        mState.animPlay = state.animPlay;
        const AnimPlay& ap = state.animPlay;

        if (ap.groupName.empty())
            return;

        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (!world)
            return;

        MWRender::Animation* anim = world->getAnimation(mNpcPtr);
        if (!anim)
        {
            Log(Debug::Warning) << "[MP] RemotePlayer " << mName << ": onAnimPlay — no Animation for NPC";
            return;
        }

        anim->play(ap.groupName, MWRender::AnimPriority(ap.priority), MWRender::BlendMask_All,
            /*autodisable=*/false,
            /*speedmult=*/1.f, ap.startKey, ap.stopKey,
            /*startpoint=*/0.f,
            /*loops=*/static_cast<uint32_t>(std::max(0, ap.loops)));

        Log(Debug::Verbose) << "[MP] RemotePlayer " << mName << ": onAnimPlay group='" << ap.groupName
                            << "' priority=" << ap.priority << " loops=" << ap.loops;
    }

    // ---------------------------------------------------------------------------
    void RemotePlayer::onAttack(const BasePlayer& state)
    {
        if (!mIsSpawned || mNpcPtr.isEmpty())
            return;

        mState.attack = state.attack;
        const Attack& atk = state.attack;

        Log(Debug::Info) << "[MP] RemotePlayer " << mName << ": onAttack hit=" << atk.hit << " block=" << atk.block
                         << " miss=" << atk.miss << " type=" << atk.type << " target='" << atk.target << "'"
                         << " targetMpNum=" << atk.targetMpNum;

        // Tell the CharacterController the actor is attacking so it transitions
        // into the correct attack animation group on its own.  We set the flag
        // to match the incoming pressed state so the controller sees both the
        // press (true) and the release (false) when the two packets arrive.
        MWMechanics::CreatureStats& stats = mNpcPtr.getClass().getCreatureStats(mNpcPtr);
        stats.setAttackingOrSpell(atk.pressed);

        // Phase 7E: resolve target Ptr and apply damage via MWMechanics combat.
    }

    // ---------------------------------------------------------------------------
    void RemotePlayer::onCast(const BasePlayer& state)
    {
        if (!mIsSpawned || mNpcPtr.isEmpty())
            return;

        mState.castSpell = state.castSpell;
        const CastSpell& cs = state.castSpell;

        Log(Debug::Info) << "[MP] RemotePlayer " << mName << ": onCast spellId='" << cs.spellId << "'"
                         << " success=" << cs.success << " targetGuid=" << cs.targetGuid;

        // Play the standard spell-cast animation on the caster's NPC.
        // "spellcast" is the vanilla Morrowind animation group name for this.
        BasePlayer animState;
        animState.animPlay.groupName = "spellcast";
        animState.animPlay.priority = 5;
        animState.animPlay.loops = 0;
        animState.animPlay.startKey = "start";
        animState.animPlay.stopKey = "stop";
        onAnimPlay(animState);

        // Phase 7E: resolve target and apply spell effects via MWMechanics.
    }

    // ---------------------------------------------------------------------------
    void RemotePlayer::onInventoryUpdate(const BasePlayer& state)
    {
        if (!mIsSpawned || mNpcPtr.isEmpty())
            return;

        mState.inventoryChanges = state.inventoryChanges;
        const auto& changes = state.inventoryChanges;

        MWWorld::ContainerStore& store = mNpcPtr.getClass().getContainerStore(mNpcPtr);

        using Action = BasePlayer::InventoryChanges::Action;

        switch (changes.action)
        {
            case Action::Set:
                store.clear();
                for (const auto& item : changes.items)
                    store.add(ESM::RefId::stringRefId(item.refId), item.count);
                Log(Debug::Verbose) << "[MP] RemotePlayer " << mName << ": inventory Set (" << changes.items.size()
                                    << " items)";
                break;

            case Action::Add:
                for (const auto& item : changes.items)
                    store.add(ESM::RefId::stringRefId(item.refId), item.count);
                Log(Debug::Verbose) << "[MP] RemotePlayer " << mName << ": inventory Add (" << changes.items.size()
                                    << " items)";
                break;

            case Action::Remove:
                for (const auto& item : changes.items)
                    store.remove(ESM::RefId::stringRefId(item.refId), item.count);
                Log(Debug::Verbose) << "[MP] RemotePlayer " << mName << ": inventory Remove (" << changes.items.size()
                                    << " items)";
                break;
        }
    }

    // ---------------------------------------------------------------------------
    // Linear-interpolate current position/rotation toward target each frame.
    void RemotePlayer::updateInterpolation(float dt)
    {
        if (!mInterp.hasTarget)
            return;

        const float prevCx = mInterp.cx;
        const float prevCy = mInterp.cy;
        const float prevCz = mInterp.cz;

        // Position: smooth, slightly laggy — hides network jitter.
        // XY already has dead reckoning below, but Z used to rely almost entirely on
        // raw packet targets except during jumps. That made hills, stairs, levitation,
        // and even jump arcs look noticeably laggier than flat ground movement.
        const bool isJumping = (mState.animFlags.movementFlags & AnimFlags::MF_JUMP) != 0;
        const bool isFlying = (mState.animFlags.movementFlags & AnimFlags::MF_FLY) != 0;
        const bool isAirborne = isJumping || isFlying;
        const float netVerticalSpeed = mState.velocity.linear[2];
        const bool hasVerticalMotion = std::abs(netVerticalSpeed) > 0.1f || isAirborne;
        // Use a larger epsilon (0.1) for idle detection to decisively filter wiggles.
        // Any real vertical motion must break the idle-grounded path or levitation and
        // stair movement get misclassified as "standing still".
        const bool isIdleGrounded
            = (std::abs(mState.animFlags.animFwd) < 0.1f && std::abs(mState.animFlags.animSide) < 0.1f) && !isAirborne;

        // Z dead-reckoning: advance the Z target using the sender's smoothed
        // vertical velocity, exactly as we do for XY.  This covers slopes, stairs,
        // jumps, and levitation — all cases where the NPC has continuous vertical
        // motion between position packets.
        //
        // Gate: only predict when the NPC is not idle-grounded.  The old
        // `hasVerticalMotion` gate (|vz| > 0.1) was too narrow — on gentle slopes
        // and stair treads the per-frame Z velocity oscillates near zero, stalling
        // prediction while XY kept advancing smoothly.  Using the same idle check
        // as XY ensures consistent treatment.
        //
        // Drift cap: 16 units grounded (was 8), 24 airborne (was 18).  The old 8
        // only covered ~80 ms at 100 u/s Z velocity — any additional inter-packet
        // time saw Z stuck at the cap.  16 matches XY's proportional headroom.
        if (!isIdleGrounded && hasVerticalMotion)
        {
            float currentVz = netVerticalSpeed;
            if (isAirborne && !isFlying)
            {
                // TES3MP lesson: pseudo-gravity dead-reckoning is unreliable because
                // the EMA significantly underestimates superjump launch velocity
                // (alpha=0.25 → only 25% of real vz captured on first frame).
                // Seeding with the wrong initial vz produces an arc that peaks too early
                // and then fights position packet corrections, causing floor/ceiling clips.
                //
                // Instead, track position packets directly (no Z prediction during jumps).
                // The lerp + gap-snap below produce smooth, clip-free arcs that match the
                // TES3MP proven approach. Pseudo-gravity is preserved for ground movement
                // (slopes, stairs) where the EMA is accurate.
                //
                // Slide the anchor each frame so the drift cap follows received Z rather
                // than clamping against stale ground level across the whole flight.
                currentVz = 0.f;
                mInterp.lastRecvZ = mInterp.tz;
            }

            mInterp.tz += currentVz * dt;

            // Scale drift cap dynamically based on velocity instead of a fixed 24u limit
            // so extreme speeds (e.g. jump spells) don't stall extrapolation.
            const float maxZExtrap = isAirborne ? std::max(80.f, std::abs(currentVz) * 0.1f) : 8.f;

            mInterp.tz = std::clamp(mInterp.tz, mInterp.lastRecvZ - maxZExtrap, mInterp.lastRecvZ + maxZExtrap);
        }

        // Idle catch-up: faster XY convergence when standing still, but NOT during
        // a jump — the arc is a fast parabola and we don't want to fight it with
        // an inflated speed multiplier that makes the Z curve feel sluggish.
        float interpSpeed = POS_INTERP_SPEED;
        if (isIdleGrounded)
        {
            interpSpeed *= 2.5f; // 15 * 2.5 = 37.5 u/s snap-to-stop on ground
            Log(Debug::Verbose) << "[MP] " << mName << " IdleGrounded speed=" << interpSpeed;
        }

        const float posAlpha = 1.0f - std::exp(-dt * interpSpeed);
        mInterp.cx += (mInterp.tx - mInterp.cx) * posAlpha;
        mInterp.cy += (mInterp.ty - mInterp.cy) * posAlpha;

        // Z-axis lerp: we rely on the explicit mState.position.isTeleporting flag 
        // to handle hard-snaps during coc/teleport. For regular movement (including
        // 1000+ Acrobatics jumps), we allow the exponential decay to stay smooth.
        // A very large fallback snap (2000u) is kept only for extreme desync safety.
        {
            const float zGap = std::abs(mInterp.tz - mInterp.cz);
            if (zGap > 2000.f)
            {
                mInterp.cz = mInterp.tz;
                Log(Debug::Info) << "[MP] " << mName << " Safety Hard-Snap gap=" << zGap;
            }
            else
            {
                // Reduced multipliers (was 3.0 : 1.5) to fix 60 FPS lurch-and-pause stutter.
                // At 60 FPS (dt=0.0166), an airborne multiplier of 1.5 yields an
                // alpha of ~0.37 (37% distance closed per frame). This perfectly
                // emulates the smooth pacing we observed at 120 FPS previously.
                const float zMultiplier = isAirborne ? 1.5f : 1.0f;
                const float zAlpha = 1.0f - std::exp(-dt * interpSpeed * zMultiplier);
                mInterp.cz += (mInterp.tz - mInterp.cz) * zAlpha;
            }
        }

        Log(Debug::Verbose) << "[MP] " << mName << " Alpha pos=" << posAlpha
                         << " zGap=" << std::abs(mInterp.tz - mInterp.cz);

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
            if (dx * dx + dy * dy < 4.f) // 2-unit XY radius
            {
                Log(Debug::Verbose) << "[MP] " << mName << " XY-Snap dist=" << std::sqrt(dx * dx + dy * dy);
                mInterp.cx = mInterp.tx;
                mInterp.cy = mInterp.ty;
            }
            if (std::abs(dz) < 3.f)
            {
                Log(Debug::Verbose) << "[MP] " << mName << " Z-Snap dz=" << dz;
                mInterp.cz = mInterp.tz;
            }
        }

        // Rotation: snappier — turns should feel responsive, not lag behind.
        // lerpAngle handles the 0/2π wrap so we always take the short path.
        const float rotAlpha = 1.0f - std::exp(-dt * ROT_INTERP_SPEED);
        const float oldCrz = mInterp.crz;
        mInterp.crx = lerpAngle(mInterp.crx, mInterp.trx, rotAlpha);
        mInterp.cry = lerpAngle(mInterp.cry, mInterp.try_, rotAlpha);
        mInterp.crz = lerpAngle(oldCrz, mInterp.trz, rotAlpha);

        // Save the wrapped delta for CharacterController's standing turn calculation
        mInterp.yawDelta = mInterp.crz - oldCrz;
        while (mInterp.yawDelta > osg::PIf)
            mInterp.yawDelta -= osg::PIf * 2.f;
        while (mInterp.yawDelta < -osg::PIf)
            mInterp.yawDelta += osg::PIf * 2.f;

        mFootstepDebugTimer = std::max(0.f, mFootstepDebugTimer - dt);
        const float remDx = mInterp.tx - mInterp.cx;
        const float remDy = mInterp.ty - mInterp.cy;
        const float remainingPlanar = std::sqrt(remDx * remDx + remDy * remDy);
        const float interpStepX = mInterp.cx - prevCx;
        const float interpStepY = mInterp.cy - prevCy;
        const float interpStepZ = mInterp.cz - prevCz;
        const float interpPlanarSpeed
            = dt > 0.f ? std::sqrt(interpStepX * interpStepX + interpStepY * interpStepY) / dt : 0.f;
        const float netPlanarSpeedRaw = std::sqrt(mState.velocity.linear[0] * mState.velocity.linear[0]
            + mState.velocity.linear[1] * mState.velocity.linear[1]);
        // Safety cap (800 u/s instead of 450) to prevent massive network velocity spikes
        // from causing jitter, while accommodating high Athletics/Acrobatics sprint speeds
        const float netPlanarSpeed = std::min(netPlanarSpeedRaw, 800.f);

        // Animation cadence — set mInterpPlanarSpeed for CharacterController.
        //
        // Normal movement uses the sender's measured physics speed so footstep
        // pacing matches the visual distance covered, including slow sneak-walk.
        // Confirmed wall-block packets are the only exception: there the sender is
        // holding movement keys but physics speed collapses toward zero, so using
        // netPlanarSpeed would play the walk cycle in extreme slow motion.
        {
            const float absFwdAnim = std::abs(mState.animFlags.animFwd);
            const float absSideAnim = std::abs(mState.animFlags.animSide);
            const bool isMovingAnim = (absFwdAnim >= 0.1f || absSideAnim >= 0.1f);
            const bool isWallBlocked = (mState.animFlags.movementFlags & AnimFlags::MF_WALL_BLOCKED) != 0;

            if (isMovingAnim)
            {
                if (isWallBlocked)
                {
                    // Confirmed wall contact: sender intent is "keep moving", but
                    // its physics speed is artificially tiny. Use the sender's
                    // actual expected pace for this stance so blocked sneak/walk/run
                    // keeps the same cadence as the local actor instead of falling
                    // back to a generic hardcoded speed.
                    const float blockedSpeed = mState.animFlags.blockedMoveSpeed;
                    mInterpPlanarSpeed = (blockedSpeed > 0.f) ? std::min(std::max(blockedSpeed, netPlanarSpeed), 800.f)
                                                              : netPlanarSpeed;
                    Log(Debug::Info) << "[MPDBG] " << mName << " Cadence blocked=" << mInterpPlanarSpeed;
                }
                else
                {
                    // To prevent "ice skating" when the interpolator is catching up over
                    // a large distance (e.g. at start of sprint), the cadence must track the
                    // actual visual speed of the actor (interpPlanarSpeed) if it exceeds the
                    // static network velocity, but should not drop below the network velocity
                    // to maintain fluid footsteps.
                    mInterpPlanarSpeed = std::min(std::max(netPlanarSpeed, interpPlanarSpeed), 800.f);
                    Log(Debug::Verbose) << "[MP] " << mName << " Cadence blend=" << mInterpPlanarSpeed;
                }
            }
            else
            {
                // Even if the interpolator is still settling the remaining error, an idle
                // grounded actor should not play movement animations. Zeroing the speed
                // immediately kills the CharacterController's footstep cadence.
                mInterpPlanarSpeed = isIdleGrounded ? 0.f : std::min(interpPlanarSpeed, 200.f);
                Log(Debug::Verbose) << "[MP] " << mName << " Cadence interp=" << mInterpPlanarSpeed;
            }
        }

        const bool movingOrSettling = (mState.animFlags.animFwd != 0.f || mState.animFlags.animSide != 0.f)
            || remainingPlanar > 1.f || std::abs(interpStepZ) > 0.1f;
        if (movingOrSettling && mFootstepDebugTimer <= 0.f)
        {
            Log(Debug::Verbose) << "[MP] Interp " << mName << " fwd=" << mState.animFlags.animFwd
                             << " side=" << mState.animFlags.animSide
                             << " run=" << ((mState.animFlags.movementFlags & AnimFlags::MF_RUN) != 0)
                             << " sneak=" << ((mState.animFlags.movementFlags & AnimFlags::MF_SNEAK) != 0)
                             << " wallBlocked=" << ((mState.animFlags.movementFlags & AnimFlags::MF_WALL_BLOCKED) != 0)
                             << " blockedSpeed=" << mState.animFlags.blockedMoveSpeed << " netPlanar=" << netPlanarSpeed
                             << " interpPlanar=" << interpPlanarSpeed << " remainingPlanar=" << remainingPlanar
                             << " posAlpha=" << posAlpha << " zStep=" << interpStepZ
                             << " jump=" << ((mState.animFlags.movementFlags & AnimFlags::MF_JUMP) != 0);
            mFootstepDebugTimer = 0.25f;
        }

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
        if (!isIdleGrounded)
        {
            const float vx = mState.velocity.linear[0];
            const float vy = mState.velocity.linear[1];
            if (vx != 0.f || vy != 0.f)
            {
                // Advance target
                mInterp.tx += vx * dt;
                mInterp.ty += vy * dt;

                // Cap drift dynamically based on velocity instead of a fixed 24u limit
                // so extreme speeds (e.g. jump spells) don't hit the interpolation ceiling.
                const float speedSq = vx * vx + vy * vy;
                const float maxExtrapDist = std::max(24.f, std::sqrt(speedSq) * 0.1f);

                const float driftX = mInterp.tx - mInterp.lastRecvX;
                const float driftY = mInterp.ty - mInterp.lastRecvY;
                const float driftSq = driftX * driftX + driftY * driftY;
                if (driftSq > maxExtrapDist * maxExtrapDist)
                {
                    const float scale = maxExtrapDist / std::sqrt(driftSq);
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
        if (!world)
            return false;

        MWWorld::Ptr localPlayer = world->getPlayerPtr();
        if (localPlayer.isEmpty() || !localPlayer.isInCell())
            return false;

        const MWWorld::CellStore* localCellStore = localPlayer.getCell();
        if (!localCellStore)
            return false;

        const MWWorld::Cell* localCell = localCellStore->getCell();
        if (!localCell)
            return false;

        const bool remoteExt = mState.cell.isExterior;
        const bool localExt = localCell->isExterior();
        if (remoteExt != localExt)
        {
            if (!quiet)
                Log(Debug::Verbose) << "[MP] isInSameCell(" << mName << ")=false:"
                                    << " exterior mismatch remote=" << remoteExt << " local=" << localExt;
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
                                    << " remote cell (" << mState.cell.gridX << "," << mState.cell.gridY
                                    << ") is no longer active";
            return match;
        }

        // Both interior — compare name
        const std::string localName = std::string(localCell->getNameId());
        const bool match = (mState.cell.cellName == localName);
        if (!match && !quiet)
            Log(Debug::Verbose) << "[MP] isInSameCell(" << mName << ")=false:"
                                << " name mismatch remote='" << mState.cell.cellName << "' local='" << localName << "'";
        return match;
    }

    // ===========================================================================
    // PlayerList
    // ===========================================================================

    void PlayerList::addPlayer(uint32_t guid, const std::string& name)
    {
        if (mPlayers.count(guid))
        {
            Log(Debug::Warning) << "[MP] PlayerList::addPlayer: guid " << guid << " already exists";
            return;
        }
        mPlayers.emplace(guid, std::make_unique<RemotePlayer>(guid, name));
    }

    void PlayerList::removePlayer(uint32_t guid)
    {
        auto it = mPlayers.find(guid);
        if (it == mPlayers.end())
        {
            Log(Debug::Warning) << "[MP] PlayerList::removePlayer: guid " << guid << " not found";
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
