#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>

namespace mwmp
{

/**
 * MasterServerClient
 *
 * Handles registration, heartbeat, and unregistration with the public
 * master server list service.  All HTTP calls are made on a background
 * thread so they never block the game server tick loop.
 *
 * Lifecycle:
 *   1. Call registerAsync() once on server startup.
 *   2. Call tickHeartbeat(dt, playerCount) every server tick (~20 Hz).
 *      Internally fires a POST /heartbeat no more than once every
 *      HEARTBEAT_INTERVAL_S seconds.
 *   3. Call unregister() on server shutdown for immediate removal.
 */
class MasterServerClient
{
public:
    static constexpr float HEARTBEAT_INTERVAL_S = 30.0f;

    struct Config
    {
        std::string masterUrl;   ///< e.g. "http://master.openmw-mp.org:8080"
        std::string serverName;
        int         port       = 25565;
        int         maxPlayers = 32;
        std::string version;
        std::string gameMode   = "Co-op";
        bool        passwordProtected = false;
    };

    MasterServerClient();
    ~MasterServerClient();

    /// Begin async registration.  No-op if masterUrl is empty.
    void registerAsync(const Config& config);

    /// Called every server tick.  Fires heartbeat at most once per
    /// HEARTBEAT_INTERVAL_S seconds.
    void tickHeartbeat(float dt, int currentPlayers);

    /// Synchronously unregister.  Called on shutdown.
    void unregister();

    bool isRegistered() const { return !mToken.empty(); }

private:
    void doRegister(const Config& config);
    void doHeartbeat(int currentPlayers);
    void doUnregister();

    std::string  mMasterUrl;

    Config       mLastConfig;       ///< stored for auto-reregister on token expiry
    std::string  mToken;
    float        mHeartbeatAccum  = 0.0f;
    bool         mRegistered      = false;

    std::thread  mWorker;
    std::mutex   mMutex;
};

} // namespace mwmp
