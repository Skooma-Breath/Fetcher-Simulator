#ifndef OPENMW_MP_PACKETACTOREQUIPMENT_HPP
#define OPENMW_MP_PACKETACTOREQUIPMENT_HPP

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorEquipment : public ActorPacket
    {
    public:
        PacketActorEquipment()
            : ActorPacket(PacketType::ActorEquipment)
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
                packEquipment(ws, actor.equipment);
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
                unpackEquipment(rs, actor.equipment);
            }
        }
    };
}

#endif // OPENMW_MP_PACKETACTOREQUIPMENT_HPP
