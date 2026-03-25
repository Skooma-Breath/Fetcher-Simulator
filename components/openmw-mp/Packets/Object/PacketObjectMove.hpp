#ifndef OPENMW_MP_PACKETOBJECTMOVE_HPP
#define OPENMW_MP_PACKETOBJECTMOVE_HPP

#include <components/openmw-mp/Packets/BasePacket.hpp>
#include <components/openmw-mp/Base/BaseStructs.hpp>

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PacketObjectMove — position/rotation update for an already-placed object.
    //
    // Sent unreliably (position changes are frequent; missed updates self-correct
    // when the next one arrives).
    //
    // Client receive: look up Ptr by mpNum, call MWBase::World::moveObject()
    // and rotateObject().
    // -----------------------------------------------------------------------
    class PacketObjectMove : public BasePacket
    {
    public:
        uint32_t    mpNum  = 0;
        std::string cellId;
        Position    position;

        PacketObjectMove() : BasePacket(PacketType::ObjectMove) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mpNum);
            ws.writeString(cellId);
            ws.write(position.pos[0]);
            ws.write(position.pos[1]);
            ws.write(position.pos[2]);
            ws.write(position.rot[0]);
            ws.write(position.rot[1]);
            ws.write(position.rot[2]);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mpNum);
            cellId = rs.readString();
            rs.read(position.pos[0]);
            rs.read(position.pos[1]);
            rs.read(position.pos[2]);
            rs.read(position.rot[0]);
            rs.read(position.rot[1]);
            rs.read(position.rot[2]);
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETOBJECTMOVE_HPP
