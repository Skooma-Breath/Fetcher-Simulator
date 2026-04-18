#ifndef OPENMW_MP_PACKETACTORATTACK_HPP
#define OPENMW_MP_PACKETACTORATTACK_HPP

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorAttack : public ActorPacket
    {
    public:
        PacketActorAttack()
            : ActorPacket(PacketType::ActorAttack)
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
                packAttack(ws, actor.attack);
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
                unpackAttack(rs, actor.attack);
            }
        }
    };
}

#endif // OPENMW_MP_PACKETACTORATTACK_HPP
