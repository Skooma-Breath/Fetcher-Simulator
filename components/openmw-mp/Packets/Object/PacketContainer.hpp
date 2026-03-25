#ifndef OPENMW_MP_PACKETCONTAINER_HPP
#define OPENMW_MP_PACKETCONTAINER_HPP

#include <components/openmw-mp/Packets/BasePacket.hpp>
#include <components/openmw-mp/Base/BaseObject.hpp>

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PacketContainer — shared container (chest/barrel/crate) sync.
    //
    // Flow:
    //   Player opens container → client sends action=Set with full contents.
    //   Server: if no authority record, store it and echo back to opener.
    //           if authority record exists, send it back as action=Set.
    //   Player takes/adds items → client sends action=Remove / Add deltas.
    //   Server: apply delta to record, relay to all clients in cell.
    //
    // Race condition (two players open same container simultaneously):
    //   Server serialises updates — first-writer wins.
    //   Slower client receives server's authoritative Set and UI refreshes.
    // -----------------------------------------------------------------------
    class PacketContainer : public BasePacket
    {
    public:
        ContainerRecord container;

        PacketContainer() : BasePacket(PacketType::Container) {}

    protected:
        void packItem(WriteStream& ws, const ContainerItem& item)
        {
            ws.writeString(item.refId);
            ws.write(item.count);
            ws.write(item.charge);
        }

        void unpackItem(ReadStream& rs, ContainerItem& item)
        {
            item.refId = rs.readString();
            rs.read(item.count);
            rs.read(item.charge);
        }

        void pack(WriteStream& ws) override
        {
            ws.writeString(container.cellId);
            ws.writeString(container.refId);
            ws.write(container.refNum);
            ws.write(container.mpNum);
            auto action = static_cast<uint8_t>(
                container.hasAuthority ? ContainerAction::Set : ContainerAction::Set);
            // caller sets action explicitly via a parallel field below
            ws.write(mAction);
            auto count = static_cast<uint16_t>(container.items.size());
            ws.write(count);
            for (const auto& item : container.items)
                packItem(ws, item);
        }

        void unpack(ReadStream& rs) override
        {
            container.cellId = rs.readString();
            container.refId  = rs.readString();
            rs.read(container.refNum);
            rs.read(container.mpNum);
            rs.read(mAction);
            uint16_t count = 0;
            rs.read(count);
            container.items.resize(count);
            for (auto& item : container.items)
                unpackItem(rs, item);
        }

    public:
        // Explicit action field — set by caller before encode(), read after decode().
        uint8_t mAction = static_cast<uint8_t>(ContainerAction::Set);
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETCONTAINER_HPP
