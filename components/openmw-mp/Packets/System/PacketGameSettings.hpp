#ifndef OPENMW_MP_PACKETGAMESETTINGS_HPP
#define OPENMW_MP_PACKETGAMESETTINGS_HPP

#include <components/openmw-mp/Base/SurfPhysicsSettings.hpp>
#include <components/openmw-mp/Packets/BasePacket.hpp>

namespace mwmp
{
    class PacketGameSettings : public BasePacket
    {
    public:
        SurfPhysicsSettings settings;

        PacketGameSettings()
            : BasePacket(PacketType::GameSettings)
        {
        }

    protected:
        void pack(WriteStream& ws) override
        {
            ws.writeString(settings.cellId);
            ws.write(settings.enabled);
            ws.write(settings.airAcceleration);
            ws.write(settings.maxAirSpeed);
            ws.write(settings.groundFriction);
            ws.write(settings.groundAcceleration);
            ws.write(settings.jumpSpeed);
            ws.write(settings.gravityMultiplier);
            ws.write(settings.overbounce);
            ws.write(settings.rampAngle);
            ws.write(settings.impactOverbounce);
            ws.write(settings.impactVelocityThreshold);
        }

        void unpack(ReadStream& rs) override
        {
            settings.cellId = rs.readString();
            rs.read(settings.enabled);
            rs.read(settings.airAcceleration);
            rs.read(settings.maxAirSpeed);
            rs.read(settings.groundFriction);
            rs.read(settings.groundAcceleration);
            rs.read(settings.jumpSpeed);
            rs.read(settings.gravityMultiplier);
            rs.read(settings.overbounce);
            rs.read(settings.rampAngle);
            rs.read(settings.impactOverbounce);
            rs.read(settings.impactVelocityThreshold);
        }
    };
}

#endif
