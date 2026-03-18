#ifndef OPENMW_MWMP_SYNC_WORLDSTATESYNC_HPP
#define OPENMW_MWMP_SYNC_WORLDSTATESYNC_HPP
#include <components/openmw-mp/Base/BaseStructs.hpp>
namespace mwmp { class NetworkClient; }
namespace mwmp
{
    // Phase 4: time, weather, global variables.
    class WorldStateSync
    {
    public:
        explicit WorldStateSync(NetworkClient& client);
        void update(float dt);
        void onServerTimeUpdate   (const Time& t, float timeScale);
        void onServerWeatherUpdate(int current, int next, float transition);

        // Diagnostic accessors — used by /time chat command
        bool        hasServerTime()  const { return mHasServerTime; }
        bool        isTimeApplied()  const { return mTimeApplied; }
        const Time& lastServerTime() const { return mTime; }
        float       lastTimeScale()  const { return mTimeScale; }
    private:
        NetworkClient& mClient;
        Time  mTime;
        float mTimeScale       = 30.f;
        int   mWeather         = 0;
        float mSyncTimer       = 0.f;
        bool  mHasServerTime   = false; // true once we've received at least one WorldTime packet
        bool  mTimeApplied     = false; // true once the stored time has been applied to the world
        static constexpr float SYNC_RATE = 5.f;

        // Apply mTime/mTimeScale to the world if it is ready.
        // Returns true on success so callers can set mTimeApplied.
        bool applyServerTime();
    };
} // namespace mwmp
#endif
