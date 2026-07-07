#ifndef OPENMW_MWMP_MAIN_HPP
#define OPENMW_MWMP_MAIN_HPP

#include <filesystem>
#include <memory>
#include <osg/Vec3f>
#include <string>
#include <vector>
#include <components/openmw-mp/Packets/System/PacketHandshake.hpp>

namespace MWWorld { class Ptr; }
namespace Files { class Collections; }

namespace mwmp
{
    // Forward declarations — full definitions in PacketHandshake.hpp
    struct CharacterEntry;
    class NetworkClient;
    class Protocol;
    class PlayerSync;
    class PlayerList;
    class ActorSync;
    class CellSync;
    class ObjectSync;
    class WorldObjectSync;
    class WorldStateSync;
    class ChatWindow;
    class MpNetworkBridge;

    // -----------------------------------------------------------------------
    // Main — singleton that owns every multiplayer subsystem.
    //
    // Lifecycle (called from engine.cpp when BUILD_MULTIPLAYER is defined):
    //   Main::init(host, port)   — construct subsystems, connect
    //   Main::get().frame(dt)    — called every engine frame
    //   Main::destroy()          — tear down, disconnect
    //
    // Access subsystems through Main::get().getXxx().
    // -----------------------------------------------------------------------
    class Main
    {
    public:
        // Singleton access
        static Main& get();
        static bool  isInitialised();

        // Lifecycle
        static bool init   (const std::string& host, uint16_t port,
                            const std::string& playerName,
                            const std::string& passwordHash,
                            bool isRegistration = false,
                            bool useKeypair     = true,
                            const std::string& autoCharacterName = {});
        static void destroy();

        // Per-frame update — call from engine frame loop
        void frame(float dt);

        // Subsystem accessors
        NetworkClient&  getNetworking()     { return *mClient; }
        Protocol&       getProtocol()       { return *mProtocol; }
        PlayerSync&     getPlayerSync()     { return *mPlayerSync; }
        PlayerList&     getPlayerList()     { return *mPlayerList; }
        ActorSync&      getActorSync()      { return *mActorSync; }
        CellSync&       getCellSync()       { return *mCellSync; }
        ObjectSync&     getObjectSync()     { return *mObjectSync; }
        WorldObjectSync& getWorldObjectSync() { return *mWorldObjectSync; }
        WorldStateSync& getWorldStateSync() { return *mWorldStateSync; }
        ChatWindow& getChatWindow() { return *mChatWindow; }
        MpNetworkBridge& getNetworkBridge() { return *mNetworkBridge; }
        bool hasChatWindow() const { return mChatWindow != nullptr; }

        // Connection state queries (used by AccountDialog to poll results)
        bool               isWorldReady()          const { return mWorldReady; }
        const std::string& getRejectReason()        const { return mRejectReason; }
        const std::string& getPlayerName()          const { return mPlayerName; }
        bool               isNewCharacter()         const { return mIsNewCharacter; }
        bool               isCharacterDataReady()   const { return mCharacterDataReady; }
        bool               isKeypairLinked()        const { return mIsLinked; }
        const std::string& getLocalPublicKey()       const { return mLocalPublicKey; }
        static void        setStaticKeysDir(const std::filesystem::path& dir);
        static void        setFileCollections(const Files::Collections* collections);
        const std::string& getCharSelectError()      const { return mCharSelectError; }
        void               clearCharSelectError()          { mCharSelectError.clear(); }
        bool               isDeleteCharResponseReady() const { return mDeleteCharResponseReady; }
        const PacketDeleteCharResponse& getDeleteCharResponse() const { return mDeleteCharResponse; }
        void               clearDeleteCharResponse()  { mDeleteCharResponseReady = false; }
        const std::string& getSpawnCell()            const { return mSpawnCell; }
        const std::string& getCharacterName()        const { return mCharacterName; }
        float              getSpawnX()    const { return mSpawnPos[0]; }
        float              getSpawnY()    const { return mSpawnPos[1]; }
        float              getSpawnZ()    const { return mSpawnPos[2]; }
        float              getSpawnRotX() const { return mSpawnRot[0]; }
        float              getSpawnRotY() const { return mSpawnRot[1]; }
        float              getSpawnRotZ() const { return mSpawnRot[2]; }

        // Character list received after handshake — used to populate CharacterSelectDialog.
        const std::vector<CharacterEntry>& getCharacterList() const { return mCharacterList; }
        /// Called by CharacterSelectDialog to arm the chargen-completion watcher.
        void               startWatchingCharGen()          { mCharGenWatching = true; }
        bool               isNetworkDisconnected()  const;
        // Returns true while a GNS connection is alive (in-lobby or in-world)
        static bool        isConnected();
        // Gracefully disconnect; safe to call from any thread context
        void               disconnect(const std::string& reason = "Client disconnect");
        void               sendCharacterSelect(const std::string& charName, bool isNew);
        bool               enterSelectedCharacterWorld(bool allowNewCharacterUi);
        void               sendActorCombatRequest(const MWWorld::Ptr& victim, float damage, bool healthDamage,
            bool knocked, const osg::Vec3f& hitPos, int attackType, float attackStrength);
        void               sendActorNpcPlayerHit(uint32_t victimGuid, const MWWorld::Ptr& npcAttacker, float damage,
            bool healthDamage, bool isDead, int attackType);

        // Restored chargen data — populated from PacketCharacterData after character selection
        const std::string& getRestoredRace()      const { return mRestoredRace; }
        const std::string& getRestoredHeadMesh()  const { return mRestoredHeadMesh; }
        const std::string& getRestoredHairMesh()  const { return mRestoredHairMesh; }
        bool               getRestoredIsMale()    const { return mRestoredIsMale; }
        const std::string& getRestoredClassId()   const { return mRestoredClassId; }
        const std::string& getRestoredClassName() const { return mRestoredClassName; }
        const std::string& getRestoredClassData() const { return mRestoredClassData; }
        const std::string& getRestoredBirthSign() const { return mRestoredBirthSign; }

    private:
        Main();
        ~Main();
        Main(const Main&)            = delete;
        Main& operator=(const Main&) = delete;

        void registerProtocolHandlers();
        void handleChallenge(const uint8_t* data, size_t size);
        void onConnected();
        void onDisconnected();
        void tryAutoSelectCharacter();
        void tryAutoEnterWorld();
        void applySelectedCharacterSpawn(const std::string& spawnCell, const char* context);
        bool captureCurrentChargenData(const char* context);
        std::string currentChargenDataKey();
        void sendChargenUpdate(bool complete, const char* reason, bool includeInventoryAndEquipment);
        void pollChargenAppearance(float dt);

        static Main* sInstance;

        std::unique_ptr<NetworkClient>  mClient;
        std::unique_ptr<Protocol>       mProtocol;
        std::unique_ptr<PlayerSync>     mPlayerSync;
        std::unique_ptr<PlayerList>     mPlayerList;
        std::unique_ptr<ActorSync>      mActorSync;
        std::unique_ptr<CellSync>       mCellSync;
        std::unique_ptr<ObjectSync>     mObjectSync;
        std::unique_ptr<WorldObjectSync> mWorldObjectSync;
        std::unique_ptr<WorldStateSync> mWorldStateSync;
        std::unique_ptr<ChatWindow> mChatWindow;
        std::unique_ptr<MpNetworkBridge> mNetworkBridge;

        std::string mPlayerName;
        bool        mWorldReady           = false;
        bool        mIsNewCharacter       = true;
        int64_t     mCharacterId          = 0;
        std::string mCharacterName;       ///< selected character slot name (may differ from login name)
        bool        mCharacterDataReady   = false;
        std::string mCharSelectError;
        bool                     mDeleteCharResponseReady = false;
        PacketDeleteCharResponse mDeleteCharResponse;
        bool        mIsLinked             = false; ///< true if this server knows our keypair
        std::string mLocalPublicKey;               ///< base64 public key for current server
        std::vector<CharacterEntry> mCharacterList;
        bool        mCharGenWatching  = false;
        float       mCharGenAppearanceSyncTimer = 0.f;
        std::string mLastCharGenDataKey;
        std::string mSpawnCell;
        float       mSpawnPos[3] = {0.f, 0.f, 0.f};
        float       mSpawnRot[3] = {0.f, 0.f, 0.f};
        std::string mPasswordHash;
        bool        mIsRegistration = false;
        bool        mUseKeypair          = true;
        std::string mAutoCharacterName;
        bool        mAutoCharacterSelectSent = false;
        bool        mAutoEnterPending = false;
        bool        mAutoEnterAllowNewCharacterUi = false;
        bool        mUnexpectedDisconnect = false; ///< set by onDisconnected when world was ready; polled on main thread
        std::string mHost;
        uint16_t    mPort           = 25565;
        std::string mRejectReason;

        // Chargen restore data (returning players)
        std::string mRestoredRace;
        std::string mRestoredHeadMesh;
        std::string mRestoredHairMesh;
        bool        mRestoredIsMale    = true;
        std::string mRestoredClassId;
        std::string mRestoredClassName;
        std::string mRestoredBirthSign;
        std::string mRestoredClassData;
    };

} // namespace mwmp

#endif // OPENMW_MWMP_MAIN_HPP
