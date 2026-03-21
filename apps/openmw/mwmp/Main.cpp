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

#include <components/openmw-mp/Packets/Player/PacketPlayerCharGen.hpp>
#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/player.hpp"
#include "../mwworld/esmstore.hpp"
#include <sstream>
#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>

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
        sInstance->mPasswordHash = passwordHash;
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

    // ── Chargen completion watcher ──────────────────────────────────────────
    // Fires once when the player is in a cell after chargen dialogs are shown.
    if (mCharGenWatching && mIsNewCharacter)
    {
        try
        {
            // Cell-presence is robust: fires regardless of how chargen ends.
            // bypass=true already set sCharGenState=-1 so we can't use that.
            const bool inCell = MWBase::Environment::get()
                                    .getWorld()->getPlayerPtr().isInCell();
            if (inCell)
            {
                mCharGenWatching = false;
                mIsNewCharacter  = false;

                Log(Debug::Info) << "[MP] Chargen complete — notifying server";

                // Read the player's chosen race/class/birthsign from the world
                // so the server can persist them for future logins.
                try
                {
                    MWBase::World* world = MWBase::Environment::get().getWorld();
                    MWWorld::Ptr playerPtr = world->getPlayerPtr();
                    const ESM::NPC* npc = playerPtr.get<ESM::NPC>()->mBase;
                    const MWWorld::Player& player = world->getPlayer();
                    const ESM::RefId& birthSign = player.getBirthSign();

                    BasePlayer& local = mPlayerSync->localPlayer();
                    // Use serializeText() so index-based RefIds (e.g. built-in
                    // classes like "0xb") round-trip correctly through DB and wire.
                    local.race     = npc->mRace.serializeText();
                    local.headMesh = npc->mHead.serializeText();
                    local.hairMesh = npc->mHair.serializeText();
                    local.isMale   = npc->isMale();

                    // Class
                    const ESM::Class* cls = world->getStore()
                        .get<ESM::Class>().search(npc->mClass);
                    if (cls)
                    {
                        local.charClass     = *cls;
                        local.charClass.mId = npc->mClass;
                    }
                    local.birthSign = birthSign.serializeText();

                    Log(Debug::Info) << "[MP] Chargen data: race=" << local.race
                                     << " class=" << local.charClass.mId.toString()
                                     << " birthSign=" << local.birthSign;
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "[MP] Could not read chargen data: " << e.what();
                }

                // Tell server: chargen complete + send all data
                PacketPlayerCharGen pkt;
                pkt.setPlayer(&mPlayerSync->localPlayer());
                mClient->sendReliable(pkt.encode());

            }

        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "[MP] chargen watcher error: " << e.what();
            mCharGenWatching = false;
        }
    }
}

// ---------------------------------------------------------------------------
void Main::onConnected()
{
    Log(Debug::Info) << "[MP] Connected — sending handshake";

    // Build and send handshake
    PacketHandshake hs;
    hs.clientVersion = "0.1.0";
    hs.playerName    = mPlayerName;
    hs.passwordHash  = mPasswordHash;
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
bool Main::isNetworkDisconnected() const
{
    return mClient->getState() == ConnectionState::Disconnected;
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
                mRejectReason = rsp.rejectReason;
                Log(Debug::Error) << "[MP] Server rejected handshake: " << mRejectReason;
                mClient->disconnect("Rejected: " + mRejectReason);
                return;
            }

            mPlayerSync->localPlayer().guid = rsp.assignedGuid;
            mIsNewCharacter = rsp.isNewCharacter;
            mSpawnCell      = rsp.spawnCell;
            mSpawnPos[0] = rsp.spawnX;
            mSpawnPos[1] = rsp.spawnY;
            mSpawnPos[2] = rsp.spawnZ;
            mSpawnRot[0] = rsp.spawnRotX;
            mSpawnRot[1] = rsp.spawnRotY;
            mSpawnRot[2] = rsp.spawnRotZ;

            // Store restored chargen data for returning players.
            // CharacterSelectDialog will apply these after newGame(true).
            mRestoredRace      = rsp.race;
            mRestoredHeadMesh  = rsp.headMesh;
            mRestoredHairMesh  = rsp.hairMesh;
            mRestoredIsMale    = rsp.isMale;
            mRestoredClassId   = rsp.classId;
            mRestoredClassName = rsp.className;
            mRestoredBirthSign = rsp.birthSign;
            mRestoredClassData = rsp.classData;

            // Decode class data directly into localPlayer so CharacterSelectDialog
            // can construct a full ESM::Class and call setPlayerClass(cls).
            if (!rsp.classData.empty())
            {
                std::istringstream ss(rsp.classData);
                char sep;
                auto& d = mPlayerSync->localPlayer().charClass.mData;
                ss >> d.mSpecialization;
                for (auto& v : d.mAttribute)  { ss >> sep >> v; }
                for (auto& row : d.mSkills)   for (auto& v : row) { ss >> sep >> v; }
                ss >> sep >> d.mIsPlayable;
                ss >> sep >> d.mServices;
                mPlayerSync->localPlayer().charClass.mName = rsp.className;
            }

            Log(Debug::Info) << "[MP] Restored chargen: race=" << mRestoredRace
                             << " class=" << mRestoredClassName
                             << " birthSign=" << mRestoredBirthSign;
            Log(Debug::Info) << "[MP] Handshake accepted, guid="
                             << rsp.assignedGuid
                             << " server=" << rsp.serverVersion
                             << " newChar=" << (mIsNewCharacter ? "yes" : "no");

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
                pkt.currentWeather, pkt.nextWeather,
                pkt.transitionFactor, pkt.regionName);
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
