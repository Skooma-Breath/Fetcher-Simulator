#ifndef OPENMW_MP_PACKETOBJECTPLACE_HPP
#define OPENMW_MP_PACKETOBJECTPLACE_HPP

#include <components/openmw-mp/Packets/BasePacket.hpp>
#include <components/openmw-mp/Base/BaseObject.hpp>

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PacketObjectPlace — place a new world object.
    //
    // Client → Server:
    //   mpNum == 0 for a newly created object. When dropping a whole inventory
    //   stack, mpNum carries its server-issued Item::instanceId; the server
    //   validates ownership before allowing that identity to move into world
    //   state. Split drops receive a fresh mpNum.
    //
    // Server → Clients (relay):
    //   mpNum is filled in.
    //   Clients call MWBase::World::placeObject() and store (mpNum → Ptr).
    //
    // Server → new cell-joining Client (catch-up):
    //   Batch of all PlacedObjects currently in the cell is sent on join.
    // -----------------------------------------------------------------------
    class PacketObjectPlace : public BasePacket
    {
    public:
        PlacedObject object;

        PacketObjectPlace() : BasePacket(PacketType::ObjectPlace) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(object.mpNum);
            ws.writeString(object.refId);
            ws.write(object.count);
            ws.writeString(object.cellId);
            ws.write(object.position.pos[0]);
            ws.write(object.position.pos[1]);
            ws.write(object.position.pos[2]);
            ws.write(object.position.rot[0]);
            ws.write(object.position.rot[1]);
            ws.write(object.position.rot[2]);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(object.mpNum);
            object.refId  = rs.readString();
            rs.read(object.count);
            object.cellId = rs.readString();
            rs.read(object.position.pos[0]);
            rs.read(object.position.pos[1]);
            rs.read(object.position.pos[2]);
            rs.read(object.position.rot[0]);
            rs.read(object.position.rot[1]);
            rs.read(object.position.rot[2]);
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETOBJECTPLACE_HPP
