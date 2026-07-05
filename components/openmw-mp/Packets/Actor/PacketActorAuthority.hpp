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
            const auto count = static_cast<uint16_t>(mActorList->actors.size());
            ws.write(count);
            for (const BaseActor& actor : mActorList->actors)
                packActorIdentity(ws, actor);
        }

        void unpack(ReadStream& rs) override
        {
            unpackBatchHeader(rs);
            uint16_t count = 0;
            rs.read(count);
            mActorList->actors.resize(count);
            for (BaseActor& actor : mActorList->actors)
                unpackActorIdentity(rs, actor);
        }
    };
}

#endif // OPENMW_MP_PACKETACTORAUTHORITY_HPP
