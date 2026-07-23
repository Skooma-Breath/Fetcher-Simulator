#ifndef OPENMW_MP_PACKETACTORCELLCHANGE_HPP
#define OPENMW_MP_PACKETACTORCELLCHANGE_HPP

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorCellChange : public ActorPacket
    {
    public:
        PacketActorCellChange()
            : ActorPacket(PacketType::ActorCellChange)
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
                    // Header cellId is the source cell. Actor identity cellId is the destination cell.
                    packActorIdentity(ws, actor);
                    packPosition(ws, actor.position);
                    packVelocity(ws, actor.velocity);
                    packActorPresentation(ws, actor);
                    packDynamicStats(ws, actor.dynamicStats);
                    packAnimFlags(ws, actor.animFlags);
                    packAI(ws, actor.ai);
                    ws.write(actor.deathState);
                    ws.write(actor.isDead);
                    ws.write(actor.isInstantDeath);
                    ws.writeString(actor.deathAnimGroup);
                    ws.write(actor.isFollowerCellChange);
                    ws.write(actor.migrationGeneration);
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
                    unpackAI(rs, actor.ai);
                    rs.read(actor.deathState);
                    rs.read(actor.isDead);
                    rs.read(actor.isInstantDeath);
                    actor.deathAnimGroup = rs.readString();
                    rs.read(actor.isFollowerCellChange);
                    rs.read(actor.migrationGeneration);
                }
        }
    };
}

#endif // OPENMW_MP_PACKETACTORCELLCHANGE_HPP
