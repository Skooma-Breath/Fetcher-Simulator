#include "WorldStateSync.hpp"

#include <components/debug/debuglog.hpp>
#include <components/openmw-mp/Packets/Worldstate/PacketWorldTime.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/statemanager.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwworld/globals.hpp"
#include "../../mwworld/datetimemanager.hpp"

#include "../network/Client.hpp"

namespace mwmp
{

WorldStateSync::WorldStateSync(NetworkClient& client)
    : mClient(client)
{
}

// ---------------------------------------------------------------------------
// Try to apply the stored mTime/mTimeScale to the live world.
// Only proceeds if the StateManager reports State_Running — any earlier and
// the global variables may not be initialised from the save yet.
// Returns true on success; false if the world isn't ready (caller retries).
bool WorldStateSync::applyServerTime()
{
    if (MWBase::Environment::get().getStateManager()->getState()
            != MWBase::StateManager::State_Running)
        return false;

    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world)
        return false;

    // --- Timescale ---
    MWWorld::DateTimeManager* dtm = world->getTimeManager();
    if (dtm && std::abs(dtm->getGameTimeScale() - mTimeScale) > 0.1f)
    {
        dtm->setGameTimeScale(mTimeScale);
        Log(Debug::Verbose) << "[MP] WorldStateSync: timescale set to " << mTimeScale;
    }

    // --- Set time absolutely ---
    // advanceTime() is additive and cannot go backwards (setHour clamps < 0).
    // We must set each field directly so we can sync regardless of whether
    // the client's save is ahead of or behind the server clock.
    //
    // Order matters: year first (no cascade), then month (may clamp day if
    // the new month is shorter), then day, then hour last.
    const float  localHour  = world->getGlobalFloat(MWWorld::Globals::sGameHour);
    const int    localDay   = world->getGlobalInt  (MWWorld::Globals::sDay);
    const int    localMonth = world->getGlobalInt  (MWWorld::Globals::sMonth);
    const int    localYear  = world->getGlobalInt  (MWWorld::Globals::sYear);

    const bool sameTime = (localYear  == mTime.year)
                       && (localMonth == mTime.month)
                       && (localDay   == mTime.day)
                       && (std::abs(localHour - mTime.hour) < 0.01f);
    if (sameTime)
    {
        Log(Debug::Verbose) << "[MP] WorldStateSync: already in sync";
        return true;
    }

    world->setGlobalInt  (MWWorld::Globals::sYear,     mTime.year);
    world->setGlobalInt  (MWWorld::Globals::sMonth,    mTime.month);
    world->setGlobalInt  (MWWorld::Globals::sDay,      mTime.day);
    world->setGlobalFloat(MWWorld::Globals::sGameHour, mTime.hour);

    Log(Debug::Info) << "[MP] WorldStateSync: set time to "
                     << mTime.year << "-" << (mTime.month + 1) << "-" << mTime.day
                     << " " << mTime.hour << "h"
                     << " (was " << localYear << "-" << (localMonth + 1) << "-" << localDay
                     << " " << localHour << "h)";

    return true;
}

// ---------------------------------------------------------------------------
void WorldStateSync::update(float dt)
{
    if (!mClient.isConnected())
        return;

    // Retry applying server time on every tick until it succeeds.
    // applyServerTime() returns false while the world isn't in State_Running,
    // so this naturally waits out the loading screen without any extra flags.
    if (mHasServerTime && !mTimeApplied)
        mTimeApplied = applyServerTime();

    mSyncTimer += dt;
    if (mSyncTimer >= SYNC_RATE)
        mSyncTimer = 0.f;
}

// ---------------------------------------------------------------------------
void WorldStateSync::onServerTimeUpdate(const Time& t, float timeScale)
{
    mTime          = t;
    mTimeScale     = timeScale;
    mHasServerTime = true;
    mTimeApplied   = false; // always re-apply — server packet supersedes local state

    Log(Debug::Verbose) << "[MP] WorldStateSync: received time "
                        << t.year << "-" << (t.month + 1) << "-" << t.day
                        << " " << t.hour << "h scale=" << timeScale;

    // Attempt immediate application in case the world is already running.
    // If it isn't (loading screen), update() will retry each frame.
    mTimeApplied = applyServerTime();
}

// ---------------------------------------------------------------------------
void WorldStateSync::onServerWeatherUpdate(int current, int /*next*/, float /*transition*/)
{
    mWeather = current;
    Log(Debug::Verbose) << "[MP] WorldStateSync: weather=" << current;
    // Weather application in follow-up commit
}

} // namespace mwmp
