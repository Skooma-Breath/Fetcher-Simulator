#ifndef OPENMW_MP_PACKETACTORPRESENTATIONV2_HPP
#define OPENMW_MP_PACKETACTORPRESENTATIONV2_HPP

#include <components/openmw-mp/Base/ActorSyncProtocol.hpp>

#include "ActorPacket.hpp"

namespace mwmp
{
    class PacketActorPresentationV2 : public ActorPacket
    {
    public:
        PacketActorPresentationV2()
            : ActorPacket(PacketType::ActorPresentationV2)
        {
        }

        void setPresentationList(ActorPresentationV2List* presentationList)
        {
            mPresentationList = presentationList;
        }

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mPresentationList->protocolVersion);
            ws.write(mPresentationList->authorityGuid);
            ws.write(mPresentationList->authorityGeneration);
            ws.write(mPresentationList->sequence);
            ws.write(mPresentationList->serverTimestamp);

            const auto count = static_cast<uint16_t>(mPresentationList->snapshots.size());
            ws.write(count);
            for (const auto& snapshot : mPresentationList->snapshots)
                packSnapshot(ws, snapshot);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPresentationList->protocolVersion);
            rs.read(mPresentationList->authorityGuid);
            rs.read(mPresentationList->authorityGeneration);
            rs.read(mPresentationList->sequence);
            rs.read(mPresentationList->serverTimestamp);

            uint16_t count = 0;
            rs.read(count);
            mPresentationList->snapshots.resize(count);
            for (auto& snapshot : mPresentationList->snapshots)
                unpackSnapshot(rs, snapshot);
        }

    private:
        void packSnapshot(WriteStream& ws, const ActorPresentationSnapshot& snapshot)
        {
            ws.write(snapshot.actorNetId);
            ws.write(snapshot.isMoving);
            ws.write(snapshot.isAttackingOrCasting);
            ws.write(snapshot.hasWeaponDrawn);
            ws.write(snapshot.hasSpellReadied);
            ws.write(snapshot.isDead);
            ws.write(snapshot.movementFlags);
            ws.write(snapshot.animFwd);
            ws.write(snapshot.animSide);
            ws.write(snapshot.presentationFlags);
            ws.writeString(snapshot.currentAnimGroup);
            ws.write(snapshot.currentAnimCompletion);
        }

        void unpackSnapshot(ReadStream& rs, ActorPresentationSnapshot& snapshot)
        {
            rs.read(snapshot.actorNetId);
            rs.read(snapshot.isMoving);
            rs.read(snapshot.isAttackingOrCasting);
            rs.read(snapshot.hasWeaponDrawn);
            rs.read(snapshot.hasSpellReadied);
            rs.read(snapshot.isDead);
            rs.read(snapshot.movementFlags);
            rs.read(snapshot.animFwd);
            rs.read(snapshot.animSide);
            rs.read(snapshot.presentationFlags);
            snapshot.currentAnimGroup = rs.readString();
            rs.read(snapshot.currentAnimCompletion);
        }

        ActorPresentationV2List* mPresentationList = nullptr;
    };
}

#endif
