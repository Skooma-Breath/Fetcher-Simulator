#include "WorldStateSync.hpp"

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/openmw-mp/Packets/Worldstate/PacketWorldTime.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/statemanager.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwworld/cell.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/globals.hpp"
#include "../../mwworld/datetimemanager.hpp"
#include "../../mwworld/ptr.hpp"
#include "../../mwworld/weather.hpp"

#include "../network/Client.hpp"
#include "../Main.hpp"
#include "../sync/PlayerSync.hpp"

namespace mwmp
{

WorldStateSync::WorldStateSync(NetworkClient& client)
    : mClient(client)
{
}

// ---------------------------------------------------------------------------
// Returns true if State_Running and world is available.
static bool worldReady()
{
    return MWBase::Environment::get().getStateManager()->getState()
               == MWBase::StateManager::State_Running
        && MWBase::Environment::get().getWorld() != nullptr;
}

// ===========================================================================
// Time
// ===========================================================================

bool WorldStateSync::applyServerTime()
{
    if (!worldReady()) return false;

    MWBase::World* world = MWBase::Environment::get().getWorld();

    // Timescale
    MWWorld::DateTimeManager* dtm = world->getTimeManager();
    if (dtm && std::abs(dtm->getGameTimeScale() - mTimeScale) > 0.1f)
    {
        dtm->setGameTimeScale(mTimeScale);
        Log(Debug::Verbose) << "[MP] WorldStateSync: timescale set to " << mTimeScale;
    }

    // Set each field directly — advanceTime() is additive and cannot go backwards.
    // Order: year (no cascade) → month (may clamp day) → day → hour.
    const float localHour  = world->getGlobalFloat(MWWorld::Globals::sGameHour);
    const int   localDay   = world->getGlobalInt  (MWWorld::Globals::sDay);
    const int   localMonth = world->getGlobalInt  (MWWorld::Globals::sMonth);
    const int   localYear  = world->getGlobalInt  (MWWorld::Globals::sYear);

    const bool sameTime = (localYear  == mTime.year)
                       && (localMonth == mTime.month)
                       && (localDay   == mTime.day)
                       && (std::abs(localHour - mTime.hour) < 0.01f);
    if (sameTime)
    {
        Log(Debug::Verbose) << "[MP] WorldStateSync: time already in sync";
        return true;
    }

    world->setGlobalInt  (MWWorld::Globals::sYear,     mTime.year);
    world->setGlobalInt  (MWWorld::Globals::sMonth,    mTime.month);
    world->setGlobalInt  (MWWorld::Globals::sDay,      mTime.day);
    world->setGlobalFloat(MWWorld::Globals::sGameHour, mTime.hour);

    Log(Debug::Info) << "[MP] WorldStateSync: set time to "
                     << mTime.year << "-" << (mTime.month + 1) << "-" << mTime.day
                     << " " << mTime.hour << "h"
                     << " (was " << localYear << "-" << (localMonth + 1)
                     << "-" << localDay << " " << localHour << "h)";
    return true;
}

// ===========================================================================
// Weather
// ===========================================================================

bool WorldStateSync::applyServerWeather()
{
    if (!worldReady()) return false;
    if (mWeatherRegion.empty()) return true; // interior / no region — nothing to do

    MWBase::World* world = MWBase::Environment::get().getWorld();

    // Deserialise the region RefId that the host sent.
    const ESM::RefId region = ESM::RefId::deserializeText(mWeatherRegion);

    // changeWeather() tells the WeatherManager to transition to mWeather for
    // this region.  Transition timing is handled locally by each client's
    // WeatherManager — we don't try to sync the transition factor precisely.
    world->changeWeather(region, static_cast<unsigned int>(mWeather));

    Log(Debug::Info) << "[MP] WorldStateSync: set weather="
                     << mWeather << " region=" << mWeatherRegion;
    return true;
}

// ---------------------------------------------------------------------------
// Called on the host client each frame (when timer fires).
// Samples the local weather and current region, sends PacketWorldWeather.
void WorldStateSync::sendWeatherToServer()
{
    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world) return;

    // Only report weather from exterior cells that have a region.
    MWWorld::Ptr player = world->getPlayerPtr();
    if (player.isEmpty() || !player.isInCell()) return;

    const MWWorld::Cell* cell = player.getCell()->getCell();
    if (cell->isExterior() == false) return;      // interior — skip
    if (cell->getRegion().empty()) return;         // no region — skip

    const int   curWeather    = world->getCurrentWeatherScriptId();
    const auto* nextWeatherPtr = world->getNextWeather();
    const int   nxtWeather    = nextWeatherPtr ? nextWeatherPtr->mScriptId : -1;
    const float transition    = world->getWeatherTransition();
    const std::string region  = cell->getRegion().serializeText();

    PacketWorldWeather pkt;
    pkt.currentWeather   = curWeather;
    pkt.nextWeather      = nxtWeather;
    pkt.transitionFactor = transition;
    pkt.regionName       = region;

    mClient.sendReliable(pkt.encode());

    Log(Debug::Verbose) << "[MP] WorldStateSync: sent weather="
                        << curWeather << " region=" << region;
}

// ===========================================================================
// Public interface
// ===========================================================================

void WorldStateSync::update(float dt)
{
    if (!mClient.isConnected())
        return;

    // Retry time application until State_Running.
    if (mHasServerTime && !mTimeApplied)
        mTimeApplied = applyServerTime();

    // Retry weather application until State_Running.
    if (mHasServerWeather && !mWeatherApplied)
        mWeatherApplied = applyServerWeather();

    // Host (guid == 1) periodically reports local weather to the server.
    // Other clients receive and apply it — they never send it.
    if (Main::isInitialised()
        && Main::get().getPlayerSync().localPlayer().guid == 1
        && worldReady())
    {
        mWeatherReportTimer += dt;
        if (mWeatherReportTimer >= WEATHER_REPORT_RATE)
        {
            mWeatherReportTimer = 0.f;
            sendWeatherToServer();
        }
    }

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
    mTimeApplied   = false;

    Log(Debug::Verbose) << "[MP] WorldStateSync: received time "
                        << t.year << "-" << (t.month + 1) << "-" << t.day
                        << " " << t.hour << "h scale=" << timeScale;

    mTimeApplied = applyServerTime();
}

// ---------------------------------------------------------------------------
void WorldStateSync::onServerWeatherUpdate(int current, int next, float transition,
                                            const std::string& regionName)
{
    mWeather          = current;
    mNextWeather      = next;
    mTransitionFactor = transition;
    mWeatherRegion    = regionName;
    mHasServerWeather = true;
    mWeatherApplied   = false;

    Log(Debug::Verbose) << "[MP] WorldStateSync: received weather="
                        << current << " region=" << regionName;

    mWeatherApplied = applyServerWeather();
}

} // namespace mwmp
