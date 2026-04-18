#ifndef OPENMW_MP_PACKETACTORANIMPLAY_HPP
#define OPENMW_MP_PACKETACTORANIMPLAY_HPP

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorAnimPlay : public ActorPacket
    {
    public:
        PacketActorAnimPlay()
            : ActorPacket(PacketType::ActorAnimPlay)
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
                packAnimPlay(ws, actor.animPlay);
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
                unpackAnimPlay(rs, actor.animPlay);
            }
        }
    };
}

#endif // OPENMW_MP_PACKETACTORANIMPLAY_HPP
