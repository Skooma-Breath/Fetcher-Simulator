#ifndef OPENMW_MP_PACKETACTORANIMFLAGS_HPP
#define OPENMW_MP_PACKETACTORANIMFLAGS_HPP

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorAnimFlags : public ActorPacket
    {
    public:
        PacketActorAnimFlags()
            : ActorPacket(PacketType::ActorAnimFlags)
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
                packAnimFlags(ws, actor.animFlags);
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
                unpackAnimFlags(rs, actor.animFlags);
            }
        }
    };
}

#endif // OPENMW_MP_PACKETACTORANIMFLAGS_HPP
