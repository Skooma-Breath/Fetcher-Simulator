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
    mState.animFlags.movementType = -1;
    mLastAppliedAnimFlags.movementType = -1;
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

    switch (f.movementType)
    {
        case 0:  mov.mPosition[1] =  1.f; mov.mPosition[0] =  0.f; break; // forward
        case 1:  mov.mPosition[1] = -1.f; mov.mPosition[0] =  0.f; break; // backward
        case 2:  mov.mPosition[0] = -1.f; mov.mPosition[1] =  0.f; break; // strafe left
        case 3:  mov.mPosition[0] =  1.f; mov.mPosition[1] =  0.f; break; // strafe right
        case 4:  mov.mPosition[0] = -1.f; mov.mPosition[1] =  0.f; break; // strafe-l (alias)
        case 5:  mov.mPosition[0] =  1.f; mov.mPosition[1] =  0.f; break; // strafe-r (alias)
        default: mov.mPosition[0] =  0.f; mov.mPosition[1] =  0.f; break; // idle / standing
    }

    // Pass the interpolated yaw delta so standing turn animations trigger
    mov.mRotation[0] = 0.f;
    mov.mRotation[1] = 0.f;
    if (f.movementType < 0 && std::abs(mInterp.yawDelta) > 0.005f)
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
    if (isJumping != mWasJumping)
    {
        MWBase::Environment::get().getWorld()->setOnGround(mNpcPtr, !isJumping);
        mWasJumping = isJumping;
    }
    // The CharacterController only selects strafe animation groups (WalkLeft,
    // RunRight, etc.) when mIsStrafing == true — it completely ignores mPosition[0]
    // otherwise and falls back to WalkForward/WalkBack.  The sender's MF_STRAFING
    // flag only fires for drawn-weapon strafing (CC line ~2099), so unarmed
    // sideways walking (movementType 2/3/4/5) arrives with MF_STRAFING=0 and
    // the remote NPC plays walkforward instead.  Always force mIsStrafing when
    // the movementType tells us the actor is moving sideways.
    const bool isSideways = (f.movementType == 2 || f.movementType == 3
                          || f.movementType == 4 || f.movementType == 5);
    mov.mIsStrafing = isSideways || (f.movementFlags & AnimFlags::MF_STRAFING) != 0;


    MWMechanics::CreatureStats& stats = mNpcPtr.getClass().getCreatureStats(mNpcPtr);
    stats.setMovementFlag(MWMechanics::CreatureStats::Flag_ForceJump, isJumping);
    stats.setMovementFlag(MWMechanics::CreatureStats::Flag_ForceMoveJump, isJumping);
    stats.setMovementFlag(MWMechanics::CreatureStats::Flag_Run,
        (f.movementFlags & AnimFlags::MF_RUN) != 0);
    stats.setMovementFlag(MWMechanics::CreatureStats::Flag_Sneak,
        (f.movementFlags & AnimFlags::MF_SNEAK) != 0);
    stats.setAttackingOrSpell(
        (f.actionFlags & AnimFlags::AF_ATTACKING) != 0 || mState.attack.pressed);

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

    // Set new interpolation target
    mInterp.tx   = state.position.pos[0];
    mInterp.ty   = state.position.pos[1];
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
        && f.movementType == mLastAppliedAnimFlags.movementType)
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
    const bool isIdleGrounded = (mState.animFlags.movementType < 0) && !isAirborne;

    // Idle catch-up: faster XY convergence when standing still, but NOT during
    // a jump — the arc is a fast parabola and we don't want to fight it with
    // an inflated speed multiplier that makes the Z curve feel sluggish.
    float interpSpeed = POS_INTERP_SPEED;
    if (isIdleGrounded)
        interpSpeed *= 2.5f; // 15 * 2.5 = 37.5 u/s snap-to-stop on ground

    const float posAlpha = std::min(1.f, dt * interpSpeed);
    mInterp.cx += (mInterp.tx - mInterp.cx) * posAlpha;
    mInterp.cy += (mInterp.ty - mInterp.cy) * posAlpha;

    // Z-axis: fast interpolation for the parabolic jump arc.
    const float zAlpha = std::min(1.f, dt * 60.f);
    mInterp.cz += (mInterp.tz - mInterp.cz) * zAlpha;

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
            mInterp.cx = mInterp.tx;
            mInterp.cy = mInterp.ty;
        }
        if (std::abs(dz) < 3.f)
            mInterp.cz = mInterp.tz;
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
    const float netPlanarSpeed = std::sqrt(
        mState.velocity.linear[0] * mState.velocity.linear[0]
        + mState.velocity.linear[1] * mState.velocity.linear[1]);

    // Override the animation cadence using `netPlanarSpeed` from the sender to
    // eliminate choppy footstep pacing. Only fall back to `interpPlanarSpeed`
    // when idle (snapping to stop). Cap to 200 so huge spawns don't trigger the 10x cap.
    if (mState.animFlags.movementType >= 0)
        mInterpPlanarSpeed = netPlanarSpeed;
    else
        mInterpPlanarSpeed = std::min(interpPlanarSpeed, 200.f);

    const bool movingOrSettling = mState.animFlags.movementType >= 0
        || remainingPlanar > 1.f
        || std::abs(interpStepZ) > 0.1f;
    if (movingOrSettling && mFootstepDebugTimer <= 0.f)
    {
        Log(Debug::Info) << "[MPDBG] Interp " << mName
                         << " moveType=" << int(mState.animFlags.movementType)
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
