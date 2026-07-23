#ifndef OPENMW_MP_PACKETOBJECTDELETE_HPP
#define OPENMW_MP_PACKETOBJECTDELETE_HPP

#include <components/openmw-mp/Packets/BasePacket.hpp>

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PacketObjectDelete — remove a placed object by mpNum, or a canonical
    // vanilla corpse by refId/refNum when mpNum is zero.
    //
    // Client → Server: send mpNum of the object to remove.
    // Server: remove from WorldState, rebroadcast raw bytes to all in cell.
    // Client receive: look up Ptr by mpNum, call MWBase::World::deleteObject().
    // -----------------------------------------------------------------------
    class PacketObjectDelete : public BasePacket
    {
    public:
        uint32_t    mpNum  = 0;
        std::string cellId;
        std::string refId;
        uint32_t    refNum = 0;
        bool        takenIntoInventory = false;

        PacketObjectDelete() : BasePacket(PacketType::ObjectDelete) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mpNum);
            ws.writeString(cellId);
            ws.writeString(refId);
            ws.write(refNum);
            ws.write(takenIntoInventory);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mpNum);
            cellId = rs.readString();
            refId = rs.readString();
            rs.read(refNum);
            rs.read(takenIntoInventory);
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETOBJECTDELETE_HPP
