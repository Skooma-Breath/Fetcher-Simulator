#ifndef OPENMW_MP_PACKETACTORATTACKV2_HPP
#define OPENMW_MP_PACKETACTORATTACKV2_HPP

#include <components/openmw-mp/Base/ActorSyncProtocol.hpp>

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorAttackV2 : public ActorPacket
    {
    public:
        PacketActorAttackV2()
            : ActorPacket(PacketType::ActorAttackV2)
        {
        }

        void setAttackList(ActorAttackV2List* attackList)
        {
            mAttackList = attackList;
        }

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mAttackList->protocolVersion);
            ws.writeString(mAttackList->cellId);
            ws.write(mAttackList->authorityGuid);
            ws.write(mAttackList->authorityGeneration);
            ws.write(mAttackList->sequence);
            ws.write(mAttackList->serverTimestamp);

            const auto count = static_cast<uint16_t>(mAttackList->events.size());
            ws.write(count);
            for (const ActorAttackV2Event& event : mAttackList->events)
            {
                ws.write(event.actorNetId);
                ws.write(event.eventId);
                packAttack(ws, event.attack);
            }
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mAttackList->protocolVersion);
            mAttackList->cellId = rs.readString();
            rs.read(mAttackList->authorityGuid);
            rs.read(mAttackList->authorityGeneration);
            rs.read(mAttackList->sequence);
            rs.read(mAttackList->serverTimestamp);

            uint16_t count = 0;
            rs.read(count);
            mAttackList->events.resize(count);
            for (ActorAttackV2Event& event : mAttackList->events)
            {
                rs.read(event.actorNetId);
                rs.read(event.eventId);
                unpackAttack(rs, event.attack);
            }
        }

    private:
        ActorAttackV2List* mAttackList = nullptr;
    };
}

#endif
