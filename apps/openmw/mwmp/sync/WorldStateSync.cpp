#include "WorldStateSync.hpp"

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadacti.hpp>
#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadbook.hpp>
#include <components/esm3/loadalch.hpp>
#include <components/esm3/loadclot.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loaddoor.hpp>
#include <components/esm3/loadench.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm3/loadmisc.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadspel.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/esm3/loadweap.hpp>
#include <components/lua/serialization.hpp>
#include <components/openmw-mp/Packets/Worldstate/PacketWorldTime.hpp>
#include <sol/sol.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/statemanager.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwlua/magictypebindings.hpp"
#include "../../mwlua/types/types.hpp"
#include "../../mwworld/cell.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/globals.hpp"
#include "../../mwworld/datetimemanager.hpp"
#include "../../mwworld/esmstore.hpp"
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

static sol::state& dynamicRecordLuaState()
{
    static sol::state lua;
    return lua;
}

template <class Record, class Parser>
static void applyDynamicRecordUpsert(
    MWWorld::ESMStore& store, const std::string& recordId, const LuaUtil::BinaryData& data, Parser&& parser)
{
    sol::object decoded = LuaUtil::deserialize(dynamicRecordLuaState().lua_state(), data);
    if (!decoded.is<sol::table>())
        throw std::runtime_error("Dynamic record payload is not a Lua table");

    Record record = parser(decoded.as<sol::table>());
    record.mId = ESM::RefId::stringRefId(recordId);
    store.overrideRecord(record);
}

template <class Record>
static bool eraseDynamicRecord(MWWorld::ESMStore& store, const std::string& recordId)
{
    return store.getWritable<Record>().eraseStatic(ESM::RefId::stringRefId(recordId));
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

    processPendingDynamicRecords();

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

void WorldStateSync::onServerRecordDynamic(
    DynamicRecordAction action, const std::string& recordType, std::vector<DynamicRecordEntry> entries)
{
    const std::string normalizedType = normalizeDynamicRecordType(recordType);
    if (normalizedType.empty() || entries.empty())
    {
        Log(Debug::Warning) << "[MP] WorldStateSync: ignored dynamic record packet with invalid type='"
                            << recordType << "'";
        return;
    }

    PendingDynamicRecord pending;
    pending.action = action;
    pending.recordType = normalizedType;
    pending.entries = std::move(entries);
    mPendingDynamicRecords.push_back(std::move(pending));
    processPendingDynamicRecords();
}

void WorldStateSync::processPendingDynamicRecords()
{
    for (std::size_t i = 0; i < mPendingDynamicRecords.size();)
    {
        auto& pending = mPendingDynamicRecords[i];
        std::string error;
        if (applyDynamicRecord(pending, &error))
        {
            mPendingDynamicRecords.erase(mPendingDynamicRecords.begin() + i);
            continue;
        }

        if (!error.empty() && (pending.attempts == 0 || pending.lastError != error))
        {
            Log(Debug::Warning) << "[MP] WorldStateSync: delaying dynamic record type="
                                << pending.recordType << " reason=" << error;
            pending.lastError = error;
        }

        ++pending.attempts;
        ++i;
    }
}

bool WorldStateSync::applyDynamicRecord(const PendingDynamicRecord& record, std::string* error)
{
    if (!worldReady())
    {
        if (error) *error = "world_not_ready";
        return false;
    }

    MWBase::World* world = MWBase::Environment::get().getWorld();
    if (!world)
    {
        if (error) *error = "world_missing";
        return false;
    }

    MWWorld::ESMStore& store = world->getStore();

    try
    {
        for (const auto& entry : record.entries)
        {
            if (entry.recordId.empty())
                throw std::runtime_error("empty_record_id");

            if (record.action == DynamicRecordAction::Remove)
            {
                if (record.recordType == "activator")
                    eraseDynamicRecord<ESM::Activator>(store, entry.recordId);
                else if (record.recordType == "armor")
                    eraseDynamicRecord<ESM::Armor>(store, entry.recordId);
                else if (record.recordType == "book")
                    eraseDynamicRecord<ESM::Book>(store, entry.recordId);
                else if (record.recordType == "clothing")
                    eraseDynamicRecord<ESM::Clothing>(store, entry.recordId);
                else if (record.recordType == "container")
                    eraseDynamicRecord<ESM::Container>(store, entry.recordId);
                else if (record.recordType == "creature")
                    eraseDynamicRecord<ESM::Creature>(store, entry.recordId);
                else if (record.recordType == "door")
                    eraseDynamicRecord<ESM::Door>(store, entry.recordId);
                else if (record.recordType == "enchantment")
                    eraseDynamicRecord<ESM::Enchantment>(store, entry.recordId);
                else if (record.recordType == "light")
                    eraseDynamicRecord<ESM::Light>(store, entry.recordId);
                else if (record.recordType == "miscellaneous")
                    eraseDynamicRecord<ESM::Miscellaneous>(store, entry.recordId);
                else if (record.recordType == "npc")
                    eraseDynamicRecord<ESM::NPC>(store, entry.recordId);
                else if (record.recordType == "potion")
                    eraseDynamicRecord<ESM::Potion>(store, entry.recordId);
                else if (record.recordType == "spell")
                    eraseDynamicRecord<ESM::Spell>(store, entry.recordId);
                else if (record.recordType == "static")
                    eraseDynamicRecord<ESM::Static>(store, entry.recordId);
                else if (record.recordType == "weapon")
                    eraseDynamicRecord<ESM::Weapon>(store, entry.recordId);
                else
                    throw std::runtime_error("unsupported_record_type");
            }
            else
            {
                if (record.recordType == "activator")
                    applyDynamicRecordUpsert<ESM::Activator>(store, entry.recordId, entry.data, MWLua::tableToActivator);
                else if (record.recordType == "armor")
                    applyDynamicRecordUpsert<ESM::Armor>(store, entry.recordId, entry.data, MWLua::tableToArmor);
                else if (record.recordType == "book")
                    applyDynamicRecordUpsert<ESM::Book>(store, entry.recordId, entry.data, MWLua::tableToBook);
                else if (record.recordType == "clothing")
                    applyDynamicRecordUpsert<ESM::Clothing>(store, entry.recordId, entry.data, MWLua::tableToClothing);
                else if (record.recordType == "container")
                    applyDynamicRecordUpsert<ESM::Container>(store, entry.recordId, entry.data, MWLua::tableToContainer);
                else if (record.recordType == "creature")
                    applyDynamicRecordUpsert<ESM::Creature>(store, entry.recordId, entry.data, MWLua::tableToCreature);
                else if (record.recordType == "door")
                    applyDynamicRecordUpsert<ESM::Door>(store, entry.recordId, entry.data, MWLua::tableToDoor);
                else if (record.recordType == "enchantment")
                    applyDynamicRecordUpsert<ESM::Enchantment>(
                        store, entry.recordId, entry.data, MWLua::tableToEnchantment);
                else if (record.recordType == "light")
                    applyDynamicRecordUpsert<ESM::Light>(store, entry.recordId, entry.data, MWLua::tableToLight);
                else if (record.recordType == "miscellaneous")
                    applyDynamicRecordUpsert<ESM::Miscellaneous>(store, entry.recordId, entry.data, MWLua::tableToMisc);
                else if (record.recordType == "npc")
                    applyDynamicRecordUpsert<ESM::NPC>(store, entry.recordId, entry.data, MWLua::tableToNPC);
                else if (record.recordType == "potion")
                    applyDynamicRecordUpsert<ESM::Potion>(store, entry.recordId, entry.data, MWLua::tableToPotion);
                else if (record.recordType == "spell")
                    applyDynamicRecordUpsert<ESM::Spell>(store, entry.recordId, entry.data, MWLua::tableToSpell);
                else if (record.recordType == "static")
                    applyDynamicRecordUpsert<ESM::Static>(store, entry.recordId, entry.data, MWLua::tableToStatic);
                else if (record.recordType == "weapon")
                    applyDynamicRecordUpsert<ESM::Weapon>(store, entry.recordId, entry.data, MWLua::tableToWeapon);
                else
                    throw std::runtime_error("unsupported_record_type");
            }
        }
    }
    catch (const std::exception& e)
    {
        if (error) *error = e.what();
        return false;
    }

    return true;
}

} // namespace mwmp
