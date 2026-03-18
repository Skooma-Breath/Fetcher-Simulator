#ifndef OPENMW_SERVER_SERVER_HPP
#define OPENMW_SERVER_SERVER_HPP

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>
#include <atomic>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <components/openmw-mp/Base/BasePlayer.hpp>
#include <components/openmw-mp/NetworkMessages.hpp>
#include <components/openmw-mp/Packets/Object/PacketDoorState.hpp>

namespace mwmp
{

// ---------------------------------------------------------------------------
// ConnectedClient — server-side representation of one connected player.
// ---------------------------------------------------------------------------
struct ConnectedClient
{
    HSteamNetConnection conn   = k_HSteamNetConnection_Invalid;
    uint32_t            guid   = 0;
    std::string         name;
    BasePlayer          player;
    bool                handshakeComplete = false;
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

private:
    // GNS callbacks
    static MPServer* sInstance;
    static void staticConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);
    void onConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);

    // Per-frame update
    void tick(float dt);
    void processIncomingMessages();

    // Client management
    void onClientConnected   (HSteamNetConnection conn);
    void onClientDisconnected(HSteamNetConnection conn, const std::string& reason);

    // Packet dispatch
    void onClientMessage(ConnectedClient& client, const uint8_t* data, size_t size);

    // Packet handlers
    void handleHandshake     (ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerBaseInfo(ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerPosition(ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerCellChange(ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerEquipment(ConnectedClient& c, const uint8_t* data, size_t size);
    void handlePlayerStatsDynamic(ConnectedClient& c, const uint8_t* data, size_t size);
    void handleChatMessage   (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleDoorState     (ConnectedClient& c, const uint8_t* data, size_t size);
    void handleWeather       (ConnectedClient& c, const uint8_t* data, size_t size);

    // Broadcast helpers
    void broadcastToAll      (const std::vector<uint8_t>& data,
                              HSteamNetConnection except = k_HSteamNetConnection_Invalid,
                              bool reliable = true);
    void sendTo              (HSteamNetConnection conn,
                              const std::vector<uint8_t>& data,
                              bool reliable = true);

    // Validation
    bool validateMovement(const ConnectedClient& c, const BasePlayer& proposed) const;

    // State
    ISteamNetworkingSockets* mInterface    = nullptr;
    HSteamListenSocket       mListenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup       mPollGroup    = k_HSteamNetPollGroup_Invalid;

    std::unordered_map<HSteamNetConnection, ConnectedClient> mClients;
    uint32_t    mNextGuid = 1;
    uint16_t    mPort;
    std::atomic<bool> mRunning { false };

    // World state — server is the authoritative clock.
    // Day/month/year are tracked so new joiners receive a complete Time packet.
    // The calendar uses simplified 30-day months; full Morrowind calendar
    // accuracy is a Phase 5 concern.
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
        static constexpr float TIME_SYNC_RATE = 60.f; // broadcast every 60 real-seconds

        // Authoritative door states: cellId → list of door entries.
        // Populated when clients activate doors; sent to new joiners as catch-up.
        std::map<std::string, std::vector<mwmp::DoorEntry>> doorStates;

        // Weather — reported by the host (guid 1) and relayed to others.
        bool        hasWeather      = false;
        uint32_t    hostGuid        = 0;     // guid of first connected player
        int         weatherCurrent  = 0;
        int         weatherNext     = -1;
        float       weatherTransition = 0.f;
        std::string weatherRegion;           // serialised ESM::RefId
    } mWorld;

    std::vector<uint8_t> buildWorldWeatherPacket() const;

    // Build an encoded WorldTime packet from current mWorld state.
    std::vector<uint8_t> buildWorldTimePacket() const;

    // Server config (later: load from cfg file)
    static constexpr int         MAX_PLAYERS    = 32;
    static constexpr float       MAX_MOVE_SPEED = 600.f; // units/sec anti-cheat cap
    static constexpr const char* SERVER_VERSION = "0.1.0";
};

} // namespace mwmp

#endif // OPENMW_SERVER_SERVER_HPP
