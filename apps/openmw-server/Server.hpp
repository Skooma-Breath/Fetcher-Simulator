#ifndef OPENMW_SERVER_SERVER_HPP
#define OPENMW_SERVER_SERVER_HPP

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>
#include <atomic>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <components/openmw-mp/Base/BaseActor.hpp>
#include <components/openmw-mp/Base/BaseObject.hpp>
#include <components/openmw-mp/Base/BasePlayer.hpp>
#include <components/openmw-mp/Base/ActorSyncProtocol.hpp>
#include <components/openmw-mp/Base/DynamicRecord.hpp>
#include <components/openmw-mp/NetworkMessages.hpp>
#include <components/openmw-mp/Packets/Object/PacketDoorState.hpp>

#include "LuaServerContext.hpp"
#include "AdminHttpServer.hpp"
#include "MasterServerClient.hpp"
#include "PlayerDatabase.hpp"
#include <optional>

namespace mwmp
{

// ---------------------------------------------------------------------------
// ConnectedClient — server-side representation of one connected player.
// ---------------------------------------------------------------------------
struct ConnectedClient
{
    HSteamNetConnection conn   = k_HSteamNetConnection_Invalid;
    uint32_t            guid   = 0;
    std::string         loginName; ///< account username — used for auth and duplicate-session checks
    std::string         name;      ///< display name — nickname if set, else character slot name
    std::string         slotName;  ///< permanent character slot name (DB key) — never changes
    std::string         nickname;  ///< cosmetic override; empty = use slotName
    BasePlayer          player;
    bool                handshakeComplete    = false; ///< auth passed, CharacterList sent
    bool                charSelectComplete   = false; ///< player chose a character, in-world
    int64_t             dbAccountId          = 0;   ///< PlayerDatabase accounts.id
    int64_t             dbCharacterId        = 0;   ///< PlayerDatabase characters.id, 0 if not set
    BasePlayer          restoredStatsSnapshot;
    bool                hasRestoredStatsSnapshot = false;
    bool                acceptedPlayerStatsThisSession = false;
    uint64_t            playerStatsRestoreGuardUntilMs = 0;
    std::vector<Item>   restoredInventorySnapshot;
    std::array<EquipmentItem, BasePlayer::NUM_EQUIPMENT_SLOTS> restoredEquipmentSnapshot{};
    bool                hasRestoredInventorySnapshot = false;
    bool                hasRestoredEquipmentSnapshot = false;
    bool                acceptedPlayerInventoryThisSession = false;
    bool                acceptedPlayerEquipmentThisSession = false;
    uint64_t            playerInventoryRestoreGuardUntilMs = 0;
    uint64_t            playerEquipmentRestoreGuardUntilMs = 0;
    uint64_t            lastPlayerInventoryRestoreCorrectionLogMs = 0;
    uint64_t            lastPlayerEquipmentRestoreCorrectionLogMs = 0;

    // Ed25519 challenge-response state (valid between receiving PacketHandshake
    // with a publicKey and receiving PacketChallengeResponse)
    uint8_t             pendingChallenge[32] = {};
    std::string         pendingPublicKey;           ///< base64 public key being challenged

    // Per-player key/value store — set/get from Lua scripts via player:setData/getData().
    std::unordered_map<std::string, std::string> scriptData;

    std::unordered_set<std::string> loadedActorCells; ///< empty falls back to player.cell
    uint32_t loadedActorCellsSequence = 0;
    uint32_t actorSyncProtocolVersion = ActorSyncProtocolVersionV1;
    std::unordered_set<ActorInstanceId> actorV2IdentitySent;
    std::unordered_set<ActorInstanceId> actorV2IdentityAcked;
    std::unordered_map<ActorInstanceId, uint64_t> actorV2LastSentMs;
    std::unordered_map<ActorInstanceId, std::size_t> actorV2MissingIdentityByNetIdWindow;
    uint64_t actorV2DiagnosticsLastLogMs = 0;
    std::size_t actorV2IdentitySentWindow = 0;
    std::size_t actorV2IdentityAckedWindow = 0;
    std::size_t actorV2SnapshotsSentWindow = 0;
    std::size_t actorV2BytesSentWindow = 0;
    std::size_t actorV2PresentationSentWindow = 0;
    std::size_t actorV2PresentationBytesSentWindow = 0;
    std::size_t actorV2AttackSentWindow = 0;
    std::size_t actorV2AttackSuppressedUntilIdentityKnownWindow = 0;
    std::size_t actorV2PresentationSuppressedUntilIdentityKnownWindow = 0;
    std::size_t actorV2DeferredWindow = 0;
    std::size_t actorV2PositionSuppressedUntilIdentityKnownWindow = 0;
    std::size_t actorV2TierCounts[5] = {};
};

// ---------------------------------------------------------------------------
// MPServer — dedicated multiplayer server.
//
// Run lifecycle:
//   MPServer server(25565);
//   server.run();          ← blocks; call requestStop() from signal handler
//   server.shutdown();
// ---------------------------------------------------------------------------
class MPServer
{
public:
    explicit MPServer(uint16_t port);
    ~MPServer();

    MPServer(const MPServer&)            = delete;
    MPServer& operator=(const MPServer&) = delete;

    // Main loop — returns when requestStop() is called
    void run();
    void requestStop() { mRunning = false; }
    void shutdown();

    // ── Script-facing public API ──────────────────────────────────────────

    // Seconds of real time since server start.
    double getUptime() const;

    // Send a chat message from the "Server" sender to all or one player.
    void broadcastServerMessage(const std::string& text);
    void broadcastServerMessageToCell(const std::string& cellId, const std::string& text);
    void sendServerMessage(uint32_t guid, const std::string& text);
    void relayPlayerChat(uint32_t guid, const std::string& text);
    void broadcastLuaEvent(uint32_t pid, const std::string& eventName, const std::string& eventData);
    void broadcastLuaEventToCell(
        const std::string& cellId, uint32_t pid, const std::string& eventName, const std::string& eventData);
    void sendLuaEvent(uint32_t guid, uint32_t pid, const std::string& eventName, const std::string& eventData);
    void broadcastLuaStorage(
        LuaStorageAction action, const std::string& section, const std::vector<LuaStorageEntry>& entries);
    void sendLuaStorage(uint32_t guid, LuaStorageAction action,
        const std::string& section, const std::vector<LuaStorageEntry>& entries);
    void broadcastGameSettingsToAllPlayers();
    void broadcastGameSettingsToCell(const std::string& cellId);
    void sendGameSettingsToPlayer(uint32_t guid);
    bool teleportPlayer(uint32_t guid, const std::string& cellId, const Position& position);
    bool upsertPlayerMark(uint32_t guid, const PlayerMark& mark);
    bool deletePlayerMark(uint32_t guid, std::string_view name);

    // Disconnect a player by guid with a reason string.
    void kickClient(uint32_t guid, const std::string& reason);

    /// Set (or clear with "") the cosmetic nickname for a connected player.
    /// Updates c.name, c.player.name, persists to DB, broadcasts PacketPlayerBaseInfo.
    void setPlayerNickname(uint32_t guid, const std::string& nickname);

    // World time accessors.
    float getWorldHour() const  { return mWorld.gameHour; }
    void  setWorldHour(float h)
    {
        mWorld.gameHour = std::fmod(h, 24.f);
        if (mWorld.gameHour < 0.f) mWorld.gameHour += 24.f;
    }

    // Look up a live client by guid. Returns nullptr if not found / disconnected.
    ConnectedClient* findClientByGuid(uint32_t guid);
    bool grantPlayerInventoryItem(uint32_t guid, const std::string& refId, int count);
    bool placeObject(const std::string& refId, int count, const std::string& cellId, const Position& position);
    bool removePlacedObjectByMpNum(uint32_t mpNum, const std::string& cellId);
    bool spawnActor(
        const std::string& refId, uint32_t refNum, uint32_t mpNum, const std::string& cellId, const Position& position,
        bool persistent = true);
    bool removeActor(uint32_t mpNum, const std::string& cellId);
    bool removeGameObject(uint32_t mpNum, const std::string& cellId);
    bool resetCellStateForTesting(const std::string& cellId);
    bool upsertDynamicRecord(const std::string& recordType, const std::string& recordId, const std::string& data,
        const std::string& recordScope, bool persistent);
    bool removeDynamicRecord(const std::string& recordType, const std::string& recordId);
    bool setDynamicRecordDependencies(
        const std::string& recordType, const std::string& recordId, const std::vector<std::string>& dependencyRecordIds);
    std::vector<DynamicRecordCatalogEntry> listDynamicRecordCatalog();
    std::optional<DynamicRecordCatalogEntry> getDynamicRecordInfo(
        std::string_view recordType, std::string_view recordId);
    std::vector<DynamicRecordCatalogEntry> collectGeneratedDynamicRecordGcCandidates(
        const std::optional<std::string>& recordType = std::nullopt,
        const std::optional<bool>& persistent = std::nullopt);
    std::vector<DatabaseTableInfo> listBrowsableTables();
    std::optional<DatabaseBrowsePage> browseDatabaseTable(
        std::string_view tableName, int64_t offset = 0, int64_t limit = 100);

    // Iterate all fully-connected (post-handshake) players.
    // Used by script bindings to build the mp.getPlayers() table.
    template<typename Fn>
    void forEachPlayer(Fn&& fn)
    {
        for (auto& [conn, client] : mClients)
            if (client.handshakeComplete)
                fn(client);
    }

    // Number of fully-connected players.
    int getPlayerCount() const;

    // ── Master server configuration (call before run()) ───────────────────
    void setServerName       (const std::string& n) { mServerName        = n; }
    void setMasterUrl        (const std::string& u) { mMasterUrl         = u; }
    void setGameMode         (const std::string& m) { mGameMode          = m; }
    void setPasswordProtected(bool v)               { mPasswordProtected = v; }
    void setDbPath           (const std::string& p) { mDbPath            = p; }
    void setSpawnCell        (const std::string& c) { mDefaultSpawnCell  = c; }
    void setMaxPlayers       (int n)                { mMaxPlayersConfig   = n; }
    void setMaxCharsPerAccount(int n)               { mMaxCharsPerAccount = n; }

private:
    // ── GNS callbacks ─────────────────────────────────────────────────────
    static MPServer* sInstance;
    static void staticConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);
    void onConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);

    // ── Per-frame update ──────────────────────────────────────────────────
    void tick(float dt);
    void processIncomingMessages();

    // ── Client management ─────────────────────────────────────────────────
    void onClientConnected   (HSteamNetConnection conn);
    void onClientDisconnected(HSteamNetConnection conn, const std::string& reason);

    // ── Packet dispatch ───────────────────────────────────────────────────
    void onClientMessage(ConnectedClient& client, const uint8_t* data, size_t size);

    // ── Packet handlers ───────────────────────────────────────────────────
    void handleHandshake          (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleCharacterSelect    (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleChallengeResponse  (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleLinkKeyRequest     (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleUnlinkKeyRequest   (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleDeleteCharRequest  (ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerCharGen    (ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerBaseInfo   (ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerPosition   (ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerCellChange (ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerLoadedCells(ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerEquipment  (ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerAnimFlags  (ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerAnimPlay   (ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerAttack     (ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerCast       (ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerInventory  (ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerStatsDynamic(ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerDeath      (ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerResurrect  (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleChatMessage      (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleLuaEvent         (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleObjectPlace      (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleObjectDelete     (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleObjectMove       (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleContainer        (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleDoorState        (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleWeather          (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorList        (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorPosition    (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorPositionV2  (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorPresentationV2(ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorIdentityAck (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorAnimFlags   (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorAnimPlay    (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorAttack      (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorAttackV2    (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorCast        (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorCellChange  (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorDeath       (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorEquipment   (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorStatsDynamic(ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorAI          (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleActorCombatRequest(ConnectedClient& c, const uint8_t* data, size_t size);


    // ── Broadcast helpers ─────────────────────────────────────────────────
    void broadcastToAll(const std::vector<uint8_t>& data,
                        HSteamNetConnection except = k_HSteamNetConnection_Invalid,
                        bool reliable = true);
    void sendTo        (HSteamNetConnection conn,
                        const std::vector<uint8_t>& data,
                        bool reliable = true);
    void broadcastToCell(const std::string& cellId,
                         const std::vector<uint8_t>& data,
                         HSteamNetConnection except = k_HSteamNetConnection_Invalid,
                         bool reliable = true);
    void broadcastActorToCell(const std::string& cellId,
                              const std::vector<uint8_t>& data,
                              HSteamNetConnection except = k_HSteamNetConnection_Invalid,
                              bool reliable = true);

    // ── Validation ────────────────────────────────────────────────────────
    bool validateMovement(const ConnectedClient& c, const BasePlayer& proposed) const;

    // ── Packet builders ───────────────────────────────────────────────────
    std::vector<uint8_t> buildWorldTimePacket()    const;
    std::vector<uint8_t> buildWorldWeatherPacket() const;
    void syncLuaSnapshot();
    void syncLuaAuthorityState();
    void sendAuthoritativeInventory(ConnectedClient& c);
    void sendAuthoritativeEquipment(ConnectedClient& c, bool includeOthers = true);
    void sendPlayerStateBootstrapToClient(ConnectedClient& receiver);
    void startAdminHttpServer();
    void stopAdminHttpServer();
    AdminHttpServer::Response handleAdminHttpRequest(
        std::string_view action, const std::map<std::string, std::string>& query);
    bool acceptPlacedObject(PlacedObject& object);
    bool grantInventoryItem(ConnectedClient& c, const std::string& refId, int count);
    struct ActorRegistryRecord;
    struct CellActorState;
    bool worldMpNumInUse(uint32_t mpNum) const;
    std::optional<uint32_t> reserveWorldMpNum();
    void advanceWorldMpNumPast(uint32_t mpNum);
    void setNextWorldMpNum(uint64_t nextMpNum);
    bool removePlacedObjectAuthoritative(uint32_t mpNum, const std::string& cellId);
    bool removePlacedObjectAuthoritativeAnyCell(uint32_t mpNum, const std::string& preferredCellId);
    void persistSpawnedActorIfNeeded(ActorRegistryRecord& record, uint64_t now = 0, bool force = true);
    void deletePersistedSpawnedActor(uint32_t mpNum);
    void sendActorLifecycleEvent(const char* eventName, const BaseActor& actor, bool persistent);
    void sendDynamicRecordsToClient(HSteamNetConnection conn);
    std::unordered_map<std::string, uint64_t> buildGeneratedDynamicRecordCounters(const std::string& prefix) const;
    struct DynamicReferenceCleanupStats
    {
        std::size_t placedObjects = 0;
        std::size_t containers = 0;
        std::size_t containerItems = 0;
        std::size_t doorStates = 0;
        std::size_t inventoryItems = 0;
        std::size_t equipmentItems = 0;
        std::size_t characters = 0;
    };
    DynamicReferenceCleanupStats cleanupDynamicReferences(
        const std::function<bool(std::string_view)>& shouldRemoveRefId,
        bool broadcastLive,
        std::string_view reason);
    std::size_t gcGeneratedDynamicRecordsAfterUnlink(std::string_view reason);
    void scheduleGeneratedDynamicRecordGc(
        std::string_view reason, std::chrono::milliseconds delay = std::chrono::milliseconds(250));
    void flushScheduledGeneratedDynamicRecordGc();
    void refreshActorAuthorityForCell(const std::string& cellId, uint32_t preferredGuid = 0);
    void sendActorAuthorityToClient(HSteamNetConnection conn, const std::string& cellId);
    void sendActorStateToClient(HSteamNetConnection conn, const std::string& cellId);
    void sendActorStateToInterestedClients(const std::string& cellId);
    void sendActorIdentityToClient(HSteamNetConnection conn, const std::string& cellId, CellActorState& cellState);
    void broadcastActorIdentityForCell(const std::string& cellId, CellActorState& cellState,
        HSteamNetConnection except = k_HSteamNetConnection_Invalid);
    void broadcastActorPositionV2ToCell(
        const std::string& cellId, CellActorState& cellState, const ActorList& actorList,
        HSteamNetConnection except = k_HSteamNetConnection_Invalid);
    void broadcastActorPresentationV2ToCell(
        const std::string& cellId, CellActorState& cellState, const ActorList& actorList,
        HSteamNetConnection except = k_HSteamNetConnection_Invalid);
    void sendGameSettingsToClient(HSteamNetConnection conn, const std::string& cellId);
    bool validateActorUpdate(const ConnectedClient& c, const ActorList& actorList, const char* packetName);
    bool clientHasActorCellLoaded(const ConnectedClient& client, const std::string& cellId) const;
    std::unordered_set<std::string> actorInterestCellsForClient(const ConnectedClient& client) const;

    struct ActorRegistryRecord
    {
        BaseActor actor;
        uint64_t lastSnapshotTime = 0;
        // Set once by spawnActor(); never updated by client reports.
        // Used to protect freshly Lua-spawned actors from being evicted by
        // the authority's first ActorList, which can arrive before the client
        // has processed the spawn notification (timing race).
        uint64_t serverSpawnTime = 0;
        bool persistent = false;
        ActorInstanceId actorNetId = 0;
        uint32_t lastDeathEventId = 0;
        uint64_t lastPersistTime = 0;
        bool pendingPersist = false;
    };

    struct CellActorState
    {
        uint32_t authorityGuid = 0;
        uint32_t authorityGeneration = 0;
        uint32_t nextSnapshotSequence = 1;
        std::unordered_map<std::string, ActorRegistryRecord> actors;
        std::unordered_set<std::string> resetSuppressedVanillaDeaths;
        std::unordered_map<std::string, uint64_t> staleLiveVanillaDeathResendMs;
    };

    // ── State ─────────────────────────────────────────────────────────────
    ActorRegistryRecord* findTrackedActor(CellActorState& cellState,
        const BaseActor& actor,
        const ConnectedClient& sender,
        const char* packetName);
    std::optional<ActorRegistryRecord> removeActorFromOtherCells(
        const BaseActor& actor,
        const std::string& destinationCellId,
        std::unordered_set<std::string>& changedCellIds);
    void broadcastActorListForCell(const std::string& cellId, CellActorState& cellState);
    ActorInstanceId assignActorNetId(const BaseActor& actor);
    ActorInstanceId ensureActorNetId(ActorRegistryRecord& record, const std::string& cellId);
    void forgetActorNetId(ActorInstanceId actorNetId, const BaseActor& actor);
    ActorIdentityList buildActorIdentityList(
        const std::string& cellId,
        CellActorState& cellState,
        std::unordered_map<std::string, ActorRegistryRecord>& actors);
    void broadcastActorIdentityRemovalForCell(
        const std::string& cellId,
        CellActorState& cellState,
        const std::vector<ActorRegistryRecord>& records);
    void upsertSpawnedActorDynamicRecordLinkIfNeeded(const BaseActor& actor);
    void rememberActorLocation(const BaseActor& actor, const std::string& cellId);
    void forgetActorLocation(const BaseActor& actor, const std::string& cellId = {});
    void rememberDeadVanillaActor(const ActorRegistryRecord& record);
    void forgetDeadVanillaActor(const BaseActor& actor, const std::string& cellId = {});
    const ActorRegistryRecord* findDeadVanillaActor(
        const BaseActor& actor, std::string* cellId = nullptr) const;
    bool rejectStaleAliveVanillaActor(
        const BaseActor& actor,
        const std::string& incomingCellId,
        const ConnectedClient& sender,
        const char* packetName) const;
    bool rejectResetStaleDeadVanillaActor(
        const BaseActor& actor,
        const std::string& incomingCellId,
        const ConnectedClient& sender,
        const char* packetName) const;
    void clearResetStaleDeathSuppressionForAliveVanillaActor(
        const BaseActor& actor,
        const std::string& incomingCellId);
    std::size_t mergeDeadVanillaActorsForCell(
        const std::string& cellId,
        std::unordered_map<std::string, ActorRegistryRecord>& actors) const;

    ISteamNetworkingSockets* mInterface    = nullptr;
    HSteamListenSocket       mListenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup       mPollGroup    = k_HSteamNetPollGroup_Invalid;

    std::unordered_map<HSteamNetConnection, ConnectedClient> mClients;
    uint32_t    mNextGuid  = 1;
    uint16_t    mPort;
    std::atomic<bool> mRunning { false };
    std::chrono::steady_clock::time_point mStartTime;
    bool mGeneratedRecordGcScheduled = false;
    std::chrono::steady_clock::time_point mGeneratedRecordGcDueTime {};
    std::string mGeneratedRecordGcReason;
    bool mAdminHttpEnabled = true;
    int mAdminHttpPort = 8081;
    int mAdminHttpTimeoutMs = 250;
    std::string mAdminHttpHost = "127.0.0.1";
    std::unique_ptr<AdminHttpServer> mAdminHttpServer;

    // ── World state ───────────────────────────────────────────────────────
    // Server is the authoritative clock. Day/month/year are tracked so new
    // joiners receive a complete Time packet. The calendar uses simplified
    // 30-day months; full Morrowind calendar accuracy is a later concern.
    struct WorldState
    {
        struct StoredDynamicRecord
        {
            std::string recordType;
            std::string recordId;
            std::string data;
            std::string recordScope = "permanent";
            bool persistent = true;
            uint64_t sequence = 0;
        };

        float gameHour   = 8.f;   // 0..24
        int   day        = 1;
        int   month      = 0;     // 0-based (0=Morning Star)
        int   year       = 427;
        float timeScale  = 30.f;  // game-seconds per real-second
        int   weather    = 0;

        // Periodic time-broadcast timer (seconds of real time)
        float timeSyncTimer = 0.f;
        static constexpr float TIME_SYNC_RATE = 60.f;

        // Authoritative door states: cellId → list of door entries.
        std::map<std::string, std::vector<mwmp::DoorEntry>> doorStates;
        std::map<std::string, std::vector<mwmp::PlacedObject>> placedObjects;
        std::unordered_map<std::string, mwmp::ContainerRecord> containers;
        std::unordered_map<std::string, StoredDynamicRecord> dynamicRecords;
        std::unordered_map<std::string, CellActorState> actorCells;
        std::unordered_map<std::string, std::string> actorLocations;
        std::unordered_map<std::string, ActorInstanceId> actorNetIdsByKey;
        std::unordered_map<ActorInstanceId, std::string> actorKeysByNetId;
        std::unordered_map<std::string, std::unordered_map<std::string, ActorRegistryRecord>> deadVanillaActorCells;
        uint64_t nextObjectMpNum = 1;
        uint64_t nextActorMpNum = 1;
        uint64_t nextDynamicRecordSequence = 1;

        // Weather — reported by the host (guid 1) and relayed to others.
        bool        hasWeather        = false;
        uint32_t    hostGuid          = 0;
        int         weatherCurrent    = 0;
        int         weatherNext       = -1;
        float       weatherTransition = 0.f;
        std::string weatherRegion;
    } mWorld;

    // ── Scripting ─────────────────────────────────────────────────────────
    LuaServerContext mLua { this };

    // ── Master server ──────────────────────────────────────────────────────
    // Populated from server.cfg / command-line before run() is called.
    MasterServerClient mMasterClient;
    std::string        mServerName   = "OpenMW Multiplayer Server";
    std::string        mMasterUrl;   ///< empty → do not register
    std::string        mGameMode     = "Co-op";
    bool               mPasswordProtected = false;

    std::optional<PlayerDatabase> mPlayerDb;
    std::string                   mDbPath            = "playerdata.db";
    std::string                   mDefaultSpawnCell  = "toddtest";
    std::string                   mGeneratedRecordIdPrefix = "$custom";
    int                           mMaxPlayersConfig   = 32;
    int                           mMaxCharsPerAccount = 5; ///< 0 = unlimited; overridden from config.lua

    // ── Config ────────────────────────────────────────────────────────────
    static constexpr float       MAX_MOVE_SPEED = 600.f;
    static constexpr const char* SERVER_VERSION = "0.1.0";

    void loadPersistentWorldState();
    void sendCellStateToClient(HSteamNetConnection conn, const std::string& cellId);
};

} // namespace mwmp

#endif // OPENMW_SERVER_SERVER_HPP
