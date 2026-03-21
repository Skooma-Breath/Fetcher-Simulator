#ifndef OPENMW_MWMP_MAIN_HPP
#define OPENMW_MWMP_MAIN_HPP

#include <memory>
#include <string>

namespace mwmp
{
    class NetworkClient;
    class Protocol;
    class PlayerSync;
    class PlayerList;
    class ActorSync;
    class CellSync;
    class ObjectSync;
    class WorldStateSync;
    class ChatWindow;

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
                            const std::string& passwordHash);
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
        WorldStateSync& getWorldStateSync() { return *mWorldStateSync; }
        ChatWindow& getChatWindow() { return *mChatWindow; }
        bool hasChatWindow() const { return mChatWindow != nullptr; }

        // Connection state queries (used by AccountDialog to poll results)
        bool               isWorldReady()          const { return mWorldReady; }
        const std::string& getRejectReason()        const { return mRejectReason; }
        const std::string& getPlayerName()          const { return mPlayerName; }
        bool               isNewCharacter()         const { return mIsNewCharacter; }
        const std::string& getSpawnCell()            const { return mSpawnCell; }
        float              getSpawnX()    const { return mSpawnPos[0]; }
        float              getSpawnY()    const { return mSpawnPos[1]; }
        float              getSpawnZ()    const { return mSpawnPos[2]; }
        float              getSpawnRotX() const { return mSpawnRot[0]; }
        float              getSpawnRotY() const { return mSpawnRot[1]; }
        float              getSpawnRotZ() const { return mSpawnRot[2]; }
        /// Called by CharacterSelectDialog to arm the chargen-completion watcher.
        void               startWatchingCharGen()          { mCharGenWatching = true; }
        bool               isNetworkDisconnected()  const;

        // Restored chargen data — populated from HandshakeResponse when isNewCharacter=false
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
        void onConnected();
        void onDisconnected();

        static Main* sInstance;

        std::unique_ptr<NetworkClient>  mClient;
        std::unique_ptr<Protocol>       mProtocol;
        std::unique_ptr<PlayerSync>     mPlayerSync;
        std::unique_ptr<PlayerList>     mPlayerList;
        std::unique_ptr<ActorSync>      mActorSync;
        std::unique_ptr<CellSync>       mCellSync;
        std::unique_ptr<ObjectSync>     mObjectSync;
        std::unique_ptr<WorldStateSync> mWorldStateSync;
        std::unique_ptr<ChatWindow> mChatWindow;

        std::string mPlayerName;
        bool        mWorldReady       = false;
        bool        mIsNewCharacter   = true;
        bool        mCharGenWatching  = false;
        std::string mSpawnCell;
        float       mSpawnPos[3] = {0.f, 0.f, 0.f};
        float       mSpawnRot[3] = {0.f, 0.f, 0.f};
        std::string mPasswordHash;
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
