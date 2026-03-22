#ifndef OPENMW_MP_PACKETDOORSTATE_HPP
#define OPENMW_MP_PACKETDOORSTATE_HPP

#include <components/openmw-mp/Packets/BasePacket.hpp>
#include <string>
#include <vector>

namespace mwmp
{
    struct DoorEntry
    {
        std::string cellId;
        std::string refId;
        uint32_t    refNum    = 0;
        uint32_t    mpNum     = 0;
        bool        isOpen    = false;
        bool        isLocked  = false;
        int         lockLevel = 0;
    };

    class PacketDoorState : public BasePacket
    {
    public:
        uint32_t              authorGuid = 0;
        std::string           cellId;
        std::vector<DoorEntry> doors;

        PacketDoorState() : BasePacket(PacketType::DoorState) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(authorGuid);
            ws.writeString(cellId);
            auto count = static_cast<uint32_t>(doors.size());
            ws.write(count);
            for (const auto& d : doors)
            {
                ws.writeString(d.cellId);
                ws.writeString(d.refId);
                ws.write(d.refNum);
                ws.write(d.mpNum);
                ws.write(d.isOpen);
                ws.write(d.isLocked);
                ws.write(d.lockLevel);
            }
        }
        void unpack(ReadStream& rs) override
        {
            rs.read(authorGuid);
            cellId = rs.readString();
            uint32_t count = 0;
            rs.read(count);
            doors.resize(count);
            for (auto& d : doors)
            {
                d.cellId   = rs.readString();
                d.refId    = rs.readString();
                rs.read(d.refNum);
                rs.read(d.mpNum);
                rs.read(d.isOpen);
                rs.read(d.isLocked);
                rs.read(d.lockLevel);
            }
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETDOORSTATE_HPP
