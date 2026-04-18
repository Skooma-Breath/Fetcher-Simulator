#ifndef OPENMW_MP_PACKETACTORAUTHORITY_HPP
#define OPENMW_MP_PACKETACTORAUTHORITY_HPP

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorAuthority : public ActorPacket
    {
    public:
        PacketActorAuthority()
            : ActorPacket(PacketType::ActorAuthority)
        {
        }

    protected:
        void pack(WriteStream& ws) override
        {
            packBatchHeader(ws);
        }

        void unpack(ReadStream& rs) override
        {
            unpackBatchHeader(rs);
        }
    };
}

#endif // OPENMW_MP_PACKETACTORAUTHORITY_HPP
