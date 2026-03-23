#include "Client.hpp"
#include <steam/steamnetworkingsockets.h>

#include <iostream>
#include <stdexcept>

#include <components/debug/debuglog.hpp>

namespace mwmp
{
    // Static instance pointer for the GNS callback
    NetworkClient* NetworkClient::sInstance = nullptr;

    // -----------------------------------------------------------------------
    NetworkClient::NetworkClient()
    {
        if (sInstance)
            throw std::runtime_error("NetworkClient: only one instance allowed");

        SteamDatagramErrMsg errMsg;
        if (!GameNetworkingSockets_Init(nullptr, errMsg))
            throw std::runtime_error(std::string("GameNetworkingSockets_Init failed: ") + errMsg);

        mInterface = SteamNetworkingSockets();
        if (!mInterface)
            throw std::runtime_error("NetworkClient: failed to get ISteamNetworkingSockets");

        sInstance = this;
        Log(Debug::Info) << "[MP] NetworkClient initialised";
    }

    // -----------------------------------------------------------------------
    NetworkClient::~NetworkClient()
    {
        disconnect("Client shutting down");
        GameNetworkingSockets_Kill();
        sInstance = nullptr;
    }

    // -----------------------------------------------------------------------
    bool NetworkClient::connect(const std::string& address, uint16_t port)
    {
        if (mState != ConnectionState::Disconnected)
        {
            Log(Debug::Warning) << "[MP] connect() called while not disconnected";
            return false;
        }

        SteamNetworkingIPAddr addr;
        addr.Clear();
        if (!addr.ParseString(address.c_str()))
        {
            Log(Debug::Error) << "[MP] Failed to parse address: " << address;
            return false;
        }
        addr.m_port = port;

        // Set up connection options
        SteamNetworkingConfigValue_t opts[2];

        // Register our status-changed callback
        opts[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                       reinterpret_cast<void*>(&staticConnectionStatusChanged));

        // Allow connections without Steam certificate authentication (no Steam backend in dev)
        opts[1].SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);

        mConnection = mInterface->ConnectByIPAddress(addr, 2, opts);
        if (mConnection == k_HSteamNetConnection_Invalid)
        {
            Log(Debug::Error) << "[MP] ConnectByIPAddress failed";
            return false;
        }

        setState(ConnectionState::Connecting);
        Log(Debug::Info) << "[MP] Connecting to " << address << ":" << port;
        return true;
    }

    // -----------------------------------------------------------------------
    void NetworkClient::disconnect(const std::string& reason)
    {
        if (mConnection != k_HSteamNetConnection_Invalid)
        {
            setState(ConnectionState::Disconnecting);
            mInterface->CloseConnection(mConnection, 0, reason.c_str(), true);
            mConnection = k_HSteamNetConnection_Invalid;
        }
        setState(ConnectionState::Disconnected);
    }

    // -----------------------------------------------------------------------
    void NetworkClient::update()
    {
        if (!mInterface)
            return;

        mInterface->RunCallbacks();

        if (mState == ConnectionState::Connected)
        {
            processIncomingMessages();

            // Update ping stat
            SteamNetConnectionRealTimeStatus_t status;
            if (mInterface->GetConnectionRealTimeStatus(mConnection, &status, 0, nullptr) == k_EResultOK)
                mPingMs = static_cast<float>(status.m_nPing);
        }
    }

    // -----------------------------------------------------------------------
    void NetworkClient::sendReliable(const std::vector<uint8_t>& data)
    {
        Log(Debug::Info) << "[MP] sendReliable called state=" << (int)mState << " size=" << data.size();
        if (mState != ConnectionState::Connected || data.empty())
            return;

        auto result = mInterface->SendMessageToConnection(
            mConnection, data.data(), static_cast<uint32_t>(data.size()),
            k_nSteamNetworkingSend_Reliable, nullptr);

        if (result == k_EResultOK)
        {
            mStats.bytesSent += data.size();
            ++mStats.packetsSent;
        }
        else
            Log(Debug::Warning) << "[MP] sendReliable failed: " << result;
    }

    // -----------------------------------------------------------------------
    void NetworkClient::sendUnreliable(const std::vector<uint8_t>& data)
    {
        if (mState != ConnectionState::Connected || data.empty())
            return;

        auto result = mInterface->SendMessageToConnection(
            mConnection, data.data(), static_cast<uint32_t>(data.size()),
            k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);

        if (result == k_EResultOK)
        {
            mStats.bytesSent += data.size();
            ++mStats.packetsSent;
        }
    }

    // -----------------------------------------------------------------------
    void NetworkClient::processIncomingMessages()
    {
        static constexpr int MAX_MSGS = 256;
        ISteamNetworkingMessage* msgs[MAX_MSGS];

        int n = mInterface->ReceiveMessagesOnConnection(mConnection, msgs, MAX_MSGS);
        for (int i = 0; i < n; ++i)
        {
            auto* msg = msgs[i];
            mStats.bytesReceived += static_cast<uint64_t>(msg->m_cbSize);
            ++mStats.packetsRecv;

            if (mMessageCb)
                mMessageCb(static_cast<const uint8_t*>(msg->m_pData),
                           static_cast<size_t>(msg->m_cbSize));

            msg->Release();
        }
    }

    // -----------------------------------------------------------------------
    void NetworkClient::setState(ConnectionState newState)
    {
        if (newState == mState)
            return;
        ConnectionState old = mState;
        mState = newState;
        if (mStateChangeCb)
            mStateChangeCb(old, newState);
    }

    // -----------------------------------------------------------------------
    // Static GNS callback — dispatches to instance method
    void NetworkClient::staticConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
    {
        if (sInstance)
            sInstance->onConnectionStatusChanged(info);
    }

    // -----------------------------------------------------------------------
    void NetworkClient::onConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
    {
        switch (info->m_info.m_eState)
        {
            case k_ESteamNetworkingConnectionState_Connecting:
                setState(ConnectionState::Connecting);
                break;

            case k_ESteamNetworkingConnectionState_Connected:
                setState(ConnectionState::Connected);
                break;

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            {
                Log(Debug::Warning) << "[MP] Connection closed: "
                                    << info->m_info.m_szEndDebug;
                if (mConnection != k_HSteamNetConnection_Invalid)
                {
                    // Drain the receive queue BEFORE closing our end.
                    // RunCallbacks() fires this callback first, which would
                    // set mState=Disconnected and mConnection=Invalid, causing
                    // update()'s processIncomingMessages() guard to skip the
                    // queue entirely.  Any buffered reliable packets — e.g. a
                    // HandshakeResponse(accepted=false) sent with linger=true
                    // — would then never be delivered to the protocol layer.
                    processIncomingMessages();
                    mInterface->CloseConnection(mConnection, 0, nullptr, false);
                    mConnection = k_HSteamNetConnection_Invalid;
                }
                setState(ConnectionState::Disconnected);
                break;
            }

            default:
                break;
        }
    }

} // namespace mwmp
