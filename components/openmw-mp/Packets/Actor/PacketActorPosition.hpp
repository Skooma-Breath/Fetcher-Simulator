#ifndef OPENMW_MP_PACKETACTORPOSITION_HPP
#define OPENMW_MP_PACKETACTORPOSITION_HPP

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorPosition : public ActorPacket
    {
    public:
        PacketActorPosition()
            : ActorPacket(PacketType::ActorPosition)
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
                // Include movement axes so the remote CC can classify walk/run/strafe
                // animations continuously without a separate ActorAnimFlags packet.
                ws.write(actor.animFlags.animFwd);
                ws.write(actor.animFlags.animSide);
                ws.write(actor.animFlags.movementFlags);
                ws.writeString(actor.animFlags.currentAnimGroup);
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
                rs.read(actor.animFlags.animFwd);
                rs.read(actor.animFlags.animSide);
                rs.read(actor.animFlags.movementFlags);
                actor.animFlags.currentAnimGroup = rs.readString();
            }
        }
    };
}

#endif // OPENMW_MP_PACKETACTORPOSITION_HPP
