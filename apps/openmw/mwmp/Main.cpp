#include "Main.hpp"

#include <stdexcept>

#include <components/debug/debuglog.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerPosition.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerCellChange.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerStatsDynamic.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerBaseInfo.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerEquipment.hpp>
#include <components/openmw-mp/Packets/Player/PacketChatMessage.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerDeath.hpp>
#include <components/openmw-mp/Packets/System/PacketHandshake.hpp>
#include <components/openmw-mp/Packets/Worldstate/PacketWorldTime.hpp>
#include <components/openmw-mp/Packets/Object/PacketDoorState.hpp>

#include "network/Client.hpp"
#include "network/Protocol.hpp"
#include "sync/PlayerSync.hpp"
#include "sync/RemotePlayer.hpp"
#include "sync/ActorSync.hpp"
#include "sync/CellSync.hpp"
#include "sync/ObjectSync.hpp"
#include "sync/WorldStateSync.hpp"
#include "gui/ChatWindow.hpp"

namespace mwmp
{

Main* Main::sInstance = nullptr;

// ---------------------------------------------------------------------------
Main& Main::get()
{
    if (!sInstance)
        throw std::runtime_error("mwmp::Main not initialised");
    return *sInstance;
}

bool Main::isInitialised()
{
    return sInstance != nullptr;
}

// ---------------------------------------------------------------------------
bool Main::init(const std::string& host, uint16_t port,
                const std::string& playerName,
                const std::string& passwordHash)
{
    if (sInstance)
    {
        Log(Debug::Warning) << "[MP] Main::init called while already initialised";
        return true;
    }

    try
    {
        sInstance = new Main();
        sInstance->mPlayerName = playerName;
        sInstance->mPlayerSync->localPlayer().name = playerName;

        // Attempt connection
        sInstance->mClient->setStateChangeCallback(
            [](ConnectionState /*old*/, ConnectionState newState)
            {
                if (newState == ConnectionState::Connected)
                    Main::get().onConnected();
                else if (newState == ConnectionState::Disconnected)
                    Main::get().onDisconnected();
            });

        sInstance->mClient->setMessageCallback(
            [](const uint8_t* data, size_t size)
            {
                Main::get().getProtocol().dispatch(data, size);
            });

        sInstance->registerProtocolHandlers();

        if (!sInstance->mClient->connect(host, port))
        {
            Log(Debug::Error) << "[MP] Failed to initiate connection to "
                              << host << ":" << port;
            delete sInstance;
            sInstance = nullptr;
            return false;
        }

        Log(Debug::Info) << "[MP] Main initialised, connecting to "
                         << host << ":" << port;
        return true;
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "[MP] Main::init exception: " << e.what();
        delete sInstance;
        sInstance = nullptr;
        return false;
    }
}

// ---------------------------------------------------------------------------
void Main::destroy()
{
    delete sInstance;
    sInstance = nullptr;
    Log(Debug::Info) << "[MP] Main destroyed";
}

// ---------------------------------------------------------------------------
Main::Main()
{
    mClient        = std::make_unique<NetworkClient>();
    mProtocol      = std::make_unique<Protocol>();
    mPlayerSync    = std::make_unique<PlayerSync>(*mClient, *mProtocol);
    mPlayerList    = std::make_unique<PlayerList>();
    mActorSync     = std::make_unique<ActorSync>(*mClient);
    mCellSync      = std::make_unique<CellSync>(*mClient);
    mObjectSync    = std::make_unique<ObjectSync>(*mClient);
    mWorldStateSync= std::make_unique<WorldStateSync>(*mClient);
    mChatWindow = std::make_unique<ChatWindow>(*mClient);
}

Main::~Main()
{
    if (mClient && mClient->isConnected())
        mClient->disconnect("Client shutdown");
}

// ---------------------------------------------------------------------------
void Main::frame(float dt)
{
    if (!mClient) return;

    mClient->update();

    if (!mClient->isConnected()) return;

    mChatWindow->update(dt);
        mPlayerSync->update(dt);
    mPlayerList->updateAll(dt);
    mActorSync->update(dt);
    mObjectSync->update(dt);
    mWorldStateSync->update(dt);
}

// ---------------------------------------------------------------------------
void Main::onConnected()
{
    Log(Debug::Info) << "[MP] Connected — sending handshake";

    // Build and send handshake
    PacketHandshake hs;
    hs.clientVersion = "0.1.0";
    hs.playerName    = mPlayerName;
    // passwordHash set by caller via init() — stored in BasePlayer on response
    mClient->sendReliable(hs.encode());
}

// ---------------------------------------------------------------------------
void Main::onDisconnected()
{
    Log(Debug::Warning) << "[MP] Disconnected from server";
    mWorldReady = false;
    // Phase 3: show reconnect dialog
}

// ---------------------------------------------------------------------------
void Main::registerProtocolHandlers()
{
    auto& proto = *mProtocol;

    // --- Handshake response ---
    proto.registerHandler(PacketType::HandshakeResponse,
        [this](const uint8_t* data, size_t size)
        {
            PacketHandshakeResponse rsp;
            // HandshakeResponse has no player pointer; decode directly
            if (!rsp.decode(data, size)) return;

            if (!rsp.accepted)
            {
                Log(Debug::Error) << "[MP] Server rejected handshake: "
                                  << rsp.rejectReason;
                mClient->disconnect("Rejected: " + rsp.rejectReason);
                return;
            }

            mPlayerSync->localPlayer().guid = rsp.assignedGuid;
            Log(Debug::Info) << "[MP] Handshake accepted, guid="
                             << rsp.assignedGuid
                             << " server=" << rsp.serverVersion;

            // Now send our base info so other players can render us
            mWorldReady = true;
            mPlayerSync->forceFullSync();
        });

    // --- Remote player position ---
    proto.registerHandler(PacketType::PlayerPosition,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerPosition pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid) return; // own echo
            if (tmp.guid == 0) return;

            auto* rp = mPlayerList->getPlayer(tmp.guid);
            if (rp) rp->onPositionUpdate(tmp);
        });

    // --- Remote player cell change ---
    proto.registerHandler(PacketType::PlayerCellChange,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerCellChange pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid) return;

            auto* rp = mPlayerList->getPlayer(tmp.guid);
            if (rp) rp->onCellChange(tmp);
        });

    // --- Remote player base info (join / appearance) ---
    proto.registerHandler(PacketType::PlayerBaseInfo,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerBaseInfo pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid) return;

            if (tmp.guid == 0)
            {
                Log(Debug::Warning) << "[MP] BaseInfo received with guid=0 for player '"
                                    << tmp.name << "' — ignoring (stale pre-handshake packet)";
                return;
            }

            if (!mPlayerList->getPlayer(tmp.guid))
            {
                mPlayerList->addPlayer(tmp.guid, tmp.name);
                Log(Debug::Info) << "[MP] Player joined: " << tmp.name
                                 << " (guid=" << tmp.guid << ")";
            }
        });

    // --- Remote player equipment ---
    proto.registerHandler(PacketType::PlayerEquipment,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerEquipment pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid) return;

            auto* rp = mPlayerList->getPlayer(tmp.guid);
            if (rp) rp->onEquipmentUpdate(tmp);
        });

    // --- Remote player dynamic stats ---
    proto.registerHandler(PacketType::PlayerStatsDynamic,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerStatsDynamic pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid) return;

            auto* rp = mPlayerList->getPlayer(tmp.guid);
            if (rp) rp->onStatsDynamicUpdate(tmp);
        });

    // --- Disconnect (player left) ---
    proto.registerHandler(PacketType::Disconnect,
        [this](const uint8_t* data, size_t size)
        {
            PacketDisconnect pkt;
            if (!pkt.decode(data, size)) return;
            if (mPlayerList->getPlayer(pkt.guid))
            {
                Log(Debug::Info) << "[MP] Player disconnected guid=" << pkt.guid;
                mPlayerList->removePlayer(pkt.guid);
            }
        });

    // --- Chat message ---
    proto.registerHandler(PacketType::ChatMessage,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketChatMessage pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            // Show message from any player including own echo from server
            mChatWindow->addMessage(tmp.name, pkt.message);
        });

        // --- World time ---
    proto.registerHandler(PacketType::WorldTime,
        [this](const uint8_t* data, size_t size)
        {
            PacketWorldTime pkt;
            if (!pkt.decode(data, size)) return;
            mWorldStateSync->onServerTimeUpdate(pkt.time, pkt.timeScale);
        });

    // --- World weather ---
    proto.registerHandler(PacketType::WorldWeather,
        [this](const uint8_t* data, size_t size)
        {
            PacketWorldWeather pkt;
            if (!pkt.decode(data, size)) return;
            mWorldStateSync->onServerWeatherUpdate(
                pkt.currentWeather, pkt.nextWeather, pkt.transitionFactor);
        });

    // --- Door state ---
    proto.registerHandler(PacketType::DoorState,
        [this](const uint8_t* data, size_t size)
        {
            PacketDoorState pkt;
            if (!pkt.decode(data, size)) return;
            // Ignore echo of our own packets (server rebroadcasts to all)
            if (pkt.authorGuid == mPlayerSync->localPlayer().guid) return;
            for (const auto& d : pkt.doors)
                mObjectSync->onServerDoorState(d.cellId, d.refId, d.refNum, d.isOpen);
        });

    Log(Debug::Info) << "[MP] Protocol handlers registered";
}

} // namespace mwmp
