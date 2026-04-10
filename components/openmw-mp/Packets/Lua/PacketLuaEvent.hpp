#ifndef OPENMW_MP_PACKETLUAEVENT_HPP
#define OPENMW_MP_PACKETLUAEVENT_HPP

#include <components/lua/serialization.hpp>

#include "../BasePacket.hpp"

namespace mwmp
{
    class PacketLuaEvent : public BasePacket
    {
    public:
        PacketLuaEvent()
            : BasePacket(PacketType::PacketLuaEvent)
        {
        }

        uint32_t pid = 0;
        std::string eventName;
        LuaUtil::BinaryData eventData;

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(pid);
            ws.writeString(eventName);
            ws.writeBytes(eventData);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(pid);
            eventName = rs.readString();
            eventData = rs.readBytes();
        }
    };
}

#endif // OPENMW_MP_PACKETLUAEVENT_HPP
