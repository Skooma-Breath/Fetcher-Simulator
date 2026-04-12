#include "LuaServerContext.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <optional>
#include <vector>

#include <sol/sol.hpp>

#include <components/debug/debuglog.hpp>
#include <components/lua/serialization.hpp>
#include <components/lua/utilpackage.hpp>
#include <components/openmw-mp/Packets/Lua/PacketLuaEvent.hpp>
#include <components/openmw-mp/Packets/Lua/PacketLuaStorage.hpp>
#include <components/vfs/filesystemarchive.hpp>

#include "Server.hpp"
#include "bindings/ServerBindings.hpp"
#include "bindings/TypesStubBindings.hpp"

namespace
{
    template <typename Fill>
    LuaUtil::BinaryData makeSerializedTable(const LuaUtil::LuaState& lua, Fill&& fill)
    {
        LuaUtil::BinaryData data;
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table payload = view.newTable();
            fill(payload);
            data = LuaUtil::serialize(payload);
        });
        return data;
    }

    template <typename Fill>
    LuaUtil::BinaryData makeMainThreadSerializedTable(Fill&& fill)
    {
        sol::state tempLua;
        sol::table payload = tempLua.create_table();
        fill(payload);
        return LuaUtil::serialize(payload);
    }

    LuaUtil::BinaryData injectPidIntoPayload(const LuaUtil::LuaState& lua, uint32_t pid, const LuaUtil::BinaryData& data)
    {
        if (pid == 0)
            return data;

        LuaUtil::BinaryData out = data;
        try
        {
            lua.protectedCall([&](LuaUtil::LuaView& view) {
                sol::object object = LuaUtil::deserialize(view.sol().lua_state(), data);
                if (object.is<sol::table>())
                {
                    sol::table payload = object.as<sol::table>();
                    payload["pid"] = pid;
                    out = LuaUtil::serialize(payload);
                }
            });
        }
        catch (const std::exception&)
        {
        }
        return out;
    }

std::optional<sol::table> loadConfigTable(LuaUtil::LuaState& lua, bool alreadyProtected = false)
    {
        std::optional<sol::table> config;

        auto load = [&]() {
            try
            {
                sol::function loader = lua.loadFromVFS(VFS::Path::Normalized("config.lua"));
                sol::environment env = lua.newInternalLibEnvironment();
                sol::set_environment(env, loader);
                sol::object result = LuaUtil::call(loader, std::string("config"));
                if (result.is<sol::table>())
                    config = result.as<sol::table>();
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "[LuaServerContext] config.lua load failed: " << e.what();
            }
        };

        if (alreadyProtected)
            load();
        else
            lua.protectedCall([&](LuaUtil::LuaView&) { load(); });

        return config;
    }

    std::filesystem::path resolveInternalLibsDir()
    {
        std::vector<std::filesystem::path> candidates = {
            std::filesystem::current_path() / "resources" / "lua_libs",
        };

#ifdef OPENMW_SERVER_SOURCE_LUA_LIBS_DIR
        candidates.emplace_back(OPENMW_SERVER_SOURCE_LUA_LIBS_DIR);
#endif

        for (const auto& candidate : candidates)
        {
            if (std::filesystem::exists(candidate) && std::filesystem::is_directory(candidate))
                return candidate;
        }

        return {};
    }
}

namespace mwmp
{

LuaServerContext::LuaServerContext(MPServer* server)
    : mServer(server)
{
    try
    {
        mScriptsDir = resolveScriptsDir();
        if (mScriptsDir.empty())
        {
            Log(Debug::Warning) << "[LuaServerContext] No server script directory found; Lua disabled";
            return;
        }

        buildVfs();
        initConfiguration();
        initLua();
        loadScripts();
    }
    catch (const std::exception& e)
    {
        Log(Debug::Warning) << "[LuaServerContext] Startup failed: " << e.what();
        mLoaded = false;
    }
}

LuaServerContext::~LuaServerContext()
{
    stop();
}

bool LuaServerContext::isRunning() const
{
    return mThreadRunning.load();
}

void LuaServerContext::start()
{
    if (!mLoaded || mThreadRunning.load())
        return;

    const int configuredTickRate = getInt("Config", "LUA_TICK_RATE", 20);
    const int tickRate = std::max(configuredTickRate, 1);
    mTickIntervalSeconds = 1.f / static_cast<float>(tickRate);
    mDiagnosticsIntervalSeconds = std::max(getInt("Config", "LUA_TICK_DIAGNOSTICS_INTERVAL", 5), 0);
    mImmediateIntentTimeoutMs = std::max(getInt("Config", "IMMEDIATE_INTENT_TIMEOUT_MS", 50), 1);
    mSlowTickLogThresholdMs = static_cast<double>(std::max(getInt("Config", "LUA_SLOW_TICK_MS", 8), 0));

    mStopRequested = false;
    mLuaThread = std::thread(&LuaServerContext::luaTickLoop, this);
    mThreadRunning = true;

    Log(Debug::Info) << "[LuaServerContext] Lua tick thread started at " << tickRate << " Hz";
    if (mDiagnosticsIntervalSeconds > 0)
    {
        Log(Debug::Info) << "[LuaServerContext] Tick diagnostics enabled: interval="
                         << mDiagnosticsIntervalSeconds << "s slowThreshold=" << mSlowTickLogThresholdMs << " ms";
    }
}

void LuaServerContext::stop()
{
    if (!mThreadRunning.load() && !mLuaThread.joinable())
        return;

    mStopRequested = true;
    mWakeCondition.notify_all();

    if (mLuaThread.joinable())
        mLuaThread.join();

    mThreadRunning = false;
}

void LuaServerContext::drainOutbound()
{
    if (!mServer)
        return;

    for (auto& action : mOutboundQueue.takeAll())
    {
        switch (action.type)
        {
            case OutboundLuaActionType::BroadcastServerMessage:
                mServer->broadcastServerMessage(action.text);
                break;
            case OutboundLuaActionType::BroadcastServerMessageToCell:
                mServer->broadcastServerMessageToCell(action.cellId, action.text);
                break;
            case OutboundLuaActionType::SendServerMessage:
                mServer->sendServerMessage(action.guid, action.text);
                break;
            case OutboundLuaActionType::RelayPlayerChat:
                mServer->relayPlayerChat(action.guid, action.text);
                break;
            case OutboundLuaActionType::KickClient:
                mServer->kickClient(action.guid, action.text);
                break;
            case OutboundLuaActionType::SetPlayerNickname:
                mServer->setPlayerNickname(action.guid, action.text);
                break;
            case OutboundLuaActionType::SetWorldHour:
                mServer->setWorldHour(action.worldHour);
                break;
            case OutboundLuaActionType::BroadcastLuaEvent:
                mServer->broadcastLuaEvent(0, action.eventName, action.eventData);
                break;
            case OutboundLuaActionType::BroadcastLuaEventToCell:
                mServer->broadcastLuaEventToCell(action.cellId, 0, action.eventName, action.eventData);
                break;
            case OutboundLuaActionType::SendLuaEvent:
                mServer->sendLuaEvent(action.guid, 0, action.eventName, action.eventData);
                break;
            case OutboundLuaActionType::BroadcastLuaStorage:
                mServer->broadcastLuaStorage(action.storageAction, action.storageSection, action.storageEntries);
                break;
            case OutboundLuaActionType::SendLuaStorage:
                mServer->sendLuaStorage(action.guid, action.storageAction, action.storageSection, action.storageEntries);
                break;
            case OutboundLuaActionType::GrantInventoryItem:
                mServer->grantPlayerInventoryItem(action.guid, action.text, action.itemCount);
                break;
            case OutboundLuaActionType::RemovePlacedObject:
                mServer->removePlacedObjectByMpNum(action.mpNum, action.cellId);
                break;
        }
    }
}

void LuaServerContext::syncSnapshot(double uptime, float worldHour, const std::vector<LuaPlayerSnapshot>& players)
{
    std::lock_guard<std::mutex> lock(mSnapshotMutex);
    mSnapshot.uptime = uptime;
    mSnapshot.worldHour = worldHour;
    mSnapshot.players = players;
}

void LuaServerContext::onServerInit()
{
    if (!mLoaded)
        return;

    enqueueEvent(0, "OnServerInit", makeEmptyPayload());
}

void LuaServerContext::onPlayerConnect(uint32_t guid, const std::string& name)
{
    if (!mLoaded)
        return;

    enqueueEvent(0, "OnPlayerConnect", makePlayerPayload(guid, name));
}

void LuaServerContext::onPlayerDisconnect(uint32_t guid, const std::string& name, const std::string& reason)
{
    if (!mLoaded)
        return;

    enqueueEvent(0, "OnPlayerDisconnect", makeDisconnectPayload(guid, name, reason));
}

void LuaServerContext::onPlayerCellChange(
    uint32_t guid, const std::string& name, const std::string& newCell, const std::string& oldCell)
{
    if (!mLoaded)
        return;

    enqueueEvent(0, "OnPlayerCellChange", makeCellChangePayload(guid, name, newCell, oldCell));
}

void LuaServerContext::onPlayerSendMessage(uint32_t guid, const std::string& name, const std::string& message)
{
    if (!mLoaded)
        return;

    enqueueEvent(0, "OnPlayerSendMessage", makeChatPayload(guid, name, message));
}

void LuaServerContext::onDoorState(const std::string& cellId, const std::string& refId, bool isOpen)
{
    if (!mLoaded)
        return;

    enqueueEvent(0, "OnDoorState", makeDoorPayload(cellId, refId, isOpen));
}

void LuaServerContext::onWorldWeather(const std::string& region, int current, int next, float transitionFactor)
{
    if (!mLoaded)
        return;

    enqueueEvent(0, "OnWorldWeather", makeWeatherPayload(region, current, next, transitionFactor));
}

void LuaServerContext::onLuaEvent(uint32_t pid, const std::string& eventName, const LuaUtil::BinaryData& data)
{
    if (!mLoaded)
        return;

    enqueueEvent(pid, eventName, data);
}

std::optional<LuaUtil::BinaryData> LuaServerContext::evaluateImmediateIntent(
    uint32_t pid, const std::string& eventName, const LuaUtil::BinaryData& data, std::string* error)
{
    if (!mLoaded)
    {
        if (error)
            *error = "lua_not_loaded";
        return std::nullopt;
    }

    auto request = std::make_shared<ImmediateIntentRequest>();
    request->pid = pid;
    request->eventName = eventName;
    request->data = data;

    {
        std::lock_guard<std::mutex> lock(mImmediateIntentMutex);
        mImmediateIntentRequests.push_back(request);
    }
    mWakeCondition.notify_all();

    std::unique_lock<std::mutex> lock(request->mutex);
    const bool completed = request->condition.wait_for(
        lock, std::chrono::milliseconds(mImmediateIntentTimeoutMs), [&request]() { return request->completed; });
    if (!completed)
    {
        if (error)
            *error = "timeout";
        return std::nullopt;
    }

    if (error)
        *error = request->error;
    return request->response;
}

void LuaServerContext::requestGlobalStorageSnapshot(uint32_t guid)
{
    if (!mLoaded || guid == 0)
        return;

    {
        std::lock_guard<std::mutex> lock(mStorageSnapshotMutex);
        mStorageSnapshotRequests.push_back(guid);
    }
    mWakeCondition.notify_all();
}

std::string LuaServerContext::getString(
    const std::string& tableName, const std::string& key, const std::string& defaultVal) const
{
    if (!mLoaded || tableName != "Config")
        return defaultVal;

    std::string out = defaultVal;
    const std::optional<sol::table> config = loadConfigTable(*mLua);
    if (!config)
        return out;

    auto maybeValue = config->get<sol::optional<std::string>>(key);
    if (maybeValue)
        out = *maybeValue;
    return out;
}

int LuaServerContext::getInt(const std::string& tableName, const std::string& key, int defaultVal) const
{
    if (!mLoaded || tableName != "Config")
        return defaultVal;

    int out = defaultVal;
    const std::optional<sol::table> config = loadConfigTable(*mLua);
    if (!config)
        return out;

    auto maybeValue = config->get<sol::optional<int>>(key);
    if (maybeValue)
        out = *maybeValue;
    return out;
}

bool LuaServerContext::getBool(const std::string& tableName, const std::string& key, bool defaultVal) const
{
    if (!mLoaded || tableName != "Config")
        return defaultVal;

    bool out = defaultVal;
    const std::optional<sol::table> config = loadConfigTable(*mLua);
    if (!config)
        return out;

    auto maybeValue = config->get<sol::optional<bool>>(key);
    if (maybeValue)
        out = *maybeValue;
    return out;
}

std::optional<LuaPlayerSnapshot> LuaServerContext::getPlayer(uint32_t guid) const
{
    std::lock_guard<std::mutex> lock(mSnapshotMutex);
    auto it = std::find_if(mSnapshot.players.begin(), mSnapshot.players.end(),
        [guid](const LuaPlayerSnapshot& player) { return player.guid == guid; });
    if (it == mSnapshot.players.end())
        return std::nullopt;
    return *it;
}

std::vector<LuaPlayerSnapshot> LuaServerContext::getPlayers() const
{
    std::lock_guard<std::mutex> lock(mSnapshotMutex);
    return mSnapshot.players;
}

int LuaServerContext::getPlayerCount() const
{
    std::lock_guard<std::mutex> lock(mSnapshotMutex);
    return static_cast<int>(mSnapshot.players.size());
}

double LuaServerContext::getUptime() const
{
    std::lock_guard<std::mutex> lock(mSnapshotMutex);
    return mSnapshot.uptime;
}

float LuaServerContext::getWorldHour() const
{
    std::lock_guard<std::mutex> lock(mSnapshotMutex);
    return mSnapshot.worldHour;
}

std::optional<PlacedObject> LuaServerContext::getPlacedObject(uint32_t mpNum) const
{
    std::lock_guard<std::mutex> lock(mPlacedObjectsMutex);
    auto it = mPlacedObjectsByMpNum.find(mpNum);
    if (it == mPlacedObjectsByMpNum.end())
        return std::nullopt;
    return it->second;
}

bool LuaServerContext::queueIntentOps(const sol::table& ops, std::string* error)
{
    const std::size_t opCount = ops.size();
    for (std::size_t i = 1; i <= opCount; ++i)
    {
        sol::optional<sol::table> op = ops.get<sol::optional<sol::table>>(i);
        if (!op)
        {
            if (error)
                *error = "op_" + std::to_string(i) + "_not_table";
            return false;
        }

        const std::string type = op->get_or("type", std::string());
        if (type == "GrantInventory" || type == "grantInventory")
        {
            const uint32_t guid = op->get_or("guid", 0u);
            const std::string refId = op->get_or("refId", std::string());
            const int count = op->get_or("count", 0);
            if (guid == 0 || refId.empty() || count <= 0)
            {
                if (error)
                    *error = "grant_inventory_invalid";
                return false;
            }
            queueGrantInventoryItem(guid, refId, count);
        }
        else if (type == "RemovePlacedObject" || type == "removePlacedObject")
        {
            const uint32_t mpNum = op->get_or("mpNum", 0u);
            const std::string cellId = op->get_or("cellId", op->get_or("cell", std::string()));
            if (mpNum == 0 || cellId.empty())
            {
                if (error)
                    *error = "remove_placed_object_invalid";
                return false;
            }
            queueRemovePlacedObject(mpNum, cellId);
        }
        else
        {
            if (error)
                *error = "unsupported_op_type:" + type;
            return false;
        }
    }

    return true;
}

void LuaServerContext::queueBroadcastServerMessage(const std::string& text)
{
    OutboundLuaAction action;
    action.type = OutboundLuaActionType::BroadcastServerMessage;
    action.text = text;
    mOutboundQueue.push(std::move(action));
}

void LuaServerContext::queueBroadcastServerMessageToCell(const std::string& cellId, const std::string& text)
{
    OutboundLuaAction action;
    action.type = OutboundLuaActionType::BroadcastServerMessageToCell;
    action.cellId = cellId;
    action.text = text;
    mOutboundQueue.push(std::move(action));
}

void LuaServerContext::queueSendServerMessage(uint32_t guid, const std::string& text)
{
    OutboundLuaAction action;
    action.type = OutboundLuaActionType::SendServerMessage;
    action.guid = guid;
    action.text = text;
    mOutboundQueue.push(std::move(action));
}

void LuaServerContext::queueRelayPlayerChat(uint32_t guid, const std::string& text)
{
    OutboundLuaAction action;
    action.type = OutboundLuaActionType::RelayPlayerChat;
    action.guid = guid;
    action.text = text;
    mOutboundQueue.push(std::move(action));
}

void LuaServerContext::queueKickClient(uint32_t guid, const std::string& reason)
{
    OutboundLuaAction action;
    action.type = OutboundLuaActionType::KickClient;
    action.guid = guid;
    action.text = reason;
    mOutboundQueue.push(std::move(action));
}

void LuaServerContext::queueSetPlayerNickname(uint32_t guid, const std::string& nickname)
{
    OutboundLuaAction action;
    action.type = OutboundLuaActionType::SetPlayerNickname;
    action.guid = guid;
    action.text = nickname;
    mOutboundQueue.push(std::move(action));
}

void LuaServerContext::queueSetWorldHour(float hour)
{
    OutboundLuaAction action;
    action.type = OutboundLuaActionType::SetWorldHour;
    action.worldHour = hour;
    mOutboundQueue.push(std::move(action));
}

void LuaServerContext::queueBroadcastLuaEvent(const std::string& eventName, const LuaUtil::BinaryData& data)
{
    OutboundLuaAction action;
    action.type = OutboundLuaActionType::BroadcastLuaEvent;
    action.eventName = eventName;
    action.eventData = data;
    mOutboundQueue.push(std::move(action));
}

void LuaServerContext::queueBroadcastLuaEventToCell(
    const std::string& cellId, const std::string& eventName, const LuaUtil::BinaryData& data)
{
    OutboundLuaAction action;
    action.type = OutboundLuaActionType::BroadcastLuaEventToCell;
    action.cellId = cellId;
    action.eventName = eventName;
    action.eventData = data;
    mOutboundQueue.push(std::move(action));
}

void LuaServerContext::queueSendLuaEvent(uint32_t guid, const std::string& eventName, const LuaUtil::BinaryData& data)
{
    OutboundLuaAction action;
    action.type = OutboundLuaActionType::SendLuaEvent;
    action.guid = guid;
    action.eventName = eventName;
    action.eventData = data;
    mOutboundQueue.push(std::move(action));
}

void LuaServerContext::queueBroadcastLuaStorageDelta(
    const std::string& section, const std::string& key, const LuaUtil::BinaryData& value)
{
    OutboundLuaAction action;
    action.type = OutboundLuaActionType::BroadcastLuaStorage;
    action.storageAction = LuaStorageAction::Delta;
    action.storageEntries.push_back({ section, key, value });
    mOutboundQueue.push(std::move(action));
}

void LuaServerContext::queueBroadcastLuaStorageSection(const std::string& section, std::vector<LuaStorageEntry> entries)
{
    OutboundLuaAction action;
    action.type = OutboundLuaActionType::BroadcastLuaStorage;
    action.storageAction = LuaStorageAction::ResetSection;
    action.storageSection = section;
    action.storageEntries = std::move(entries);
    mOutboundQueue.push(std::move(action));
}

void LuaServerContext::queueGrantInventoryItem(uint32_t guid, const std::string& refId, int count)
{
    OutboundLuaAction action;
    action.type = OutboundLuaActionType::GrantInventoryItem;
    action.guid = guid;
    action.text = refId;
    action.itemCount = count;
    mOutboundQueue.push(std::move(action));
}

void LuaServerContext::queueRemovePlacedObject(uint32_t mpNum, const std::string& cellId)
{
    OutboundLuaAction action;
    action.type = OutboundLuaActionType::RemovePlacedObject;
    action.mpNum = mpNum;
    action.cellId = cellId;
    mOutboundQueue.push(std::move(action));
}

void LuaServerContext::setPlayerData(uint32_t guid, const std::string& key, const std::string& value)
{
    std::lock_guard<std::mutex> lock(mPlayerDataMutex);
    mPlayerScriptData[guid][key] = value;
}

std::optional<std::string> LuaServerContext::getPlayerData(uint32_t guid, const std::string& key) const
{
    std::lock_guard<std::mutex> lock(mPlayerDataMutex);
    auto playerIt = mPlayerScriptData.find(guid);
    if (playerIt == mPlayerScriptData.end())
        return std::nullopt;

    auto valueIt = playerIt->second.find(key);
    if (valueIt == playerIt->second.end())
        return std::nullopt;

    return valueIt->second;
}

void LuaServerContext::clearPlayerData(uint32_t guid)
{
    std::lock_guard<std::mutex> lock(mPlayerDataMutex);
    mPlayerScriptData.erase(guid);
}

void LuaServerContext::syncPlacedObjects(std::vector<PlacedObject> objects)
{
    std::lock_guard<std::mutex> lock(mPlacedObjectsMutex);
    mPlacedObjectsByMpNum.clear();
    for (auto& object : objects)
    {
        if (object.mpNum == 0)
            continue;
        mPlacedObjectsByMpNum[object.mpNum] = std::move(object);
    }
}

void LuaServerContext::upsertPlacedObject(PlacedObject object)
{
    if (object.mpNum == 0)
        return;

    std::lock_guard<std::mutex> lock(mPlacedObjectsMutex);
    mPlacedObjectsByMpNum[object.mpNum] = std::move(object);
}

void LuaServerContext::removePlacedObject(uint32_t mpNum)
{
    if (mpNum == 0)
        return;

    std::lock_guard<std::mutex> lock(mPlacedObjectsMutex);
    mPlacedObjectsByMpNum.erase(mpNum);
}

std::filesystem::path LuaServerContext::resolveScriptsDir() const
{
    std::vector<std::filesystem::path> candidates = {
        std::filesystem::current_path() / "server-scripts",
        std::filesystem::current_path() / "apps" / "openmw-server" / "scripts",
    };

#ifdef OPENMW_SERVER_SOURCE_SCRIPTS_DIR
    candidates.emplace_back(OPENMW_SERVER_SOURCE_SCRIPTS_DIR);
#endif

    for (const auto& candidate : candidates)
    {
        if (std::filesystem::exists(candidate) && std::filesystem::is_directory(candidate))
            return candidate;
    }

    return {};
}

void LuaServerContext::buildVfs()
{
    mVfs.reset();
    mVfs.addArchive(std::make_unique<VFS::FileSystemArchive>(mScriptsDir));
    mVfs.buildIndex();
}

void LuaServerContext::initConfiguration()
{
    ESM::LuaScriptsCfg cfg;

    std::vector<std::filesystem::path> manifests;
    for (const auto& entry : std::filesystem::directory_iterator(mScriptsDir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".omwscripts")
            manifests.push_back(entry.path());
    }
    std::sort(manifests.begin(), manifests.end());

    if (!manifests.empty())
    {
        for (const auto& manifest : manifests)
        {
            std::ifstream stream(manifest);
            std::string data((std::istreambuf_iterator<char>(stream)), {});
            LuaUtil::parseOMWScripts(cfg, data);
            Log(Debug::Info) << "[LuaServerContext] Parsed manifest: " << manifest.filename();
        }
    }
    else if (std::filesystem::exists(mScriptsDir / "core.lua"))
    {
        LuaUtil::parseOMWScripts(cfg, "GLOBAL: core.lua\n");
        Log(Debug::Info) << "[LuaServerContext] No .omwscripts found; defaulting to GLOBAL: core.lua";
    }

    mConfiguration.init(std::move(cfg), false);
}

void LuaServerContext::initLua()
{
    if (mConfiguration.getGlobalConf().empty())
    {
        Log(Debug::Warning) << "[LuaServerContext] No GLOBAL scripts configured; Lua disabled";
        return;
    }

    // Dedicated server Lua does not use the in-engine profiler UI, and the
    // count-hook/allocator tracking can show up as recurring hitches.
    LuaUtil::LuaState::disableProfiler();
    mLua = std::make_unique<LuaUtil::LuaState>(&mVfs, &mConfiguration, LuaUtil::LuaStateSettings{});
    mLua->addInternalLibSearchPath(mScriptsDir);
    if (const std::filesystem::path internalLibsDir = resolveInternalLibsDir(); !internalLibsDir.empty())
        mLua->addInternalLibSearchPath(internalLibsDir);
    else
        Log(Debug::Warning) << "[LuaServerContext] No internal Lua libs directory found; built-in packages may fail";

    mLua->protectedCall([&](LuaUtil::LuaView& view) {
        LuaUtil::LuaStorage::initLuaBindings(view);
        mGlobalStorage.setActive(true);
        mGlobalStorage.setListener(&mStorageSyncListener);

        mLua->addCommonPackage("mp", initMpPackage(view, this, &mGlobalStorage));

        const std::optional<sol::table> config = loadConfigTable(*mLua, true);
        if (config)
            mLua->addCommonPackage("config", *config);
        else
            mLua->addCommonPackage("config", view.newTable());

        mLua->addCommonPackage("openmw.util", LuaUtil::initUtilPackage(view.sol().lua_state()));
        mLua->addCommonPackage("openmw.storage", LuaUtil::LuaStorage::initGlobalPackage(view, &mGlobalStorage));
        mLua->addCommonPackage("openmw.types", initTypesStubPackage(view));
    });
}

void LuaServerContext::loadScripts()
{
    if (!mLua)
        return;

    mGlobalScripts = std::make_unique<LuaUtil::ScriptsContainer>(mLua.get(), "Server");
    mGlobalScripts->setAutoStartConf(mConfiguration.getGlobalConf());
    mGlobalScripts->addAutoStartedScripts();
    mLoaded = true;

    Log(Debug::Info) << "[LuaServerContext] Loaded "
                     << mConfiguration.getGlobalConf().size()
                     << " global server Lua script(s) from " << mScriptsDir;
}

std::size_t LuaServerContext::dispatchQueuedEvents()
{
    if (!mLoaded)
        return 0;

    std::vector<InboundLuaEvent> events = mInboundQueue.takeAll();
    for (auto& event : events)
        mGlobalScripts->receiveEvent(event.name, injectPidIntoPayload(*mLua, event.pid, event.data));
    return events.size();
}

std::size_t LuaServerContext::processImmediateIntentRequests()
{
    if (!mLoaded)
        return 0;

    std::vector<std::shared_ptr<ImmediateIntentRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(mImmediateIntentMutex);
        requests.swap(mImmediateIntentRequests);
    }

    for (const auto& request : requests)
    {
        try
        {
            request->response = evaluateImmediateIntentOnLuaThread(request->pid, request->eventName, request->data);
            if (!request->response)
                request->error = "no_result";
        }
        catch (const std::exception& e)
        {
            request->error = e.what();
        }

        {
            std::lock_guard<std::mutex> requestLock(request->mutex);
            request->completed = true;
        }
        request->condition.notify_all();
    }

    return requests.size();
}

void LuaServerContext::processStorageSnapshotRequests()
{
    std::vector<uint32_t> requests;
    {
        std::lock_guard<std::mutex> lock(mStorageSnapshotMutex);
        requests.swap(mStorageSnapshotRequests);
    }

    if (requests.empty())
        return;

    std::vector<LuaStorageEntry> entries = makeGlobalStorageSnapshot();
    for (uint32_t guid : requests)
    {
        OutboundLuaAction action;
        action.type = OutboundLuaActionType::SendLuaStorage;
        action.guid = guid;
        action.storageAction = LuaStorageAction::Snapshot;
        action.storageEntries = entries;
        mOutboundQueue.push(std::move(action));
    }
}

std::vector<LuaStorageEntry> LuaServerContext::makeGlobalStorageSnapshot() const
{
    std::vector<LuaStorageEntry> entries;
    for (const auto& value : mGlobalStorage.getSerializedValues())
        entries.push_back({ value.mSection, value.mKey, value.mValue });
    return entries;
}

void LuaServerContext::luaTickLoop()
{
    using clock = std::chrono::steady_clock;
    auto nextWake = clock::now();
    auto diagnosticsWindowStart = nextWake;
    std::size_t windowTicks = 0;
    std::size_t windowEvents = 0;
    std::size_t windowPeakEvents = 0;
    double windowTotalMs = 0.0;
    double windowMaxMs = 0.0;

    while (!mStopRequested.load())
    {
        nextWake += std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<float>(mTickIntervalSeconds));
        const auto tickStart = clock::now();
        std::size_t processedEvents = 0;

        try
        {
            processedEvents += processImmediateIntentRequests();
            processStorageSnapshotRequests();
            processedEvents += dispatchQueuedEvents();
            mGlobalScripts->receiveEvent("OnServerTick", makeTickPayload());
            mGlobalScripts->update(mTickIntervalSeconds);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Error) << "[LuaServerContext] Lua tick error: " << e.what();
        }

        const auto tickEnd = clock::now();
        const double elapsedMs = std::chrono::duration<double, std::milli>(tickEnd - tickStart).count();
        ++windowTicks;
        windowEvents += processedEvents;
        windowPeakEvents = std::max(windowPeakEvents, processedEvents);
        windowTotalMs += elapsedMs;
        windowMaxMs = std::max(windowMaxMs, elapsedMs);

        if (mSlowTickLogThresholdMs > 0.0 && elapsedMs >= mSlowTickLogThresholdMs)
        {
            Log(Debug::Warning) << "[LuaServerContext] Slow Lua tick: "
                                << elapsedMs << " ms (events=" << processedEvents << ")";
        }

        if (mDiagnosticsIntervalSeconds > 0
            && tickEnd - diagnosticsWindowStart >= std::chrono::seconds(mDiagnosticsIntervalSeconds))
        {
            const double avgMs = windowTicks > 0 ? windowTotalMs / static_cast<double>(windowTicks) : 0.0;
            Log(Debug::Info) << "[LuaServerContext] Tick diagnostics: ticks=" << windowTicks
                             << " avg=" << avgMs << " ms"
                             << " max=" << windowMaxMs << " ms"
                             << " events=" << windowEvents
                             << " peakEvents=" << windowPeakEvents;
            diagnosticsWindowStart = tickEnd;
            windowTicks = 0;
            windowEvents = 0;
            windowPeakEvents = 0;
            windowTotalMs = 0.0;
            windowMaxMs = 0.0;
        }

        std::unique_lock<std::mutex> lock(mWakeMutex);
        mWakeCondition.wait_until(lock, nextWake);
    }
}

std::optional<LuaUtil::BinaryData> LuaServerContext::evaluateImmediateIntentOnLuaThread(
    uint32_t pid, const std::string& eventName, const LuaUtil::BinaryData& data)
{
    if (!mGlobalScripts)
        throw std::runtime_error("global_scripts_missing");

    std::optional<LuaUtil::BinaryData> out;
    mLua->protectedCall([&](LuaUtil::LuaView& view) {
        LuaUtil::BinaryData payload = injectPidIntoPayload(*mLua, pid, data);
        sol::object payloadObject = LuaUtil::deserialize(view.sol().lua_state(), payload);
        auto resultObject = mGlobalScripts->callPublicInterface<sol::object>("IntentPolicy", "evaluateIntent", eventName, payloadObject);
        if (!resultObject || !resultObject->is<sol::table>())
            throw std::runtime_error("intent_policy_interface_missing");

        sol::table result = resultObject->as<sol::table>();
        sol::object opsObject = result["ops"];
        if (opsObject.is<sol::table>())
        {
            std::string opError;
            if (!queueIntentOps(opsObject.as<sol::table>(), &opError))
                throw std::runtime_error("intent_ops_error:" + opError);
            result["ops"] = sol::nil;
        }

        out = LuaUtil::serialize(result);
    });
    return out;
}

LuaUtil::BinaryData LuaServerContext::makeEmptyPayload() const
{
    return makeMainThreadSerializedTable([](sol::table&) {});
}

LuaUtil::BinaryData LuaServerContext::makeTickPayload() const
{
    return makeSerializedTable(*mLua, [this](sol::table& payload) {
        payload["dt"] = mTickIntervalSeconds;
    });
}

LuaUtil::BinaryData LuaServerContext::makePlayerPayload(uint32_t guid, const std::string& name) const
{
    return makeMainThreadSerializedTable([guid, &name](sol::table& payload) {
        payload["guid"] = guid;
        payload["name"] = name;
    });
}

LuaUtil::BinaryData LuaServerContext::makeDisconnectPayload(
    uint32_t guid, const std::string& name, const std::string& reason) const
{
    return makeMainThreadSerializedTable([guid, &name, &reason](sol::table& payload) {
        payload["guid"] = guid;
        payload["name"] = name;
        payload["reason"] = reason;
    });
}

LuaUtil::BinaryData LuaServerContext::makeCellChangePayload(uint32_t guid, const std::string& name,
    const std::string& newCell, const std::string& oldCell) const
{
    return makeMainThreadSerializedTable([guid, &name, &newCell, &oldCell](sol::table& payload) {
        payload["guid"] = guid;
        payload["name"] = name;
        payload["newCell"] = newCell;
        payload["oldCell"] = oldCell;
    });
}

LuaUtil::BinaryData LuaServerContext::makeChatPayload(
    uint32_t guid, const std::string& name, const std::string& message) const
{
    return makeMainThreadSerializedTable([guid, &name, &message](sol::table& payload) {
        payload["guid"] = guid;
        payload["name"] = name;
        payload["message"] = message;
    });
}

LuaUtil::BinaryData LuaServerContext::makeDoorPayload(
    const std::string& cellId, const std::string& refId, bool isOpen) const
{
    return makeMainThreadSerializedTable([&cellId, &refId, isOpen](sol::table& payload) {
        payload["cellId"] = cellId;
        payload["refId"] = refId;
        payload["isOpen"] = isOpen;
    });
}

LuaUtil::BinaryData LuaServerContext::makeWeatherPayload(
    const std::string& region, int current, int next, float transitionFactor) const
{
    return makeMainThreadSerializedTable([&region, current, next, transitionFactor](sol::table& payload) {
        payload["region"] = region;
        payload["current"] = current;
        payload["next"] = next;
        payload["transitionFactor"] = transitionFactor;
    });
}

void LuaServerContext::enqueueEvent(uint32_t pid, std::string name, LuaUtil::BinaryData data)
{
    mInboundQueue.push({ pid, std::move(name), std::move(data) });
    mWakeCondition.notify_all();
}

void LuaServerContext::StorageSyncListener::valueChanged(
    std::string_view section, std::string_view key, const sol::object& value) const
{
    if (!mContext)
        return;

    try
    {
        mContext->queueBroadcastLuaStorageDelta(
            std::string(section), std::string(key), LuaUtil::serialize(value));
    }
    catch (const std::exception& e)
    {
        Log(Debug::Warning) << "[LuaServerContext] Failed to serialize storage delta: " << e.what();
    }
}

void LuaServerContext::StorageSyncListener::sectionReplaced(
    std::string_view section, const sol::optional<sol::table>& values) const
{
    if (!mContext)
        return;

    std::vector<LuaStorageEntry> entries;
    try
    {
        if (values)
        {
            for (const auto& [key, value] : *values)
            {
                entries.push_back({
                    std::string(section),
                    LuaUtil::cast<std::string>(key),
                    LuaUtil::serialize(value),
                });
            }
        }
        mContext->queueBroadcastLuaStorageSection(std::string(section), std::move(entries));
    }
    catch (const std::exception& e)
    {
        Log(Debug::Warning) << "[LuaServerContext] Failed to serialize storage section: " << e.what();
    }
}

} // namespace mwmp
