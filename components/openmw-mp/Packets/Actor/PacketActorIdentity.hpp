#ifndef OPENMW_MP_PACKETACTORIDENTITY_HPP
#define OPENMW_MP_PACKETACTORIDENTITY_HPP

#include <components/openmw-mp/Base/ActorSyncProtocol.hpp>

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorIdentity : public ActorPacket
    {
    public:
        PacketActorIdentity()
            : ActorPacket(PacketType::ActorIdentity)
        {
        }

        void setIdentityList(ActorIdentityList* identityList)
        {
            mIdentityList = identityList;
        }

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mIdentityList->protocolVersion);
            ws.writeString(mIdentityList->cellId);
            ws.write(mIdentityList->authorityGuid);
            ws.write(mIdentityList->authorityGeneration);
            ws.write(mIdentityList->sequence);
            ws.write(mIdentityList->serverTimestamp);
            ws.write(mIdentityList->completeCellSnapshot);

            const auto count = static_cast<uint16_t>(mIdentityList->actors.size());
            ws.write(count);
            for (const auto& record : mIdentityList->actors)
            {
                ws.write(record.actorNetId);
                    ws.write(record.persistent);
                    ws.write(record.serverSpawned);
                    ws.write(record.removed);
                    ws.write(record.baselineReset);
                    ws.write(record.teleport);
                    ws.write(record.migrationGeneration);
                    ws.write(static_cast<uint8_t>(record.removalReason));

                const BaseActor& actor = record.actor;
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
                ws.writeString(actor.deathAnimGroup);
                ws.write(actor.isFollowerCellChange);
            }
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mIdentityList->protocolVersion);
            mIdentityList->cellId = rs.readString();
            rs.read(mIdentityList->authorityGuid);
            rs.read(mIdentityList->authorityGeneration);
            rs.read(mIdentityList->sequence);
            rs.read(mIdentityList->serverTimestamp);
            rs.read(mIdentityList->completeCellSnapshot);

            uint16_t count = 0;
            rs.read(count);
            mIdentityList->actors.resize(count);
            for (auto& record : mIdentityList->actors)
            {
                rs.read(record.actorNetId);
                    rs.read(record.persistent);
                    rs.read(record.serverSpawned);
                    rs.read(record.removed);
                    rs.read(record.baselineReset);
                    rs.read(record.teleport);
                    rs.read(record.migrationGeneration);
                    uint8_t rawRemovalReason = 0;
                    rs.read(rawRemovalReason);
                    record.removalReason = static_cast<ActorRemovalReason>(rawRemovalReason);

                BaseActor& actor = record.actor;
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
                actor.deathAnimGroup = rs.readString();
                rs.read(actor.isFollowerCellChange);
            }
        }

    private:
        ActorIdentityList* mIdentityList = nullptr;
    };

    class PacketActorIdentityAck : public BasePacket
    {
    public:
        PacketActorIdentityAck()
            : BasePacket(PacketType::ActorIdentityAck)
        {
        }

        void setAck(ActorIdentityAck* ack)
        {
            mAck = ack;
        }

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mAck->protocolVersion);
            ws.writeString(mAck->cellId);
            ws.write(mAck->sequence);

            const auto count = static_cast<uint16_t>(mAck->actorNetIds.size());
            ws.write(count);
            for (ActorInstanceId actorNetId : mAck->actorNetIds)
                ws.write(actorNetId);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mAck->protocolVersion);
            mAck->cellId = rs.readString();
            rs.read(mAck->sequence);

            uint16_t count = 0;
            rs.read(count);
            mAck->actorNetIds.resize(count);
            for (ActorInstanceId& actorNetId : mAck->actorNetIds)
                rs.read(actorNetId);
        }

    private:
        ActorIdentityAck* mAck = nullptr;
    };
}

#endif
