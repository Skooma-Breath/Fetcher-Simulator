#ifndef OPENMW_MP_PACKETWORLDTIME_HPP
#define OPENMW_MP_PACKETWORLDTIME_HPP

#include <components/openmw-mp/Packets/BasePacket.hpp>

namespace mwmp
{
    class PacketWorldTime : public BasePacket
    {
    public:
        Time    time;
        float   timeScale = 30.f;

        PacketWorldTime() : BasePacket(PacketType::WorldTime) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(time.day);
            ws.write(time.month);
            ws.write(time.year);
            ws.write(time.hour);
            ws.write(time.dayOfWeek);
            ws.write(timeScale);
        }
        void unpack(ReadStream& rs) override
        {
            rs.read(time.day);
            rs.read(time.month);
            rs.read(time.year);
            rs.read(time.hour);
            rs.read(time.dayOfWeek);
            rs.read(timeScale);
        }
    };

    class PacketWorldWeather : public BasePacket
    {
    public:
        int     currentWeather  = 0;
        int     nextWeather     = 0;
        float   transitionFactor = 0.f;
        std::string regionName;

        PacketWorldWeather() : BasePacket(PacketType::WorldWeather) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(currentWeather);
            ws.write(nextWeather);
            ws.write(transitionFactor);
            ws.writeString(regionName);
        }
        void unpack(ReadStream& rs) override
        {
            rs.read(currentWeather);
            rs.read(nextWeather);
            rs.read(transitionFactor);
            regionName = rs.readString();
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETWORLDTIME_HPP
