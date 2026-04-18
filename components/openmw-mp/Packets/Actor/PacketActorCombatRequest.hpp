#ifndef OPENMW_MP_PACKETACTORCOMBATREQUEST_HPP
#define OPENMW_MP_PACKETACTORCOMBATREQUEST_HPP

#include "ActorPacket.hpp"

namespace mwmp
{
    // Sent by a non-authority client when its local player hits an NPC in a cell
    // owned by a different authority. The server routes this to the authority client
    // so vanilla actorAttacked() runs on the correct machine.
    class PacketActorCombatRequest : public ActorPacket
    {
    public:
        PacketActorCombatRequest()
            : ActorPacket(PacketType::ActorCombatRequest)
        {
        }

    protected:
        void pack(WriteStream& ws) override
        {
            packBatchHeader(ws);
            ws.write(mActorList->victimPlayerGuid);
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
            rs.read(mActorList->victimPlayerGuid);
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

#endif // OPENMW_MP_PACKETACTORCOMBATREQUEST_HPP
