#ifndef OPENMW_MP_PACKETACTORAI_HPP
#define OPENMW_MP_PACKETACTORAI_HPP

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorAI : public ActorPacket
    {
    public:
        PacketActorAI()
            : ActorPacket(PacketType::ActorAI)
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
                packAI(ws, actor.ai);
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
                unpackAI(rs, actor.ai);
            }
        }
    };
}

#endif // OPENMW_MP_PACKETACTORAI_HPP
