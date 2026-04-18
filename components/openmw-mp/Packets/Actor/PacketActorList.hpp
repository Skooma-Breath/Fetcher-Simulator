#ifndef OPENMW_MP_PACKETACTORLIST_HPP
#define OPENMW_MP_PACKETACTORLIST_HPP

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorList : public ActorPacket
    {
    public:
        PacketActorList()
            : ActorPacket(PacketType::ActorList)
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
                packVelocity(ws, actor.velocity);
                packActorPresentation(ws, actor);
                packDynamicStats(ws, actor.dynamicStats);
                packAnimFlags(ws, actor.animFlags);
                packAnimPlay(ws, actor.animPlay);
                packAttack(ws, actor.attack);
                packCast(ws, actor.cast);
                packEquipment(ws, actor.equipment);
                packAI(ws, actor.ai);
                ws.write(actor.deathState);
                ws.write(actor.isDead);
                ws.write(actor.isInstantDeath);
                ws.write(actor.isFollowerCellChange);
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
                unpackVelocity(rs, actor.velocity);
                unpackActorPresentation(rs, actor);
                unpackDynamicStats(rs, actor.dynamicStats);
                unpackAnimFlags(rs, actor.animFlags);
                unpackAnimPlay(rs, actor.animPlay);
                unpackAttack(rs, actor.attack);
                unpackCast(rs, actor.cast);
                unpackEquipment(rs, actor.equipment);
                unpackAI(rs, actor.ai);
                rs.read(actor.deathState);
                rs.read(actor.isDead);
                rs.read(actor.isInstantDeath);
                rs.read(actor.isFollowerCellChange);
            }
        }
    };
}

#endif // OPENMW_MP_PACKETACTORLIST_HPP
