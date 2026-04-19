#ifndef OPENMW_MP_SURFPHYSICSSETTINGS_HPP
#define OPENMW_MP_SURFPHYSICSSETTINGS_HPP

#include <string>

namespace mwmp
{
    struct SurfPhysicsSettings
    {
        std::string cellId;
        bool enabled = false;
        float airAcceleration = 70.f;
        float maxAirSpeed = 2000.f;
        float groundFriction = 5.f;
        float groundAcceleration = 10.f;
        float jumpSpeed = 268.f;
        float gravityMultiplier = 1.f;
        float overbounce = 1.1f;
        float rampAngle = 0.8f;
        float impactOverbounce = 1.1f;
        float impactVelocityThreshold = 200.f;
    };
}

#endif
