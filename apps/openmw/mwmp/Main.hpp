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

        std::string mPlayerName;
        bool        mWorldReady = false;
    };

} // namespace mwmp

#endif // OPENMW_MWMP_MAIN_HPP
