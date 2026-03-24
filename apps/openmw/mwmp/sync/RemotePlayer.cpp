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
#include "../../mwmechanics/creaturestats.hpp"

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
        // Use the "Player" NPC record as the base template.
        // This gives us a human-shaped actor with all the right animations.
        MWWorld::ManualRef ref(world->getStore(),
                               ESM::RefId::stringRefId("player"), 1);

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
        world->setActorCollisionMode(mNpcPtr, false, false);


        mIsSpawned = true;

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
    mIsSpawned       = false;
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
    const std::string& newName = state.name;
    if (newName == mName) return;  // nothing changed

    Log(Debug::Info) << "[MP] RemotePlayer " << mName
                     << " renamed to '" << newName << "'";
    mName = newName;

    // Update the user value used by the hover-tooltip system
    if (!mNpcPtr.isEmpty())
        mNpcPtr.getRefData().getBaseNode()->setUserValue("mp_player_name", mName);

    // Update the live nameplate text — no node rebuild needed
    if (mNameplate)
        mNameplate->updateName(mName);
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
// Linear-interpolate current position/rotation toward target each frame.
void RemotePlayer::updateInterpolation(float dt)
{
    if (!mInterp.hasTarget) return;

    // Position: smooth, slightly laggy — hides network jitter
    const float posAlpha = std::min(1.f, dt * POS_INTERP_SPEED);
    mInterp.cx += (mInterp.tx - mInterp.cx) * posAlpha;
    mInterp.cy += (mInterp.ty - mInterp.cy) * posAlpha;
    mInterp.cz += (mInterp.tz - mInterp.cz) * posAlpha;

    // Rotation: snappier — turns should feel responsive, not lag behind.
    // lerpAngle handles the 0/2π wrap so we always take the short path.
    const float rotAlpha = std::min(1.f, dt * ROT_INTERP_SPEED);
    mInterp.crx = lerpAngle(mInterp.crx, mInterp.trx,  rotAlpha);
    mInterp.cry = lerpAngle(mInterp.cry, mInterp.try_,  rotAlpha);
    mInterp.crz = lerpAngle(mInterp.crz, mInterp.trz,  rotAlpha);
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
