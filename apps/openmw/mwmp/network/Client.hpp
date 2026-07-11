#ifndef OPENMW_MWMP_NETWORK_CLIENT_HPP
#define OPENMW_MWMP_NETWORK_CLIENT_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingtypes.h>

namespace mwmp
{
    enum class ConnectionState
    {
        Disconnected,
        Connecting,
        Connected,
        Disconnecting
    };

    // -----------------------------------------------------------------------
    // NetworkClient — wraps GameNetworkingSockets for the multiplayer client.
    //
    // Usage:
    //   1. Construct (initialises GNS library).
    //   2. setMessageCallback / setStateChangeCallback.
    //   3. connect(addr, port).
    //   4. Call update(dt) every frame.
    //   5. sendReliable / sendUnreliable to push encoded packets.
    //   6. Destroy or call disconnect() when done.
    // -----------------------------------------------------------------------
    class NetworkClient
    {
    public:
        NetworkClient();
        ~NetworkClient();

        // Non-copyable
        NetworkClient(const NetworkClient&)            = delete;
        NetworkClient& operator=(const NetworkClient&) = delete;

        // ------------------------------------------------------------------
        // Connection management
        // ------------------------------------------------------------------
        bool connect(const std::string& address, uint16_t port);
        void disconnect(const std::string& reason = "Client disconnect");

        // Call once per frame from the engine's update loop
        void update();

        // ------------------------------------------------------------------
        // Sending
        // ------------------------------------------------------------------
        void sendReliable  (const std::vector<uint8_t>& data);
        void sendUnreliable(const std::vector<uint8_t>& data);

        // ------------------------------------------------------------------
        // State
        // ------------------------------------------------------------------
        ConnectionState getState()    const { return mState; }
        bool            isConnected() const { return mState == ConnectionState::Connected; }
        float           getPing()     const { return mPingMs; }

        // ------------------------------------------------------------------
        // Callbacks
        // ------------------------------------------------------------------
        using MessageCallback     = std::function<void(const uint8_t* data, size_t size)>;
        using StateChangeCallback = std::function<void(ConnectionState oldState, ConnectionState newState)>;

        void setMessageCallback    (MessageCallback     cb) { mMessageCb     = std::move(cb); }
        void setStateChangeCallback(StateChangeCallback cb) { mStateChangeCb = std::move(cb); }

        // ------------------------------------------------------------------
        // Stats (for debug overlay)
        // ------------------------------------------------------------------
        struct Stats
        {
            uint64_t bytesSent     = 0;
            uint64_t bytesReceived = 0;
            uint32_t packetsSent   = 0;
            uint32_t packetsRecv   = 0;
        };
        const Stats& getStats() const { return mStats; }

    private:
        // GNS static callback — must be static, dispatches to instance method
        static NetworkClient* sInstance;
        static void staticConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);

        void onConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info);
        void processIncomingMessages();
        void setState(ConnectionState newState);
        EResult sendOnConfiguredLane(const std::vector<uint8_t>& data, int flags);

        ISteamNetworkingSockets* mInterface    = nullptr;
        HSteamNetConnection      mConnection   = k_HSteamNetConnection_Invalid;
        ConnectionState          mState        = ConnectionState::Disconnected;
        float                    mPingMs       = 0.f;

        MessageCallback     mMessageCb;
        StateChangeCallback mStateChangeCb;
        Stats               mStats;
    };

} // namespace mwmp

#endif // OPENMW_MWMP_NETWORK_CLIENT_HPP
