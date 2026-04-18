#ifndef OPENMW_MP_PACKETACTORCAST_HPP
#define OPENMW_MP_PACKETACTORCAST_HPP

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorCast : public ActorPacket
    {
    public:
        PacketActorCast()
            : ActorPacket(PacketType::ActorCast)
        {
        }

    protected:
        void pack(WriteStream& ws) override
        {
            packBatchHeader(ws);
            const auto count = static_cast<uint16_t>(mActorList->actors.size());
            ws.write(count);
            for (const auto& actor : mActorList->actors)
            {
                packActorIdentity(ws, actor);
                packCast(ws, actor.cast);
            }
        }

        void unpack(ReadStream& rs) override
        {
            unpackBatchHeader(rs);
            uint16_t count = 0;
            rs.read(count);
            mActorList->actors.resize(count);
            for (auto& actor : mActorList->actors)
            {
                unpackActorIdentity(rs, actor);
                unpackCast(rs, actor.cast);
            }
        }
    };
}

#endif // OPENMW_MP_PACKETACTORCAST_HPP
