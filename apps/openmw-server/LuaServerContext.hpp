#ifndef OPENMW_SERVER_LUASERVERCONTEXT_HPP
#define OPENMW_SERVER_LUASERVERCONTEXT_HPP

#include <atomic>
#include <array>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <components/openmw-mp/Base/BaseObject.hpp>
#include <components/openmw-mp/Base/BaseActor.hpp>
#include <components/openmw-mp/Base/SurfPhysicsSettings.hpp>
#include <components/lua/configuration.hpp>
#include <components/lua/luastate.hpp>
#include <components/lua/scriptscontainer.hpp>
#include <components/lua/storage.hpp>
#include <components/openmw-mp/Base/BasePlayer.hpp>
#include <components/openmw-mp/Packets/Lua/PacketLuaStorage.hpp>
#include <components/vfs/manager.hpp>

#include "MpEventQueue.hpp"
#include "OutboundQueue.hpp"
#include "PlayerDatabase.hpp"
#include "PlayerMark.hpp"

namespace mwmp
{

class MPServer;

struct LuaPlayerSnapshot
{
    uint32_t guid = 0;
    std::string name;
    std::string cell;
    std::string nickname;
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float rx = 0.f;
    float ry = 0.f;
    float rz = 0.f;
    DynamicStats dynamicStats;
    std::array<Skill, BasePlayer::NUM_SKILLS> skills;
    std::vector<Item> inventory;
};

class LuaServerContext
{
public:
    explicit LuaServerContext(MPServer* server);
    ~LuaServerContext();

    LuaServerContext(const LuaServerContext&) = delete;
    LuaServerContext& operator=(const LuaServerContext&) = delete;

    bool isLoaded() const { return mLoaded; }
    bool isRunning() const;

    void start();
    void stop();
    void drainOutbound();
    void syncSnapshot(double uptime, float worldHour, const std::vector<LuaPlayerSnapshot>& players);

    void onServerInit();
    void onPlayerConnect(uint32_t guid, const std::string& name);
    void onPlayerDisconnect(uint32_t guid, const std::string& name, const std::string& reason);
    void onPlayerCellChange(
        uint32_t guid, const std::string& name, const std::string& newCell, const std::string& oldCell);
    void onPlayerSendMessage(uint32_t guid, const std::string& name, const std::string& message);
    void onDoorState(const std::string& cellId, const std::string& refId, bool isOpen);
    void onWorldWeather(const std::string& region, int current, int next, float transitionFactor);
    void onActorSpawned(const BaseActor& actor, bool persistent);
    void onActorDeath(const BaseActor& actor, bool persistent);
    void onLuaEvent(uint32_t pid, const std::string& eventName, const LuaUtil::BinaryData& data);
    void requestGlobalStorageSnapshot(uint32_t guid);
    std::optional<LuaUtil::BinaryData> evaluateImmediateIntent(
        uint32_t pid, const std::string& eventName, const LuaUtil::BinaryData& data, std::string* error = nullptr);
    std::optional<LuaUtil::BinaryData> callSynchronousInterface(
        const std::string& interfaceName,
        const std::string& identifier,
        const LuaUtil::BinaryData& data,
        int timeoutMs,
        std::string* error = nullptr);

    std::string getString(
        const std::string& tableName, const std::string& key, const std::string& defaultVal = "") const;
    int getInt(const std::string& tableName, const std::string& key, int defaultVal = 0) const;
    bool getBool(const std::string& tableName, const std::string& key, bool defaultVal = false) const;
    std::optional<LuaPlayerSnapshot> getPlayer(uint32_t guid) const;
    std::vector<LuaPlayerSnapshot> getPlayers() const;
    int getPlayerCount() const;
    double getUptime() const;
    float getWorldHour() const;
    std::optional<PlacedObject> getPlacedObject(uint32_t mpNum) const;
    SurfPhysicsSettings getGlobalSurfPhysicsSettings() const;
    SurfPhysicsSettings getCellSurfPhysicsSettings(const std::string& cellId) const;
    SurfPhysicsSettings getPlayerSurfPhysicsSettings(uint32_t guid, const std::string& cellId = "") const;
    SurfPhysicsSettings getEffectiveSurfPhysicsSettings(uint32_t guid, const std::string& cellId) const;
    std::vector<PlayerMark> getPlayerMarks(uint32_t guid) const;
    void setPlayerMarks(uint32_t guid, std::vector<PlayerMark> marks);
    void upsertPlayerMark(uint32_t guid, PlayerMark mark);
    void deletePlayerMark(uint32_t guid, const std::string& name);
    void clearPlayerMarks(uint32_t guid);
    void setGlobalSurfPhysicsSettings(const SurfPhysicsSettings& settings);
    void setCellSurfPhysicsSettings(const SurfPhysicsSettings& settings);
    void clearCellSurfPhysicsSettings(const std::string& cellId);
    void setPlayerSurfPhysicsSettings(uint32_t guid, const SurfPhysicsSettings& settings);
    void clearPlayerSurfPhysicsSettings(uint32_t guid);

    void queueBroadcastServerMessage(const std::string& text);
    void queueBroadcastServerMessageToCell(const std::string& cellId, const std::string& text);
    void queueSendServerMessage(uint32_t guid, const std::string& text);
    void queueRelayPlayerChat(uint32_t guid, const std::string& text);
    void queuePlaceObject(const std::string& refId, int count, const std::string& cellId, const Position& position);
    void queueSpawnActor(
        const std::string& refId, uint32_t refNum, uint32_t mpNum, const std::string& cellId, const Position& position,
        bool persistent = true);
    void queueRemoveActor(uint32_t mpNum, const std::string& cellId);
    void queueRemoveGameObject(uint32_t mpNum, const std::string& cellId);
    void queueTeleportPlayer(uint32_t guid, const std::string& cellId, const Position& position);
    void queueUpsertPlayerMark(uint32_t guid, const PlayerMark& mark);
    void queueDeletePlayerMark(uint32_t guid, const std::string& name);
    void queueKickClient(uint32_t guid, const std::string& reason);
    void queueSetPlayerNickname(uint32_t guid, const std::string& nickname);
    void queueSetWorldHour(float hour);
    void queueBroadcastLuaEvent(const std::string& eventName, const LuaUtil::BinaryData& data);
    void queueBroadcastLuaEventToCell(
        const std::string& cellId, const std::string& eventName, const LuaUtil::BinaryData& data);
    void queueSendLuaEvent(uint32_t guid, const std::string& eventName, const LuaUtil::BinaryData& data);
    void queueBroadcastLuaStorageDelta(
        const std::string& section, const std::string& key, const LuaUtil::BinaryData& value);
    void queueBroadcastLuaStorageSection(const std::string& section, std::vector<LuaStorageEntry> entries);
    bool queueIntentOps(const sol::table& ops, std::string* error = nullptr);
    void queueGrantInventoryItem(uint32_t guid, const std::string& refId, int count);
    void queueRemovePlacedObject(uint32_t mpNum, const std::string& cellId);
    void queueUpsertDynamicRecord(const std::string& recordType, const std::string& recordId,
        const LuaUtil::BinaryData& data, const std::string& recordScope, bool persistent);
    void queueRemoveDynamicRecord(const std::string& recordType, const std::string& recordId);
    void queueSetDynamicRecordDependencies(
        const std::string& recordType, const std::string& recordId, std::vector<std::string> dependencyRecordIds);
    void queueRefreshAllGameSettings();
    void queueRefreshCellGameSettings(const std::string& cellId);
    void queueRefreshPlayerGameSettings(uint32_t guid);
    std::string getGeneratedRecordIdPrefix() const;
    std::string generateDynamicRecordId(const std::string& recordType);
    void syncGeneratedRecordState(
        std::string prefix, const std::unordered_map<std::string, uint64_t>& nextGeneratedNumbers);
    void observeGeneratedRecordId(const std::string& recordType, const std::string& recordId);
    std::vector<DynamicRecordCatalogEntry> getDynamicRecordCatalog() const;
    std::optional<DynamicRecordCatalogEntry> getDynamicRecordInfo(
        const std::string& recordType, const std::string& recordId) const;
    std::vector<DynamicRecordCatalogEntry> queueRemoveUnlinkedGeneratedDynamicRecords(
        const std::optional<std::string>& recordType = std::nullopt,
        const std::optional<bool>& persistent = std::nullopt);
    std::vector<DatabaseTableInfo> listDatabaseTables() const;
    std::optional<DatabaseBrowsePage> browseDatabaseTable(
        const std::string& tableName, int64_t offset = 0, int64_t limit = 100) const;

    void setPlayerData(uint32_t guid, const std::string& key, const std::string& value);
    std::optional<std::string> getPlayerData(uint32_t guid, const std::string& key) const;
    void clearPlayerData(uint32_t guid);
    void syncPlacedObjects(std::vector<PlacedObject> objects);
    void upsertPlacedObject(PlacedObject object);
    void removePlacedObject(uint32_t mpNum);

private:
    struct SnapshotState
    {
        double uptime = 0.0;
        float worldHour = 0.f;
        std::vector<LuaPlayerSnapshot> players;
    };

    struct ImmediateIntentRequest
    {
        uint32_t pid = 0;
        std::string eventName;
        LuaUtil::BinaryData data;
        std::optional<LuaUtil::BinaryData> response;
        std::string error;
        bool completed = false;
        std::mutex mutex;
        std::condition_variable condition;
    };

    struct SyncInterfaceRequest
    {
        std::string interfaceName;
        std::string identifier;
        LuaUtil::BinaryData data;
        std::optional<LuaUtil::BinaryData> response;
        std::string error;
        bool completed = false;
        std::mutex mutex;
        std::condition_variable condition;
    };

    std::filesystem::path resolveScriptsDir() const;
    void buildVfs();
    void initConfiguration();
    void initLua();
    void loadScripts();
    std::filesystem::path globalStoragePath() const;
    void loadGlobalStorage(LuaUtil::LuaView& view);
    void saveGlobalStorage();
    std::size_t dispatchQueuedEvents();
    std::size_t processImmediateIntentRequests();
    std::size_t processSyncInterfaceRequests();
    void processStorageSnapshotRequests();
    std::vector<LuaStorageEntry> makeGlobalStorageSnapshot() const;
    void luaTickLoop();
    std::optional<LuaUtil::BinaryData> evaluateImmediateIntentOnLuaThread(
        uint32_t pid, const std::string& eventName, const LuaUtil::BinaryData& data);
    std::optional<LuaUtil::BinaryData> callInterfaceOnLuaThread(
        const std::string& interfaceName, const std::string& identifier, const LuaUtil::BinaryData& data);

    LuaUtil::BinaryData makeEmptyPayload() const;
    LuaUtil::BinaryData makeTickPayload() const;
    LuaUtil::BinaryData makePlayerPayload(uint32_t guid, const std::string& name) const;
    LuaUtil::BinaryData makeDisconnectPayload(uint32_t guid, const std::string& name, const std::string& reason) const;
    LuaUtil::BinaryData makeCellChangePayload(uint32_t guid, const std::string& name,
        const std::string& newCell, const std::string& oldCell) const;
    LuaUtil::BinaryData makeChatPayload(uint32_t guid, const std::string& name, const std::string& message) const;
    LuaUtil::BinaryData makeDoorPayload(const std::string& cellId, const std::string& refId, bool isOpen) const;
    LuaUtil::BinaryData makeWeatherPayload(
        const std::string& region, int current, int next, float transitionFactor) const;

    void enqueueEvent(uint32_t pid, std::string name, LuaUtil::BinaryData data);

    class StorageSyncListener final : public LuaUtil::LuaStorage::Listener
    {
    public:
        explicit StorageSyncListener(LuaServerContext* context)
            : mContext(context)
        {
        }

        void valueChanged(std::string_view section, std::string_view key, const sol::object& value) const override;
        void sectionReplaced(std::string_view section, const sol::optional<sol::table>& values) const override;

    private:
        LuaServerContext* mContext = nullptr;
    };

    MPServer*                                  mServer = nullptr;
    std::filesystem::path                      mScriptsDir;
    VFS::Manager                               mVfs;
    LuaUtil::ScriptsConfiguration              mConfiguration;
    std::unique_ptr<LuaUtil::LuaState>         mLua;
    std::unique_ptr<LuaUtil::ScriptsContainer> mGlobalScripts;
    LuaUtil::LuaStorage                        mGlobalStorage;
    StorageSyncListener                        mStorageSyncListener { this };
    MpEventQueue                               mInboundQueue;
    OutboundQueue                              mOutboundQueue;
    mutable std::mutex                         mStorageSnapshotMutex;
    std::vector<uint32_t>                      mStorageSnapshotRequests;
    mutable std::mutex                         mImmediateIntentMutex;
    std::vector<std::shared_ptr<ImmediateIntentRequest>> mImmediateIntentRequests;
    mutable std::mutex                         mSyncInterfaceMutex;
    std::vector<std::shared_ptr<SyncInterfaceRequest>> mSyncInterfaceRequests;
    mutable std::mutex                         mSnapshotMutex;
    SnapshotState                              mSnapshot;
    mutable std::mutex                         mPlayerDataMutex;
    std::unordered_map<uint32_t, std::unordered_map<std::string, std::string>> mPlayerScriptData;
    mutable std::mutex                         mPlayerMarksMutex;
    std::unordered_map<uint32_t, std::unordered_map<std::string, PlayerMark>> mPlayerMarks;
    mutable std::mutex                         mPlacedObjectsMutex;
    std::unordered_map<uint32_t, PlacedObject> mPlacedObjectsByMpNum;
    mutable std::mutex                         mGeneratedRecordIdMutex;
    std::string                                mGeneratedRecordIdPrefix = "$custom";
    std::unordered_map<std::string, uint64_t>  mNextGeneratedRecordNumByType;
    mutable std::mutex                         mSurfPhysicsMutex;
    SurfPhysicsSettings                        mGlobalSurfPhysicsSettings;
    std::unordered_map<std::string, SurfPhysicsSettings> mCellSurfPhysicsSettings;
    std::unordered_map<uint32_t, SurfPhysicsSettings> mPlayerSurfPhysicsSettings;
    std::thread                                mLuaThread;
    mutable std::mutex                         mWakeMutex;
    std::condition_variable                    mWakeCondition;
    std::atomic<bool>                          mStopRequested { false };
    std::atomic<bool>                          mThreadRunning { false };
    float                                      mTickIntervalSeconds = 0.05f;
    int                                        mDiagnosticsIntervalSeconds = 5;
    int                                        mImmediateIntentTimeoutMs = 50;
    double                                     mSlowTickLogThresholdMs = 8.0;
    bool                                       mLoaded = false;
};

} // namespace mwmp

#endif // OPENMW_SERVER_LUASERVERCONTEXT_HPP
