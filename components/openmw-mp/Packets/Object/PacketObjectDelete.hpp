#ifndef OPENMW_MP_PACKETOBJECTDELETE_HPP
#define OPENMW_MP_PACKETOBJECTDELETE_HPP

#include <components/openmw-mp/Packets/BasePacket.hpp>

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PacketObjectDelete — remove a placed object by mpNum.
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
        bool        takenIntoInventory = false;

        PacketObjectDelete() : BasePacket(PacketType::ObjectDelete) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mpNum);
            ws.writeString(cellId);
            ws.write(takenIntoInventory);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mpNum);
            cellId = rs.readString();
            rs.read(takenIntoInventory);
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETOBJECTDELETE_HPP
