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
    private:
        NetworkClient& mClient;
        Time  mTime;
        float mTimeScale   = 30.f;
        int   mWeather     = 0;
        float mSyncTimer   = 0.f;
        static constexpr float SYNC_RATE = 5.f;
    };
} // namespace mwmp
#endif
