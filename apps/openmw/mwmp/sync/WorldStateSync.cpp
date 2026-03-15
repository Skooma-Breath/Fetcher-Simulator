#include "WorldStateSync.hpp"
#include <components/debug/debuglog.hpp>
namespace mwmp
{
    WorldStateSync::WorldStateSync(NetworkClient& client) : mClient(client) {}
    void WorldStateSync::update(float /*dt*/) { /* Phase 4 */ }
    void WorldStateSync::onServerTimeUpdate(const Time& t, float timeScale)
    {
        mTime      = t;
        mTimeScale = timeScale;
        Log(Debug::Verbose) << "[MP] WorldStateSync: time=" << t.hour
                            << " scale=" << timeScale;
        // Phase 4: set MWBase::World time
    }
    void WorldStateSync::onServerWeatherUpdate(int current, int next, float transition)
    {
        mWeather = current;
        Log(Debug::Verbose) << "[MP] WorldStateSync: weather current=" << current
                            << " next=" << next << " t=" << transition;
        // Phase 4: set MWBase::World weather
    }
} // namespace mwmp
