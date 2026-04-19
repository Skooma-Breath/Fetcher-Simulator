#ifndef OPENMW_MWPHYSICS_SURFPHYSICS_HPP
#define OPENMW_MWPHYSICS_SURFPHYSICS_HPP

#include <mutex>

#include <components/openmw-mp/Base/SurfPhysicsSettings.hpp>

namespace MWPhysics
{
    using SurfPhysicsSettings = mwmp::SurfPhysicsSettings;

    inline std::mutex sSurfPhysicsSettingsMutex;
    inline SurfPhysicsSettings sSurfPhysicsSettings;

    inline SurfPhysicsSettings getSurfPhysicsSettings()
    {
        std::lock_guard<std::mutex> lock(sSurfPhysicsSettingsMutex);
        return sSurfPhysicsSettings;
    }

    inline void setSurfPhysicsSettings(const SurfPhysicsSettings& settings)
    {
        std::lock_guard<std::mutex> lock(sSurfPhysicsSettingsMutex);
        sSurfPhysicsSettings = settings;
    }

    inline void resetSurfPhysicsSettings()
    {
        setSurfPhysicsSettings(SurfPhysicsSettings{});
    }
}

#endif
