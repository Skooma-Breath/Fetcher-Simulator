#ifndef OPENMW_MWMP_SYNC_WORLDSTATESYNC_HPP
#define OPENMW_MWMP_SYNC_WORLDSTATESYNC_HPP
#include <string>
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
        void onServerWeatherUpdate(int current, int next, float transition,
                                   const std::string& regionName);

        // Diagnostic accessors — used by /time chat command
        bool        hasServerTime()  const { return mHasServerTime; }
        bool        isTimeApplied()  const { return mTimeApplied; }
        const Time& lastServerTime() const { return mTime; }
        float       lastTimeScale()  const { return mTimeScale; }
    private:
        NetworkClient& mClient;

        // --- Time state ---
        Time  mTime;
        float mTimeScale          = 30.f;
        float mSyncTimer          = 0.f;
        bool  mHasServerTime      = false;
        bool  mTimeApplied        = false;
        bool applyServerTime();

        // --- Weather state ---
        int         mWeather          = 0;
        int         mNextWeather      = -1;
        float       mTransitionFactor = 0.f;
        std::string mWeatherRegion;           // serialised ESM::RefId of the host's region
        bool        mHasServerWeather = false;
        bool        mWeatherApplied   = false;
        float       mWeatherReportTimer = 0.f; // host-side send timer
        bool applyServerWeather();
        void sendWeatherToServer();

        static constexpr float SYNC_RATE           = 5.f;
        static constexpr float WEATHER_REPORT_RATE = 30.f; // host reports every 30 real-seconds
    };
} // namespace mwmp
#endif
