#include "Server.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <components/debug/debuglog.hpp>
#include <components/openmw-mp/Packets/BasePacket.hpp>
#include <components/openmw-mp/Packets/System/PacketHandshake.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerBaseInfo.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerPosition.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerCellChange.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerEquipment.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerStatsDynamic.hpp>
#include <components/openmw-mp/Packets/Player/PacketChatMessage.hpp>
#include <components/openmw-mp/Packets/Worldstate/PacketWorldTime.hpp>

namespace mwmp
{

MPServer* MPServer::sInstance = nullptr;

// ---------------------------------------------------------------------------
MPServer::MPServer(uint16_t port) : mPort(port)
{
    if (sInstance)
        throw std::runtime_error("MPServer: only one instance allowed");

    SteamDatagramErrMsg errMsg;
    if (!GameNetworkingSockets_Init(nullptr, errMsg))
        throw std::runtime_error(std::string("GNS init failed: ") + errMsg);

    mInterface = SteamNetworkingSockets();
    if (!mInterface)
        throw std::runtime_error("MPServer: failed to get ISteamNetworkingSockets");

    sInstance = this;
    Log(Debug::Info) << "[Server] Initialised";
}

// ---------------------------------------------------------------------------
MPServer::~MPServer()
{
    shutdown();
    GameNetworkingSockets_Kill();
    sInstance = nullptr;
}

// ---------------------------------------------------------------------------
void MPServer::run()
{
    // Create listen socket
    SteamNetworkingIPAddr listenAddr;
    listenAddr.Clear();
    listenAddr.m_port = mPort;

    SteamNetworkingConfigValue_t opts[2];
    opts[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void*>(&staticConnectionStatusChanged));
    // Allow connections without Steam certificate authentication (no Steam backend in dev)
    opts[1].SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);

    mListenSocket = mInterface->CreateListenSocketIP(listenAddr, 2, opts);
    if (mListenSocket == k_HSteamListenSocket_Invalid)
        throw std::runtime_error("MPServer: CreateListenSocketIP failed");

    mPollGroup = mInterface->CreatePollGroup();
    if (mPollGroup == k_HSteamNetPollGroup_Invalid)
        throw std::runtime_error("MPServer: CreatePollGroup failed");

    Log(Debug::Info) << "[Server] Listening on port " << mPort;

    mRunning = true;
    using Clock = std::chrono::steady_clock;
    auto last   = Clock::now();

    while (mRunning)
    {
        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        mInterface->RunCallbacks();
        tick(dt);
        processIncomingMessages();

        // 20 Hz server tick
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    shutdown();
}

// ---------------------------------------------------------------------------
void MPServer::shutdown()
{
    if (mListenSocket != k_HSteamListenSocket_Invalid)
    {
        // Close all client connections gracefully
        for (auto& [conn, client] : mClients)
            mInterface->CloseConnection(conn, 0, "Server shutdown", true);
        mClients.clear();

        mInterface->CloseListenSocket(mListenSocket);
        mListenSocket = k_HSteamListenSocket_Invalid;
    }
    if (mPollGroup != k_HSteamNetPollGroup_Invalid)
    {
        mInterface->DestroyPollGroup(mPollGroup);
        mPollGroup = k_HSteamNetPollGroup_Invalid;
    }
    Log(Debug::Info) << "[Server] Shutdown complete";
}

// ---------------------------------------------------------------------------
void MPServer::tick(float dt)
{
    // Advance world time — carry over into day/month/year when hour wraps.
    // 30-day months, 12-month year (Morrowind calendar approximation).
    mWorld.gameHour += (dt * mWorld.timeScale) / 3600.f;
    while (mWorld.gameHour >= 24.f)
    {
        mWorld.gameHour -= 24.f;
        if (++mWorld.day > 30)
        {
            mWorld.day = 1;
            if (++mWorld.month > 11)
            {
                mWorld.month = 0;
                ++mWorld.year;
            }
        }
    }

    // Periodic world-time broadcast so connected clients stay in sync.
    mWorld.timeSyncTimer += dt;
    if (mWorld.timeSyncTimer >= WorldState::TIME_SYNC_RATE)
    {
        mWorld.timeSyncTimer = 0.f;
        if (!mClients.empty())
            broadcastToAll(buildWorldTimePacket());
    }
}

// ---------------------------------------------------------------------------
void MPServer::processIncomingMessages()
{
    static constexpr int MAX_MSGS = 512;
    ISteamNetworkingMessage* msgs[MAX_MSGS];

    int n = mInterface->ReceiveMessagesOnPollGroup(mPollGroup, msgs, MAX_MSGS);
    for (int i = 0; i < n; ++i)
    {
        auto* msg = msgs[i];
        auto  it  = mClients.find(msg->m_conn);
        if (it != mClients.end())
            onClientMessage(it->second,
                            static_cast<const uint8_t*>(msg->m_pData),
                            static_cast<size_t>(msg->m_cbSize));
        msg->Release();
    }
}

// ---------------------------------------------------------------------------
void MPServer::onClientConnected(HSteamNetConnection conn)
{
    if ((int)mClients.size() >= MAX_PLAYERS)
    {
        mInterface->CloseConnection(conn, 0, "Server full", false);
        return;
    }

    ConnectedClient client;
    client.conn = conn;
    client.guid = mNextGuid++;
    mClients.emplace(conn, client);

    mInterface->SetConnectionPollGroup(conn, mPollGroup);
    Log(Debug::Info) << "[Server] Client connected, conn=" << conn;
}

// ---------------------------------------------------------------------------
void MPServer::onClientDisconnected(HSteamNetConnection conn, const std::string& reason)
{
    auto it = mClients.find(conn);
    if (it == mClients.end()) return;

    const auto& client = it->second;
    Log(Debug::Info) << "[Server] Client disconnected: "
                     << client.name << " (" << reason << ")";

    if (client.handshakeComplete)
    {
        // Notify all others
        PacketDisconnect pkt;
        pkt.guid   = client.guid;
        pkt.reason = reason;
        broadcastToAll(pkt.encode(), conn);
    }

    mInterface->CloseConnection(conn, 0, nullptr, false);
    mClients.erase(it);
}

// ---------------------------------------------------------------------------
void MPServer::onClientMessage(ConnectedClient& client,
                               const uint8_t* data, size_t size)
{
    PacketHeader hdr;
    if (!BasePacket::peekHeader(data, size, hdr))
        return;

    auto type = static_cast<PacketType>(hdr.type);

    // Must complete handshake before any other packet is processed
    if (!client.handshakeComplete && type != PacketType::Handshake)
    {
        Log(Debug::Warning) << "[Server] Pre-handshake packet from conn="
                            << client.conn << ", ignoring";
        return;
    }

    switch (type)
    {
        case PacketType::Handshake:        handleHandshake(client, data, size);          break;
        case PacketType::PlayerBaseInfo:   handlePlayerBaseInfo(client, data, size);     break;
        case PacketType::PlayerPosition:   handlePlayerPosition(client, data, size);     break;
        case PacketType::PlayerCellChange: handlePlayerCellChange(client, data, size);   break;
        case PacketType::PlayerEquipment:  handlePlayerEquipment(client, data, size);    break;
        case PacketType::PlayerStatsDynamic: handlePlayerStatsDynamic(client, data, size); break;
        case PacketType::ChatMessage:      handleChatMessage(client, data, size);        break;
        default:
            Log(Debug::Verbose) << "[Server] Unhandled packet type " << hdr.type;
            break;
    }
}

// ---------------------------------------------------------------------------
std::vector<uint8_t> MPServer::buildWorldTimePacket() const
{
    PacketWorldTime pkt;
    pkt.time.hour      = mWorld.gameHour;
    pkt.time.day       = mWorld.day;
    pkt.time.month     = mWorld.month;
    pkt.time.year      = mWorld.year;
    pkt.time.gameHour  = mWorld.gameHour;
    pkt.timeScale      = mWorld.timeScale;
    return pkt.encode();
}

// ---------------------------------------------------------------------------
void MPServer::handleHandshake(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketHandshake hs;
    if (!hs.decode(data, size))
    {
        mInterface->CloseConnection(c.conn, 0, "Bad handshake", false);
        return;
    }

    // Version check
    if (hs.clientVersion != SERVER_VERSION)
    {
        PacketHandshakeResponse rsp;
        rsp.accepted      = false;
        rsp.serverVersion = SERVER_VERSION;
        rsp.rejectReason  = "Version mismatch: server=" + std::string(SERVER_VERSION)
                          + " client=" + hs.clientVersion;
        sendTo(c.conn, rsp.encode());
        mInterface->CloseConnection(c.conn, 0, "Version mismatch", false);
        return;
    }

    // Accept
    c.name                    = hs.playerName;
    c.player.guid             = c.guid;
    c.player.name             = hs.playerName;
    c.handshakeComplete       = true;

    PacketHandshakeResponse rsp;
    rsp.accepted       = true;
    rsp.assignedGuid   = c.guid;
    rsp.serverVersion  = SERVER_VERSION;
    sendTo(c.conn, rsp.encode());

    Log(Debug::Info) << "[Server] Handshake accepted: " << c.name
                     << " guid=" << c.guid;

    // Send all existing clients' current state to the new joiner so they
    // see players who connected before them (late-join catch-up).
    for (auto& [existingConn, existingClient] : mClients)
    {
        if (existingConn == c.conn || !existingClient.handshakeComplete)
            continue;

        PacketPlayerBaseInfo baseInfo;
        baseInfo.setPlayer(&existingClient.player);
        sendTo(c.conn, baseInfo.encode());

        PacketPlayerCellChange cellChange;
        cellChange.setPlayer(&existingClient.player);
        sendTo(c.conn, cellChange.encode());

        PacketPlayerEquipment equipment;
        equipment.setPlayer(&existingClient.player);
        sendTo(c.conn, equipment.encode());

        // Send last known position so the new joiner can spawn the NPC
        // in the right place instead of at the world origin.
        if (existingClient.player.position.pos[0] != 0.f
            || existingClient.player.position.pos[1] != 0.f
            || existingClient.player.position.pos[2] != 0.f)
        {
            PacketPlayerPosition position;
            position.setPlayer(&existingClient.player);
            sendTo(c.conn, position.encode());
        }

        Log(Debug::Info) << "[Server] Sent catch-up state for "
                         << existingClient.name << " to " << c.name;
    }

    // Send authoritative world time so the new client's clock snaps to the
    // server immediately rather than waiting for the next 60-second broadcast.
    sendTo(c.conn, buildWorldTimePacket());
    Log(Debug::Info) << "[Server] Sent world time to " << c.name
                     << " (hour=" << mWorld.gameHour
                     << " day=" << mWorld.day
                     << " month=" << mWorld.month
                     << " year=" << mWorld.year << ")";
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerBaseInfo(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketPlayerBaseInfo pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;

    // Broadcast to everyone else so they can render this player
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerPosition(ConnectedClient& c, const uint8_t* data, size_t size)
{
    BasePlayer proposed = c.player;
    PacketPlayerPosition pkt;
    pkt.setPlayer(&proposed);
    if (!pkt.decode(data, size)) return;

    if (!validateMovement(c, proposed))
    {
        // Send correction back
        PacketPlayerPosition correction;
        correction.setPlayer(&c.player);
        sendTo(c.conn, correction.encode());
        return;
    }

    c.player.position = proposed.position;
    c.player.velocity = proposed.velocity;

    // Relay to all other clients (unreliable is fine — we use raw broadcast)
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn, /*reliable=*/false);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerCellChange(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketPlayerCellChange pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;

    Log(Debug::Info) << "[Server] " << c.name << " → cell: "
                     << c.player.cell.cellName;
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerEquipment(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketPlayerEquipment pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handlePlayerStatsDynamic(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketPlayerStatsDynamic pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;
    broadcastToAll(std::vector<uint8_t>(data, data + size), c.conn);
}

// ---------------------------------------------------------------------------
void MPServer::handleChatMessage(ConnectedClient& c, const uint8_t* data, size_t size)
{
    PacketChatMessage pkt;
    pkt.setPlayer(&c.player);
    if (!pkt.decode(data, size)) return;

    Log(Debug::Info) << "[Server] Chat [" << c.name << "] "
                     << "(server time " << mWorld.gameHour << "h "
                     << "day=" << mWorld.day << " mo=" << mWorld.month
                     << " yr=" << mWorld.year << "): "
                     << pkt.message;
    // Relay to everyone including sender (so they see their own message)
    broadcastToAll(std::vector<uint8_t>(data, data + size));
}

// ---------------------------------------------------------------------------
bool MPServer::validateMovement(const ConnectedClient& /*c*/,
                                const BasePlayer& /*proposed*/) const
{
    // TODO: re-enable anti-cheat once position sync is stable.
    // The per-tick distance check was causing false positives during
    // load transitions and initial position sync.
    return true;
}

// ---------------------------------------------------------------------------
void MPServer::broadcastToAll(const std::vector<uint8_t>& data,
                              HSteamNetConnection except, bool reliable)
{
    int flags = reliable ? k_nSteamNetworkingSend_Reliable
                         : k_nSteamNetworkingSend_UnreliableNoDelay;
    for (auto& [conn, client] : mClients)
    {
        if (conn == except || !client.handshakeComplete) continue;
        mInterface->SendMessageToConnection(
            conn, data.data(), static_cast<uint32_t>(data.size()), flags, nullptr);
    }
}

void MPServer::sendTo(HSteamNetConnection conn,
                      const std::vector<uint8_t>& data, bool reliable)
{
    int flags = reliable ? k_nSteamNetworkingSend_Reliable
                         : k_nSteamNetworkingSend_UnreliableNoDelay;
    mInterface->SendMessageToConnection(
        conn, data.data(), static_cast<uint32_t>(data.size()), flags, nullptr);
}

// ---------------------------------------------------------------------------
void MPServer::staticConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
{
    if (sInstance) sInstance->onConnectionStatusChanged(info);
}

void MPServer::onConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
{
    switch (info->m_info.m_eState)
    {
        case k_ESteamNetworkingConnectionState_Connecting:
            // Accept all incoming connections
            if (mInterface->AcceptConnection(info->m_hConn) != k_EResultOK)
            {
                mInterface->CloseConnection(info->m_hConn, 0, "Accept failed", false);
                return;
            }
            onClientConnected(info->m_hConn);
            break;

        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
            onClientDisconnected(info->m_hConn, info->m_info.m_szEndDebug);
            break;

        default:
            break;
    }
}

} // namespace mwmp
