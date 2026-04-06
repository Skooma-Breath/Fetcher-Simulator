#ifndef OPENMW_SERVER_SERVER_HPP
#define OPENMW_SERVER_SERVER_HPP

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>
#include <atomic>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <components/openmw-mp/Base/BaseObject.hpp>
#include <components/openmw-mp/Base/BasePlayer.hpp>
#include <components/openmw-mp/NetworkMessages.hpp>
#include <components/openmw-mp/Packets/Object/PacketDoorState.hpp>

#include "ScriptEngine.hpp"
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

    // Ed25519 challenge-response state (valid between receiving PacketHandshake
    // with a publicKey and receiving PacketChallengeResponse)
    uint8_t             pendingChallenge[32] = {};
    std::string         pendingPublicKey;           ///< base64 public key being challenged

    // Per-player key/value store — set/get from Lua scripts via player:setData/getData().
    std::unordered_map<std::string, std::string> scriptData;
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
    void sendServerMessage(uint32_t guid, const std::string& text);

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
    void handleObjectPlace      (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleObjectDelete     (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleObjectMove       (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleContainer        (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleDoorState        (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleWeather          (ConnectedClient& c, const uint8_t* data, size_t size);

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

    // ── Validation ────────────────────────────────────────────────────────
    bool validateMovement(const ConnectedClient& c, const BasePlayer& proposed) const;

    // ── Packet builders ───────────────────────────────────────────────────
    std::vector<uint8_t> buildWorldTimePacket()    const;
    std::vector<uint8_t> buildWorldWeatherPacket() const;

    // ── State ─────────────────────────────────────────────────────────────
    ISteamNetworkingSockets* mInterface    = nullptr;
    HSteamListenSocket       mListenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup       mPollGroup    = k_HSteamNetPollGroup_Invalid;

    std::unordered_map<HSteamNetConnection, ConnectedClient> mClients;
    uint32_t    mNextGuid  = 1;
    uint16_t    mPort;
    std::atomic<bool> mRunning { false };
    std::chrono::steady_clock::time_point mStartTime;

    // ── World state ───────────────────────────────────────────────────────
    // Server is the authoritative clock. Day/month/year are tracked so new
    // joiners receive a complete Time packet. The calendar uses simplified
    // 30-day months; full Morrowind calendar accuracy is a later concern.
    struct WorldState
    {
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
        uint32_t nextObjectMpNum = 1;

        // Weather — reported by the host (guid 1) and relayed to others.
        bool        hasWeather        = false;
        uint32_t    hostGuid          = 0;
        int         weatherCurrent    = 0;
        int         weatherNext       = -1;
        float       weatherTransition = 0.f;
        std::string weatherRegion;
    } mWorld;

    // ── Scripting ─────────────────────────────────────────────────────────
    ScriptEngine mScript { this };

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
