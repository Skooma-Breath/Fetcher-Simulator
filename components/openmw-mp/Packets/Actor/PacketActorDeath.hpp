#ifndef OPENMW_MP_PACKETACTORDEATH_HPP
#define OPENMW_MP_PACKETACTORDEATH_HPP

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorDeath : public ActorPacket
    {
    public:
        PacketActorDeath()
            : ActorPacket(PacketType::ActorDeath)
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
                packPosition(ws, actor.position);
                ws.write(actor.deathEventId);
                ws.write(actor.deathState);
                ws.write(actor.isDead);
                ws.write(actor.isInstantDeath);
                ws.write(actor.dynamicStats.health.current);
                ws.writeString(actor.deathAnimGroup);
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
                unpackPosition(rs, actor.position);
                rs.read(actor.deathEventId);
                rs.read(actor.deathState);
                rs.read(actor.isDead);
                rs.read(actor.isInstantDeath);
                rs.read(actor.dynamicStats.health.current);
                actor.deathAnimGroup = rs.readString();
            }
        }
    };
}

#endif // OPENMW_MP_PACKETACTORDEATH_HPP
