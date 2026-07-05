#include "Main.hpp"
#include "Identity.hpp"
#include "MpNetworkBridge.hpp"
#include "sha256.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <stdexcept>
#include <string_view>

#include <components/debug/debuglog.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerPosition.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerCellChange.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerStatsDynamic.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerBaseInfo.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerEquipment.hpp>
#include <components/openmw-mp/Packets/Player/PacketChatMessage.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerDeath.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerResurrect.hpp>
#include <components/openmw-mp/Packets/System/PacketGameSettings.hpp>
#include <components/openmw-mp/Packets/System/PacketHandshake.hpp>
#include <components/openmw-mp/Packets/Worldstate/PacketRecordDynamic.hpp>
#include <components/openmw-mp/Packets/Worldstate/PacketWorldTime.hpp>
#include <components/openmw-mp/Packets/Object/PacketDoorState.hpp>
#include <components/openmw-mp/Packets/Object/PacketObjectPlace.hpp>
#include <components/openmw-mp/Packets/Object/PacketObjectDelete.hpp>
#include <components/openmw-mp/Packets/Object/PacketObjectMove.hpp>
#include <components/openmw-mp/Packets/Object/PacketContainer.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerAnimFlags.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerAnimPlay.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerAttack.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerCast.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerSpeech.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerInventory.hpp>
#include <components/openmw-mp/Packets/Lua/PacketLuaEvent.hpp>
#include <components/openmw-mp/Packets/Lua/PacketLuaStorage.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAI.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAnimFlags.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAnimPlay.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAttack.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAttackV2.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorAuthority.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorCast.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorCellChange.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorCombatRequest.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorDeath.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorEquipment.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorIdentity.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorList.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorPosition.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorPositionV2.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorPresentationV2.hpp>
#include <components/openmw-mp/Packets/Actor/PacketActorStatsDynamic.hpp>

#include "network/Client.hpp"
#include "network/Protocol.hpp"
#include "sync/PlayerSync.hpp"
#include "sync/RemotePlayer.hpp"
#include "sync/ActorSync.hpp"
#include "sync/CellSync.hpp"
#include "sync/ObjectSync.hpp"
#include "sync/WorldObjectSync.hpp"
#include "sync/WorldStateSync.hpp"
#include "gui/ChatWindow.hpp"

#include <components/openmw-mp/Packets/Player/PacketPlayerCharGen.hpp>
#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwphysics/surfphysics.hpp"
#include "../mwbase/world.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwgui/inventorywindow.hpp"
#include <components/vfs/pathutil.hpp>
#include "../mwgui/mode.hpp"
#include "../mwworld/player.hpp"
#include "../mwworld/esmstore.hpp"
#include <sstream>
#include <filesystem>
#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadclas.hpp>

namespace mwmp
{
namespace
{
    bool startsWithNoCase(std::string_view value, std::string_view prefix)
    {
        if (value.size() < prefix.size())
            return false;

        for (std::size_t i = 0; i < prefix.size(); ++i)
        {
            const auto left = static_cast<unsigned char>(value[i]);
            const auto right = static_cast<unsigned char>(prefix[i]);
            if (std::tolower(left) != std::tolower(right))
                return false;
        }

        return true;
    }

    std::string normalizeSpeechSoundPath(std::string soundPath)
    {
        std::replace(soundPath.begin(), soundPath.end(), '/', '\\');
        if (startsWithNoCase(soundPath, "sound\\") || !startsWithNoCase(soundPath, "vo\\"))
            return soundPath;

        return "Sound\\" + soundPath;
    }
}

Main* Main::sInstance = nullptr;

// ---------------------------------------------------------------------------
Main& Main::get()
{
    if (!sInstance)
        throw std::runtime_error("mwmp::Main not initialised");
    return *sInstance;
}

void Main::sendActorCombatRequest(const MWWorld::Ptr& victim, float damage, bool healthDamage, bool knocked,
    const osg::Vec3f& hitPos, int attackType, float attackStrength)
{
    if (mActorSync)
        mActorSync->sendCombatRequest(victim, damage, healthDamage, knocked, hitPos, attackType, attackStrength);
}

void Main::sendActorNpcPlayerHit(uint32_t victimGuid, const MWWorld::Ptr& npcAttacker, float damage, bool healthDamage,
    bool isDead, int attackType)
{
    if (mActorSync)
        mActorSync->sendNpcPlayerDamage(victimGuid, damage, healthDamage, isDead, attackType, npcAttacker);
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
                bool useKeypair,
                const std::string& autoCharacterName)
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
        sInstance->mAutoCharacterName = autoCharacterName;
        sInstance->mHost           = host;
        sInstance->mPort           = port;
        sInstance->mPlayerSync->localPlayer().name = playerName;
        MWBase::Environment::get().getLuaManager()->prepareMultiplayerPlayerStorage();

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
        if (!autoCharacterName.empty())
        {
            Log(Debug::Info) << "[MP] Auto character selection armed"
                             << " account=" << playerName
                             << " character=" << autoCharacterName;
        }
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
    mWorldObjectSync = std::make_unique<WorldObjectSync>(*mClient);
    mWorldStateSync= std::make_unique<WorldStateSync>(*mClient);
    mChatWindow = std::make_unique<ChatWindow>(*mClient);
    mNetworkBridge = std::make_unique<MpNetworkBridge>();
}

Main::~Main()
{
    if (mClient && mClient->isConnected())
    {
        if (mPlayerSync)
            mPlayerSync->flushPersistentStats();
        mClient->disconnect("Client shutdown");
    }
}

// ---------------------------------------------------------------------------
void Main::frame(float dt)
{
    if (!mClient) return;

    mClient->update();
    if (mNetworkBridge && mClient->isConnected())
        mNetworkBridge->drainOutgoing(*mClient);

    // Handle unexpected server disconnect — return player to main menu.
    if (mUnexpectedDisconnect)
    {
        mUnexpectedDisconnect = false;
        mCharGenWatching = false;
        Log(Debug::Warning) << "[MP] Unexpected disconnect — returning to main menu";
        MWBase::Environment::get().getStateManager()->returnToMainMenu();
        return;
    }

    if (!mClient->isConnected()) return;

    tryAutoEnterWorld();
    if (!mClient->isConnected()) return;

    mPlayerSync->update(dt);
    mPlayerList->updateAll(dt);
    mActorSync->update(dt);
    mObjectSync->update(dt);
    mWorldObjectSync->update(dt);
    mWorldStateSync->update(dt);

    mChatWindow->update(dt);
    pollChargenAppearance(dt);

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

                sendChargenUpdate(true, "complete", true);

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
bool Main::captureCurrentChargenData(const char* context)
{
    try
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (!world)
            return false;

        MWWorld::Ptr playerPtr = world->getPlayerPtr();
        if (playerPtr.isEmpty())
            return false;

        const auto* npcRef = playerPtr.get<ESM::NPC>();
        if (!npcRef || !npcRef->mBase)
            return false;

        const ESM::NPC* npc = npcRef->mBase;
        BasePlayer& local = mPlayerSync->localPlayer();

        local.race = npc->mRace.serializeText();
        local.headMesh = npc->mHead.serializeText();
        local.hairMesh = npc->mHair.serializeText();
        local.isMale = npc->isMale();

        const ESM::Class* cls = world->getStore().get<ESM::Class>().search(npc->mClass);
        if (cls)
        {
            local.charClass = *cls;
            local.charClass.mId = npc->mClass;
        }
        else
            local.charClass.mId = npc->mClass;

        local.birthSign = world->getPlayer().getBirthSign().serializeText();
        return true;
    }
    catch (const std::exception& e)
    {
        Log(Debug::Warning) << "[MP] Could not read chargen data for " << context << ": " << e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
std::string Main::currentChargenDataKey()
{
    const BasePlayer& local = mPlayerSync->localPlayer();
    std::ostringstream key;
    key << local.race << '\n'
        << local.headMesh << '\n'
        << local.hairMesh << '\n'
        << local.isMale << '\n'
        << local.charClass.mId.serializeText() << '\n'
        << local.charClass.mName << '\n'
        << local.birthSign;
    return key.str();
}

// ---------------------------------------------------------------------------
void Main::sendChargenUpdate(bool complete, const char* reason, bool includeInventoryAndEquipment)
{
    if (!mClient || !mClient->isConnected())
        return;

    if (!captureCurrentChargenData(reason))
        return;

    PacketPlayerCharGen pkt;
    pkt.setPlayer(&mPlayerSync->localPlayer());
    pkt.isComplete = complete;
    mClient->sendReliable(pkt.encode());

    const BasePlayer& local = mPlayerSync->localPlayer();
    Log(Debug::Info) << "[MP] Chargen update sent: reason=" << reason
                     << " complete=" << complete
                     << " race=" << local.race
                     << " head=" << local.headMesh
                     << " hair=" << local.hairMesh
                     << " class=" << local.charClass.mId.toString()
                     << " birthSign=" << local.birthSign;

    mLastCharGenDataKey = currentChargenDataKey();
    mPlayerSync->forceFullSync(includeInventoryAndEquipment);
}

// ---------------------------------------------------------------------------
void Main::pollChargenAppearance(float dt)
{
    if (!mIsNewCharacter || !mClient || !mClient->isConnected())
    {
        mCharGenAppearanceSyncTimer = 0.f;
        return;
    }

    mCharGenAppearanceSyncTimer -= dt;
    if (mCharGenAppearanceSyncTimer > 0.f)
        return;
    mCharGenAppearanceSyncTimer = 0.25f;

    if (!captureCurrentChargenData("live"))
        return;

    const std::string key = currentChargenDataKey();
    if (mLastCharGenDataKey.empty())
    {
        mLastCharGenDataKey = key;
        return;
    }

    if (key != mLastCharGenDataKey)
        sendChargenUpdate(false, "live", false);
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
    hs.actorSyncProtocolVersion = ActorSyncProtocolVersionV2;
    
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
    // If we were already in-world, request a main-menu return on the next frame.
    // Do NOT touch engine state here — this fires inside mClient->update() and
    // must remain engine-API-free to stay thread-safe.
    if (mWorldReady)
        mUnexpectedDisconnect = true;
    mWorldReady         = false;
    mCharacterDataReady = false;
    mCharacterId        = 0;
    mCharSelectError.clear();
    mCharacterName.clear();
    mCharacterList.clear();
    mAutoCharacterSelectSent = false;
    mAutoEnterPending = false;
    mAutoEnterAllowNewCharacterUi = false;
    mIsLinked       = false;
    mLocalPublicKey.clear();
    MWPhysics::resetSurfPhysicsSettings();
    // Clear all per-session actor/cell tracking so that stale MWWorld::Ptr
    // references from the now-dying game world are never accessed on reconnect.
    if (mActorSync)
        mActorSync->resetSessionState();
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
/*static*/
bool Main::isConnected()
{
    return sInstance && sInstance->mClient && sInstance->mClient->isConnected();
}

// ---------------------------------------------------------------------------
void Main::disconnect(const std::string& reason)
{
    if (mClient && mClient->isConnected())
    {
        if (mPlayerSync)
            mPlayerSync->flushPersistentStats();
        mClient->disconnect(reason);
    }
}

// ---------------------------------------------------------------------------
void Main::sendCharacterSelect(const std::string& charName, bool isNew)
{
    if (!mClient)
        return;

    mCharSelectError.clear();
    mCharacterDataReady = false;
    mCharacterId = 0;

    PacketCharacterSelect pkt;
    pkt.charName = charName;
    pkt.isNew = isNew;
    mClient->sendReliable(pkt.encode());

    Log(Debug::Info) << "[MP] Sent CharacterSelect: '" << charName << "' isNew=" << isNew;
}

// ---------------------------------------------------------------------------
void Main::tryAutoSelectCharacter()
{
    if (mAutoCharacterName.empty() || mAutoCharacterSelectSent)
        return;

    const auto characterIt = std::find_if(mCharacterList.begin(), mCharacterList.end(),
        [&](const CharacterEntry& entry) { return entry.name == mAutoCharacterName; });
    if (characterIt == mCharacterList.end())
    {
        mRejectReason = "Auto character '" + mAutoCharacterName + "' was not found on account '" + mPlayerName + "'.";
        Log(Debug::Error) << "[MP] " << mRejectReason;
        disconnect(mRejectReason);
        return;
    }

    mAutoCharacterSelectSent = true;
    mAutoEnterAllowNewCharacterUi = characterIt->isNew;
    Log(Debug::Info) << "[MP] Auto-selecting character '" << characterIt->name
                     << "' incomplete=" << characterIt->isNew;
    sendCharacterSelect(characterIt->name, false);
}

// ---------------------------------------------------------------------------
bool Main::enterSelectedCharacterWorld(bool allowNewCharacterUi)
{
    MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();

    const bool isNew = isNewCharacter();
    const std::string spawnCell = getSpawnCell();
    const std::string worldName = getCharacterName().empty()
        ? getPlayerSync().localPlayer().name
        : getCharacterName();

    if (isNew && !allowNewCharacterUi)
    {
        Log(Debug::Error) << "[MP] Auto-enter refused incomplete/new character '" << worldName << "'";
        disconnect("Auto-enter refused incomplete character");
        return false;
    }

    const auto normalizedIdentityPart = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    const std::string storageNamespace = crypto::sha256hex(
        normalizedIdentityPart(mHost) + ":" + std::to_string(mPort) + "\n" + normalizedIdentityPart(mPlayerName));
    const std::string characterKey = mCharacterId > 0
        ? "id-" + std::to_string(mCharacterId)
        : "name-" + crypto::sha256hex(normalizedIdentityPart(worldName));
    std::string storageError;
    if (!MWBase::Environment::get().getLuaManager()->bindMultiplayerPlayerStorage(
            storageNamespace, characterKey, worldName, storageError))
    {
        Log(Debug::Error) << "[MP] " << storageError;
        windowManager->messageBox(storageError);
        return false;
    }

    windowManager->removeGuiMode(MWGui::GM_MainMenu);

    if (isNew)
    {
        Log(Debug::Info) << "[MP] New character - spawning in: " << spawnCell;
        mLastCharGenDataKey.clear();
        mCharGenAppearanceSyncTimer = 0.f;
        MWBase::Environment::get().getStateManager()->newGame(true);
        applySelectedCharacterSpawn(spawnCell, "new character");
        windowManager->updatePlayer();
        if (!worldName.empty())
            MWBase::Environment::get().getMechanicsManager()->setPlayerName(worldName);
        Log(Debug::Info) << "[MP] New character initial world sync";
        getPlayerSync().forceFullSync(false);
        windowManager->setCharGenCompleteCallback(
            []() {
                if (Main::isInitialised())
                {
                    Log(Debug::Info) << "[MP] Chargen complete - arming watcher";
                    Main::get().startWatchingCharGen();
                }
                MWBase::Environment::get().getWindowManager()->setNewGame(false);
            });
        windowManager->startCharGen();
        windowManager->pushGuiMode(MWGui::GM_Race);
        return true;
    }

    Log(Debug::Info) << "[MP] Returning player - restoring in: " << spawnCell;
    MWBase::Environment::get().getStateManager()->newGame(true);
    windowManager->updatePlayer();

    if (!worldName.empty())
        MWBase::Environment::get().getMechanicsManager()->setPlayerName(worldName);

    auto mechanicsManager = MWBase::Environment::get().getMechanicsManager();
    try
    {
        const std::string race = getRestoredRace();
        const std::string head = getRestoredHeadMesh();
        const std::string hair = getRestoredHairMesh();
        const bool isMale = getRestoredIsMale();
        const std::string className = getRestoredClassName();
        const std::string birthSign = getRestoredBirthSign();

        if (!race.empty())
        {
            mechanicsManager->setPlayerRace(ESM::RefId::deserializeText(race), isMale,
                ESM::RefId::deserializeText(head), ESM::RefId::deserializeText(hair));
            windowManager->getInventoryWindow()->rebuildAvatar();
        }
        if (!className.empty())
        {
            ESM::Class playerClass;
            playerClass.mName = className;
            playerClass.mData = getPlayerSync().localPlayer().charClass.mData;
            playerClass.mRecordFlags = 0;
            mechanicsManager->setPlayerClass(playerClass);
        }
        if (!birthSign.empty())
            mechanicsManager->setPlayerBirthsign(ESM::RefId::deserializeText(birthSign));
    }
    catch (const std::exception& e)
    {
        Log(Debug::Warning) << "[MP] Chargen restore error: " << e.what();
    }

    applySelectedCharacterSpawn(spawnCell, "returning player");

    getPlayerSync().applyRestoredStatsToPlayer();
    Log(Debug::Info) << "[MP] Returning player restore complete - sending full sync";
    getPlayerSync().forceFullSync(false);
    return true;
}

// ---------------------------------------------------------------------------
void Main::applySelectedCharacterSpawn(const std::string& spawnCell, const char* context)
{
    const std::string targetCell = spawnCell.empty() ? "toddtest" : spawnCell;
    const float sx = getSpawnX();
    const float sy = getSpawnY();
    const float sz = getSpawnZ();
    const float rx = getSpawnRotX();
    const float ry = getSpawnRotY();
    const float rz = getSpawnRotZ();
    const bool hasSavedPos = sx != 0.f || sy != 0.f || sz != 0.f;

    MWBase::World* world = MWBase::Environment::get().getWorld();
    ESM::Position dest {};

    int exteriorGridX = 0;
    int exteriorGridY = 0;
    const bool isExteriorCellKey = targetCell.rfind("EXT:", 0) == 0;
    if (isExteriorCellKey)
    {
        if (std::sscanf(targetCell.c_str(), "EXT:%d,%d", &exteriorGridX, &exteriorGridY) != 2)
            throw std::runtime_error("Invalid saved exterior cell key: " + targetCell);

        // Player database exterior locations use the canonical EXT:x,y key.
        // Resolve that key directly instead of passing it through the named-cell
        // lookup, which treats it as an interior name and aborts the restore.
        dest.pos[0] = sx;
        dest.pos[1] = sy;
        dest.pos[2] = sz;
        dest.rot[0] = rx;
        dest.rot[1] = ry;
        dest.rot[2] = rz;
        world->changeToCell(
            ESM::Cell::generateIdForCell(true, {}, exteriorGridX, exteriorGridY), dest, true);
    }
    else
    {
        const auto interiorId = world->findInteriorPosition(targetCell, dest);
        if (!interiorId.empty())
        {
            if (hasSavedPos)
            {
                dest.pos[0] = sx;
                dest.pos[1] = sy;
                dest.pos[2] = sz;
                dest.rot[0] = rx;
                dest.rot[1] = ry;
                dest.rot[2] = rz;
            }
            world->changeToCell(interiorId, dest, true);
        }
        else
        {
            const auto exteriorId = world->findExteriorPosition(targetCell, dest);
            if (!exteriorId.empty())
            {
                if (hasSavedPos)
                {
                    dest.pos[0] = sx;
                    dest.pos[1] = sy;
                    dest.pos[2] = sz;
                    dest.rot[0] = rx;
                    dest.rot[1] = ry;
                    dest.rot[2] = rz;
                }
                world->changeToCell(exteriorId, dest, true);
            }
            else
                world->changeToInteriorCell(targetCell, dest, true);
        }
    }

    Log(Debug::Info) << "[MP] Applied " << context << " spawn: cell=" << targetCell
                     << " pos=(" << dest.pos[0] << "," << dest.pos[1] << "," << dest.pos[2] << ")"
                     << " rot=(" << dest.rot[0] << "," << dest.rot[1] << "," << dest.rot[2] << ")";
}

// ---------------------------------------------------------------------------
void Main::tryAutoEnterWorld()
{
    if (!mAutoEnterPending || !mCharacterDataReady)
        return;

    const bool allowNewCharacterUi = mAutoEnterAllowNewCharacterUi;
    mAutoEnterPending = false;
    mAutoEnterAllowNewCharacterUi = false;
    enterSelectedCharacterWorld(allowNewCharacterUi);
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
                             << " server=" << rsp.serverVersion
                             << " actorSyncProtocol=" << rsp.actorSyncProtocolVersion;
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
            for (const auto& entry : mCharacterList)
            {
                Log(Debug::Info) << "[MP] Character entry"
                                 << " name=" << entry.name
                                 << " isNew=" << entry.isNew
                                 << " race=" << entry.race
                                 << " class=" << entry.className;
            }

            // Signal AccountDialog that the connection is ready so it can
            // open CharacterSelectDialog.
            mWorldReady = true;
            tryAutoSelectCharacter();
        });

    // --- Character select error — server rejected the CharacterSelect request ---
    proto.registerHandler(PacketType::CharacterSelectError,
        [this](const uint8_t* data, size_t size)
        {
            PacketCharacterSelectError err;
            if (!err.decode(data, size)) return;
            mCharSelectError = err.reason;
            Log(Debug::Warning) << "[MP] CharacterSelect rejected: " << mCharSelectError;
            if (!mAutoCharacterName.empty())
            {
                mRejectReason = mCharSelectError;
                disconnect("Auto CharacterSelect rejected: " + mCharSelectError);
            }
        });

    
    // --- Delete character response ---
    proto.registerHandler(PacketType::DeleteCharResponse,
        [this](const uint8_t* data, size_t size)
        {
            PacketDeleteCharResponse rsp;
            if (!rsp.decode(data, size)) return;
            mDeleteCharResponse = rsp;
            mDeleteCharResponseReady = true;
            Log(Debug::Info) << "[MP] DeleteCharResponse: success=" << rsp.success
                             << " char='" << rsp.charName << "'"
                             << (rsp.success ? "" : " error=" + rsp.error);
        });
// --- Character data — arrives after client sends PacketCharacterSelect ---
    proto.registerHandler(PacketType::CharacterData,
        [this](const uint8_t* data, size_t size)
        {
            PacketCharacterData cd;
            if (!cd.decode(data, size)) return;

            mIsNewCharacter = cd.isNewCharacter;
            mCharacterId    = cd.characterId;
            mCharacterName  = cd.characterName;
            // Update the sync layer name to the character slot name so
            // PacketPlayerBaseInfo (sent by forceFullSync in CharacterSelectDialog
            // after setPlayerRace()) broadcasts the correct name to other players.
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
            BasePlayer& localPlayer = mPlayerSync->localPlayer();
            localPlayer.hasSavedStats = cd.hasSavedStats;
            localPlayer.dynamicStats = cd.dynamicStats;
            localPlayer.attributes = cd.attributes;
            localPlayer.skills = cd.skills;
            localPlayer.level = cd.level;
            localPlayer.levelProgress = cd.levelProgress;
            if (cd.hasSavedStats)
            {
                BasePlayer restoredStats;
                restoredStats.hasSavedStats = true;
                restoredStats.dynamicStats = cd.dynamicStats;
                restoredStats.attributes = cd.attributes;
                restoredStats.skills = cd.skills;
                restoredStats.level = cd.level;
                restoredStats.levelProgress = cd.levelProgress;
                mPlayerSync->queueRestoredStats(restoredStats);
            }

            if (!cd.classData.empty())
            {
                std::istringstream ss(cd.classData);
                char sep;
                auto& d = localPlayer.charClass.mData;
                ss >> d.mSpecialization;
                for (auto& v : d.mAttribute)  { ss >> sep >> v; }
                for (auto& row : d.mSkills)   for (auto& v : row) { ss >> sep >> v; }
                ss >> sep >> d.mIsPlayable;
                ss >> sep >> d.mServices;
                localPlayer.charClass.mName = cd.className;
            }

            Log(Debug::Info) << "[MP] CharacterData received: newChar="
                             << (mIsNewCharacter ? "yes" : "no")
                             << " charId=" << mCharacterId
                             << " charName=" << mCharacterName
                             << " cell=" << mSpawnCell
                             << " pos=(" << mSpawnPos[0] << "," << mSpawnPos[1] << "," << mSpawnPos[2] << ")"
                             << " rot=(" << mSpawnRot[0] << "," << mSpawnRot[1] << "," << mSpawnRot[2] << ")"
                             << " race=" << mRestoredRace
                             << " class=" << mRestoredClassName;

            mCharacterDataReady = true;
            if (!mAutoCharacterName.empty())
            {
                if (mIsNewCharacter)
                    mAutoEnterAllowNewCharacterUi = true;
                mAutoEnterPending = true;
            }
            // NOTE: do NOT call forceFullSync() here for returning players.
            // At this point world->getPlayerPtr() still has the blank template
            // NPC record — setPlayerRace() has not been called yet.
            // CharacterSelectDialog::startReturningPlayer() calls forceFullSync()
            // *after* setPlayerRace() so the BaseInfo packet carries the real
            // race/head/hair. New characters use the chargen-complete watcher.
        });

    // --- Remote player position ---
    proto.registerHandler(PacketType::PlayerPosition,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerPosition pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid)
            {
                mPlayerSync->applyServerPositionCorrection(tmp);
                return;
            }
            if (tmp.guid == 0) return;

            auto* rp = mPlayerList->getPlayer(tmp.guid);
            if (rp) rp->onPositionUpdate(tmp, pkt.getSequence());
        });

    // --- Remote player cell change ---
    proto.registerHandler(PacketType::PlayerCellChange,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerCellChange pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid)
            {
                mPlayerSync->applyServerCellChange(tmp);
                return;
            }

            auto* rp = mPlayerList->getPlayer(tmp.guid);
            if (rp) rp->onCellChange(tmp, pkt.getSequence());
        });

    // --- Remote player base info (join / appearance) ---
    proto.registerHandler(PacketType::PlayerBaseInfo,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerBaseInfo pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;

            if (tmp.guid == 0)
            {
                Log(Debug::Warning) << "[MP] BaseInfo received with guid=0 for player '"
                                    << tmp.name << "' — ignoring (stale pre-handshake packet)";
                return;
            }

            // If this is our own guid, update localPlayer name so outgoing
            // chat and base-info broadcasts use the current nickname.
            if (tmp.guid == mPlayerSync->localPlayer().guid)
            {
                mPlayerSync->localPlayer().name = tmp.name;
                return;
            }

            RemotePlayer* rp = mPlayerList->getPlayer(tmp.guid);
            if (!rp)
            {
                mPlayerList->addPlayer(tmp.guid, tmp.name);
                Log(Debug::Info) << "[MP] Player joined: " << tmp.name
                                 << " (guid=" << tmp.guid << ")";
                // Populate appearance on the new RemotePlayer immediately —
                // addPlayer() only sets the name; race/head/hair live in onBaseInfoUpdate.
                rp = mPlayerList->getPlayer(tmp.guid);
            }
            // Always apply appearance (covers both new join and live updates)
            if (rp) rp->onBaseInfoUpdate(tmp);
        });

    // --- Remote player equipment ---
    proto.registerHandler(PacketType::PlayerEquipment,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerEquipment pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid)
            {
                mPlayerSync->queueAuthoritativeEquipment(tmp);
                return;
            }

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
            mChatWindow->addMessage(tmp.name, pkt.message, pkt.channel);
        });

        // --- World time ---
    proto.registerHandler(PacketType::WorldTime,
        [this](const uint8_t* data, size_t size)
        {
            PacketWorldTime pkt;
            if (!pkt.decode(data, size)) return;
            mWorldStateSync->onServerTimeUpdate(pkt.time, pkt.timeScale);
        });

    proto.registerHandler(PacketType::GameSettings,
        [](const uint8_t* data, size_t size)
        {
            PacketGameSettings pkt;
            if (!pkt.decode(data, size)) return;

            MWPhysics::setSurfPhysicsSettings(pkt.settings);
            Log(Debug::Info) << "[MP] Applied surf settings for cell=" << pkt.settings.cellId
                             << " enabled=" << (pkt.settings.enabled ? "true" : "false");
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

    proto.registerHandler(PacketType::RecordDynamic,
        [this](const uint8_t* data, size_t size)
        {
            PacketRecordDynamic pkt;
            if (!pkt.decode(data, size)) return;
            mWorldStateSync->onServerRecordDynamic(pkt.action, pkt.recordType, std::move(pkt.entries));
        });

    // --- Persisted / relayed world objects ---
    proto.registerHandler(PacketType::ObjectPlace,
        [this](const uint8_t* data, size_t size)
        {
            PacketObjectPlace pkt;
            if (!pkt.decode(data, size)) return;
            mWorldObjectSync->onServerObjectPlace(
                pkt.object.mpNum, pkt.object.refId, pkt.object.count,
                pkt.object.position, pkt.object.cellId);
        });

    proto.registerHandler(PacketType::ObjectDelete,
        [this](const uint8_t* data, size_t size)
        {
            PacketObjectDelete pkt;
            if (!pkt.decode(data, size)) return;
            mWorldObjectSync->onServerObjectDelete(pkt.mpNum, pkt.cellId);
        });

    proto.registerHandler(PacketType::ObjectMove,
        [this](const uint8_t* data, size_t size)
        {
            PacketObjectMove pkt;
            if (!pkt.decode(data, size)) return;
            mWorldObjectSync->onServerObjectMove(pkt.mpNum, pkt.cellId, pkt.position);
        });

    proto.registerHandler(PacketType::Container,
        [this](const uint8_t* data, size_t size)
        {
            PacketContainer pkt;
            if (!pkt.decode(data, size)) return;
            mWorldObjectSync->onServerContainer(
                pkt.container,
                static_cast<ContainerAction>(pkt.mAction));
        });

    // --- Remote player animation flags ---
    proto.registerHandler(PacketType::PlayerAnimFlags,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerAnimFlags pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid) return;

            auto* rp = mPlayerList->getPlayer(tmp.guid);
            if (rp) rp->onAnimFlagsUpdate(tmp);
        });

    // --- Remote player one-shot animation ---
    proto.registerHandler(PacketType::PlayerAnimPlay,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerAnimPlay pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid) return;

            auto* rp = mPlayerList->getPlayer(tmp.guid);
            if (rp) rp->onAnimPlay(tmp);
        });

    // --- Remote player attack event ---
    proto.registerHandler(PacketType::PlayerAttack,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerAttack pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid) return;

            auto* rp = mPlayerList->getPlayer(tmp.guid);
            if (rp) rp->onAttack(tmp);
        });

    // --- Player speech event ---
    proto.registerHandler(PacketType::PlayerSpeech,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerSpeech pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size) || tmp.speechSound.empty()) return;
            tmp.speechSound = normalizeSpeechSoundPath(tmp.speechSound);

            if (tmp.guid == mPlayerSync->localPlayer().guid)
            {
                MWBase::World* world = MWBase::Environment::get().getWorld();
                MWBase::SoundManager* sound = MWBase::Environment::get().getSoundManager();
                if (world && sound)
                    sound->say(world->getPlayerPtr(), VFS::Path::Normalized(tmp.speechSound));
                return;
            }

            auto* rp = mPlayerList->getPlayer(tmp.guid);
            if (rp) rp->onSpeech(tmp);
        });

    // --- Remote player cast event ---
    proto.registerHandler(PacketType::PlayerCast,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerCast pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid) return;

            auto* rp = mPlayerList->getPlayer(tmp.guid);
            if (rp) rp->onCast(tmp);
        });

    // --- Remote player cosmetic inventory delta ---
    proto.registerHandler(PacketType::PlayerInventory,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerInventory pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid)
            {
                mPlayerSync->queueAuthoritativeInventory(tmp);
                return;
            }

            auto* rp = mPlayerList->getPlayer(tmp.guid);
            if (rp) rp->onInventoryUpdate(tmp);
        });

    proto.registerHandler(PacketType::PlayerDeath,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerDeath pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid)
            {
                mPlayerSync->applyServerDeath(tmp);
                return;
            }

            auto* rp = mPlayerList->getPlayer(tmp.guid);
            if (rp) rp->onDeath(tmp);
        });

    proto.registerHandler(PacketType::PlayerResurrect,
        [this](const uint8_t* data, size_t size)
        {
            BasePlayer tmp;
            PacketPlayerResurrect pkt;
            pkt.setPlayer(&tmp);
            if (!pkt.decode(data, size)) return;
            if (tmp.guid == mPlayerSync->localPlayer().guid) return;

            auto* rp = mPlayerList->getPlayer(tmp.guid);
            if (rp) rp->onResurrect(tmp);
        });

    proto.registerHandler(PacketType::ActorAuthority,
        [this](const uint8_t* data, size_t size)
        {
            ActorList tmp;
            PacketActorAuthority pkt;
            pkt.setActorList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onAuthorityUpdate(tmp);
        });

    proto.registerHandler(PacketType::ActorList,
        [this](const uint8_t* data, size_t size)
        {
            ActorList tmp;
            PacketActorList pkt;
            pkt.setActorList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorListUpdate(tmp);
        });

    proto.registerHandler(PacketType::ActorIdentity,
        [this](const uint8_t* data, size_t size)
        {
            ActorIdentityList tmp;
            PacketActorIdentity pkt;
            pkt.setIdentityList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorIdentityUpdate(tmp);
        });

    proto.registerHandler(PacketType::ActorPosition,
        [this](const uint8_t* data, size_t size)
        {
            ActorList tmp;
            PacketActorPosition pkt;
            pkt.setActorList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorPositionUpdate(tmp);
        });

    proto.registerHandler(PacketType::ActorPositionV2,
        [this](const uint8_t* data, size_t size)
        {
            ActorPositionV2List tmp;
            PacketActorPositionV2 pkt;
            pkt.setPositionList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorPositionV2Update(tmp);
        });

    proto.registerHandler(PacketType::ActorPresentationV2,
        [this](const uint8_t* data, size_t size)
        {
            ActorPresentationV2List tmp;
            PacketActorPresentationV2 pkt;
            pkt.setPresentationList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorPresentationV2Update(tmp);
        });

    proto.registerHandler(PacketType::ActorAnimFlags,
        [this](const uint8_t* data, size_t size)
        {
            ActorList tmp;
            PacketActorAnimFlags pkt;
            pkt.setActorList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorAnimFlagsUpdate(tmp);
        });

    proto.registerHandler(PacketType::ActorAnimPlay,
        [this](const uint8_t* data, size_t size)
        {
            ActorList tmp;
            PacketActorAnimPlay pkt;
            pkt.setActorList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorAnimPlay(tmp);
        });

    proto.registerHandler(PacketType::ActorAttack,
        [this](const uint8_t* data, size_t size)
        {
            ActorList tmp;
            PacketActorAttack pkt;
            pkt.setActorList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorAttack(tmp);
        });

    proto.registerHandler(PacketType::ActorAttackV2,
        [this](const uint8_t* data, size_t size)
        {
            ActorAttackV2List tmp;
            PacketActorAttackV2 pkt;
            pkt.setAttackList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorAttackV2(tmp);
        });

    proto.registerHandler(PacketType::ActorCast,
        [this](const uint8_t* data, size_t size)
        {
            ActorList tmp;
            PacketActorCast pkt;
            pkt.setActorList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorCast(tmp);
        });

    proto.registerHandler(PacketType::ActorCellChange,
        [this](const uint8_t* data, size_t size)
        {
            ActorList tmp;
            PacketActorCellChange pkt;
            pkt.setActorList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorCellChange(tmp);
        });

    proto.registerHandler(PacketType::ActorDeath,
        [this](const uint8_t* data, size_t size)
        {
            ActorList tmp;
            PacketActorDeath pkt;
            pkt.setActorList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorDeath(tmp);
        });

    proto.registerHandler(PacketType::ActorEquipment,
        [this](const uint8_t* data, size_t size)
        {
            ActorList tmp;
            PacketActorEquipment pkt;
            pkt.setActorList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorEquipment(tmp);
        });

    proto.registerHandler(PacketType::ActorStatsDynamic,
        [this](const uint8_t* data, size_t size)
        {
            ActorList tmp;
            PacketActorStatsDynamic pkt;
            pkt.setActorList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorStatsDynamic(tmp);
        });

    proto.registerHandler(PacketType::ActorAI,
        [this](const uint8_t* data, size_t size)
        {
            ActorList tmp;
            PacketActorAI pkt;
            pkt.setActorList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorAI(tmp);
        });

    proto.registerHandler(PacketType::ActorCombatRequest,
        [this](const uint8_t* data, size_t size)
        {
            ActorList tmp;
            PacketActorCombatRequest pkt;
            pkt.setActorList(&tmp);
            if (!pkt.decode(data, size)) return;
            mActorSync->onActorCombatRequest(tmp);
        });

    proto.registerHandler(PacketType::PacketLuaEvent,
        [this](const uint8_t* data, size_t size)
        {
            PacketLuaEvent pkt;
            if (!pkt.decode(data, size)) return;

            if (mNetworkBridge)
                mNetworkBridge->queueInbound({ pkt.pid, std::move(pkt.eventName), std::move(pkt.eventData) });
        });

    proto.registerHandler(PacketType::PacketLuaStorage,
        [this](const uint8_t* data, size_t size)
        {
            PacketLuaStorage pkt;
            if (!pkt.decode(data, size)) return;

            if (mNetworkBridge)
                mNetworkBridge->queueStorage(pkt.action, std::move(pkt.section), std::move(pkt.entries));
        });

    Log(Debug::Info) << "[MP] Protocol handlers registered";
}

} // namespace mwmp
