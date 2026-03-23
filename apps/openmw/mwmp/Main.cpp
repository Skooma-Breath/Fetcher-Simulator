#include "Main.hpp"
#include "Identity.hpp"
#include <cstring>

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
#include <filesystem>
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
                const std::string& passwordHash,
                bool isRegistration,
                bool useKeypair)
{
    if (sInstance)
    {
        Log(Debug::Warning) << "[MP] Main::init called while already initialised";
        return true;
    }

    try
    {
        sInstance = new Main();
        sInstance->mPlayerName     = playerName;
        sInstance->mPasswordHash   = passwordHash;
        sInstance->mIsRegistration = isRegistration;
        sInstance->mUseKeypair     = useKeypair;
        sInstance->mHost           = host;
        sInstance->mPort           = port;
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
    hs.clientVersion   = "0.1.0";
    hs.playerName      = mPlayerName;
    hs.passwordHash    = mPasswordHash;
    hs.isRegistration  = mIsRegistration;
    
    if (mUseKeypair)
    {
        mLocalPublicKey = Identity::getPublicKeyBase64(mHost, mPort);
        if (!mLocalPublicKey.empty())
        {
            hs.publicKey = mLocalPublicKey;
            mIsLinked    = true;
            Log(Debug::Info) << "[MP] Sending keypair auth for " << mPlayerName;
        }
        else
            mIsLinked = false;
    }
    else
    {
        mLocalPublicKey.clear();
        mIsLinked = false;
        Log(Debug::Info) << "[MP] Connected to server";
    }
    mClient->sendReliable(hs.encode());
}

// ---------------------------------------------------------------------------
void Main::onDisconnected()
{
    Log(Debug::Warning) << "[MP] Disconnected from server";
    mWorldReady         = false;
    mCharacterDataReady = false;
    mCharSelectError.clear();
    mCharacterName.clear();
    mCharacterList.clear();
    mIsLinked       = false;
    mLocalPublicKey.clear();
    // Phase 3: show reconnect dialog
}

// ---------------------------------------------------------------------------
/*static*/
void Main::setStaticKeysDir(const std::filesystem::path& dir)
{
    Identity::setKeysDir(dir);
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
            if (!rsp.decode(data, size)) return;

            if (!rsp.accepted)
            {
                mRejectReason = rsp.rejectReason;
                Log(Debug::Error) << "[MP] Server rejected handshake: " << mRejectReason;
                mClient->disconnect("Rejected: " + mRejectReason);
                return;
            }

            mPlayerSync->localPlayer().guid = rsp.assignedGuid;

            Log(Debug::Info) << "[MP] Handshake accepted, guid=" << rsp.assignedGuid
                             << " server=" << rsp.serverVersion;
            // mWorldReady is set when PacketCharacterList arrives.
        });

    // --- Ed25519 challenge — server sends 32-byte nonce, we sign and respond ---
    proto.registerHandler(PacketType::Challenge,
        [this](const uint8_t* data, size_t size)
        {
            Log(Debug::Info) << "[MP] Challenge received, signing nonce";
            PacketChallenge pkt;
            if (!pkt.decode(data, size))
            {
                Log(Debug::Error) << "[MP] Challenge decode FAILED";
                return;
            }
            uint8_t sig[64] = {};
            if (!Identity::sign(mHost, mPort, pkt.nonce, sig))
            {
                Log(Debug::Error) << "[MP] Identity::sign FAILED for "
                                  << mHost << ":" << mPort;
                mClient->disconnect("No keypair");
                return;
            }
            PacketChallengeResponse rsp;
            std::memcpy(rsp.signature, sig, 64);
            mClient->sendReliable(rsp.encode());
            Log(Debug::Info) << "[MP] Challenge response sent";
            });
    Log(Debug::Info) << "[MP] Challenge handler registered, type="
                     << static_cast<int>(PacketType::Challenge);

// --- Character list — arrives immediately after accepted handshake ---
    proto.registerHandler(PacketType::CharacterList,
        [this](const uint8_t* data, size_t size)
        {
            PacketCharacterList pkt;
            if (!pkt.decode(data, size)) return;

            mCharacterList = pkt.characters;
            Log(Debug::Info) << "[MP] Received character list: "
                             << mCharacterList.size() << " character(s)";

            // Signal AccountDialog that the connection is ready so it can
            // open CharacterSelectDialog.
            mWorldReady = true;
        });

    // --- Character select error — server rejected the CharacterSelect request ---
    proto.registerHandler(PacketType::CharacterSelectError,
        [this](const uint8_t* data, size_t size)
        {
            PacketCharacterSelectError err;
            if (!err.decode(data, size)) return;
            mCharSelectError = err.reason;
            Log(Debug::Warning) << "[MP] CharacterSelect rejected: " << mCharSelectError;
        });

    // --- Character data — arrives after client sends PacketCharacterSelect ---
    proto.registerHandler(PacketType::CharacterData,
        [this](const uint8_t* data, size_t size)
        {
            PacketCharacterData cd;
            if (!cd.decode(data, size)) return;

            mIsNewCharacter = cd.isNewCharacter;
            mCharacterName  = cd.characterName;
            // Update the sync layer name to the character slot name so
            // PacketPlayerBaseInfo (sent via forceFullSync below) broadcasts
            // the correct name to other players. Falls back to login name.
            if (!cd.characterName.empty())
                mPlayerSync->localPlayer().name = cd.characterName;
            mSpawnCell      = cd.spawnCell;
            mSpawnPos[0] = cd.spawnX;
            mSpawnPos[1] = cd.spawnY;
            mSpawnPos[2] = cd.spawnZ;
            mSpawnRot[0] = cd.spawnRotX;
            mSpawnRot[1] = cd.spawnRotY;
            mSpawnRot[2] = cd.spawnRotZ;

            mRestoredRace      = cd.race;
            mRestoredHeadMesh  = cd.headMesh;
            mRestoredHairMesh  = cd.hairMesh;
            mRestoredIsMale    = cd.isMale;
            mRestoredClassId   = cd.classId;
            mRestoredClassName = cd.className;
            mRestoredBirthSign = cd.birthSign;
            mRestoredClassData = cd.classData;

            if (!cd.classData.empty())
            {
                std::istringstream ss(cd.classData);
                char sep;
                auto& d = mPlayerSync->localPlayer().charClass.mData;
                ss >> d.mSpecialization;
                for (auto& v : d.mAttribute)  { ss >> sep >> v; }
                for (auto& row : d.mSkills)   for (auto& v : row) { ss >> sep >> v; }
                ss >> sep >> d.mIsPlayable;
                ss >> sep >> d.mServices;
                mPlayerSync->localPlayer().charClass.mName = cd.className;
            }

            Log(Debug::Info) << "[MP] CharacterData received: newChar="
                             << (mIsNewCharacter ? "yes" : "no")
                             << " charName=" << mCharacterName
                             << " cell=" << mSpawnCell
                             << " race=" << mRestoredRace
                             << " class=" << mRestoredClassName;

            mCharacterDataReady = true;
            // For returning players, push base info immediately so other
            // clients can render us. New characters must wait until chargen
            // completes and the watcher fires.
            if (!cd.isNewCharacter)
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
