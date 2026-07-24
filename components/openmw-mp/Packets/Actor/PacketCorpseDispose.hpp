#ifndef OPENMW_MP_PACKETCORPSEDISPOSE_HPP
#define OPENMW_MP_PACKETCORPSEDISPOSE_HPP

#include <string>

#include <components/openmw-mp/Base/ActorSyncProtocol.hpp>
#include <components/openmw-mp/Packets/BasePacket.hpp>

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PacketCorpseDispose — Client → Server: request authoritative corpse disposal.
    //
    // Carries enough identity to resolve both server-spawned (mpNum != 0) and
    // vanilla (mpNum == 0) dead actors. The server validates, then broadcasts
    // an ActorIdentity removal with ActorRemovalReason::CorpseDisposed.
    // -----------------------------------------------------------------------
    class PacketCorpseDispose : public BasePacket
    {
    public:
        ActorInstanceId actorNetId = 0;
        uint32_t        mpNum     = 0;
        std::string     refId;
        uint32_t        refNum    = 0;
        std::string     cellId;

        PacketCorpseDispose() : BasePacket(PacketType::CorpseDispose) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(actorNetId);
            ws.write(mpNum);
            ws.writeString(refId);
            ws.write(refNum);
            ws.writeString(cellId);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(actorNetId);
            rs.read(mpNum);
            refId = rs.readString();
            rs.read(refNum);
            cellId = rs.readString();
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETCORPSEDISPOSE_HPP
