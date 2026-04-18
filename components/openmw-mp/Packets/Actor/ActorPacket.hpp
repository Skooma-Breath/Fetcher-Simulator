#ifndef OPENMW_MP_ACTORPACKET_HPP
#define OPENMW_MP_ACTORPACKET_HPP

#include <components/openmw-mp/Base/BaseActor.hpp>
#include <components/openmw-mp/Packets/BasePacket.hpp>

namespace mwmp
{
    class ActorPacket : public BasePacket
    {
    public:
        explicit ActorPacket(PacketType type)
            : BasePacket(type)
            , mActorList(nullptr)
        {
        }

        void setActorList(ActorList* actorList)
        {
            mActorList = actorList;
        }

        ActorList* getActorList() const
        {
            return mActorList;
        }

    protected:
        void packBatchHeader(WriteStream& ws) const
        {
            ws.writeString(mActorList->cellId);
            ws.write(mActorList->isAuthority);
            ws.write(mActorList->authorityGuid);
            ws.write(mActorList->authorityGeneration);
            ws.write(mActorList->snapshotSequence);
            ws.write(mActorList->serverTimestamp);
        }

        void unpackBatchHeader(ReadStream& rs)
        {
            mActorList->cellId = rs.readString();
            rs.read(mActorList->isAuthority);
            rs.read(mActorList->authorityGuid);
            rs.read(mActorList->authorityGeneration);
            rs.read(mActorList->snapshotSequence);
            rs.read(mActorList->serverTimestamp);
        }

        void packActorIdentity(WriteStream& ws, const BaseActor& actor) const
        {
            ws.writeString(actor.refId);
            ws.write(actor.refNum);
            ws.write(actor.mpNum);
            ws.writeString(actor.cellId);
        }

        void unpackActorIdentity(ReadStream& rs, BaseActor& actor)
        {
            actor.refId = rs.readString();
            rs.read(actor.refNum);
            rs.read(actor.mpNum);
            actor.cellId = rs.readString();
        }

        void packPosition(WriteStream& ws, const Position& pos) const
        {
            ws.write(pos.pos[0]);
            ws.write(pos.pos[1]);
            ws.write(pos.pos[2]);
            ws.write(pos.rot[0]);
            ws.write(pos.rot[1]);
            ws.write(pos.rot[2]);
            ws.write(pos.isTeleporting);
        }

        void unpackPosition(ReadStream& rs, Position& pos)
        {
            rs.read(pos.pos[0]);
            rs.read(pos.pos[1]);
            rs.read(pos.pos[2]);
            rs.read(pos.rot[0]);
            rs.read(pos.rot[1]);
            rs.read(pos.rot[2]);
            rs.read(pos.isTeleporting);
        }

        void packVelocity(WriteStream& ws, const Velocity& velocity) const
        {
            ws.write(velocity.linear[0]);
            ws.write(velocity.linear[1]);
            ws.write(velocity.linear[2]);
            ws.write(velocity.angular[0]);
            ws.write(velocity.angular[1]);
            ws.write(velocity.angular[2]);
        }

        void unpackVelocity(ReadStream& rs, Velocity& velocity)
        {
            rs.read(velocity.linear[0]);
            rs.read(velocity.linear[1]);
            rs.read(velocity.linear[2]);
            rs.read(velocity.angular[0]);
            rs.read(velocity.angular[1]);
            rs.read(velocity.angular[2]);
        }

        void packActorPresentation(WriteStream& ws, const BaseActor& actor) const
        {
            ws.write(actor.isMoving);
            ws.write(actor.hasWeaponDrawn);
            ws.write(actor.hasSpellReadied);
            ws.write(actor.isAttackingOrCasting);
        }

        void unpackActorPresentation(ReadStream& rs, BaseActor& actor)
        {
            rs.read(actor.isMoving);
            rs.read(actor.hasWeaponDrawn);
            rs.read(actor.hasSpellReadied);
            rs.read(actor.isAttackingOrCasting);
        }

        void packDynamicStat(WriteStream& ws, const DynamicStat& value) const
        {
            ws.write(value.base);
            ws.write(value.current);
            ws.write(value.mod);
        }

        void unpackDynamicStat(ReadStream& rs, DynamicStat& value)
        {
            rs.read(value.base);
            rs.read(value.current);
            rs.read(value.mod);
        }

        void packDynamicStats(WriteStream& ws, const DynamicStats& stats) const
        {
            packDynamicStat(ws, stats.health);
            packDynamicStat(ws, stats.magicka);
            packDynamicStat(ws, stats.fatigue);
        }

        void unpackDynamicStats(ReadStream& rs, DynamicStats& stats)
        {
            unpackDynamicStat(rs, stats.health);
            unpackDynamicStat(rs, stats.magicka);
            unpackDynamicStat(rs, stats.fatigue);
        }

        void packAnimFlags(WriteStream& ws, const AnimFlags& flags) const
        {
            ws.write(flags.movementFlags);
            ws.write(flags.actionFlags);
            ws.write(flags.animFwd);
            ws.write(flags.animSide);
            ws.write(flags.blockedMoveSpeed);
            ws.write(flags.jumpVz);
            ws.writeString(flags.currentAnimGroup);
        }

        void unpackAnimFlags(ReadStream& rs, AnimFlags& flags)
        {
            rs.read(flags.movementFlags);
            rs.read(flags.actionFlags);
            rs.read(flags.animFwd);
            rs.read(flags.animSide);
            rs.read(flags.blockedMoveSpeed);
            rs.read(flags.jumpVz);
            flags.currentAnimGroup = rs.readString();
        }

        void packAnimPlay(WriteStream& ws, const AnimPlay& animPlay) const
        {
            ws.writeString(animPlay.groupName);
            ws.write(animPlay.priority);
            ws.write(animPlay.loops);
            ws.writeString(animPlay.startKey);
            ws.writeString(animPlay.stopKey);
        }

        void unpackAnimPlay(ReadStream& rs, AnimPlay& animPlay)
        {
            animPlay.groupName = rs.readString();
            rs.read(animPlay.priority);
            rs.read(animPlay.loops);
            animPlay.startKey = rs.readString();
            animPlay.stopKey = rs.readString();
        }

        void packAttack(WriteStream& ws, const Attack& attack) const
        {
            ws.writeString(attack.target);
            ws.write(attack.targetMpNum);
            ws.write(attack.hitPos[0]);
            ws.write(attack.hitPos[1]);
            ws.write(attack.hitPos[2]);
            ws.write(attack.hit);
            ws.write(attack.block);
            ws.write(attack.miss);
            ws.write(attack.pressed);
            ws.write(attack.knocked);
            ws.write(attack.healthDamage);
            ws.write(attack.strength);
            ws.write(attack.damage);
            ws.write(attack.type);
            ws.writeString(attack.attackAnimation);
        }

        void unpackAttack(ReadStream& rs, Attack& attack)
        {
            attack.target = rs.readString();
            rs.read(attack.targetMpNum);
            rs.read(attack.hitPos[0]);
            rs.read(attack.hitPos[1]);
            rs.read(attack.hitPos[2]);
            rs.read(attack.hit);
            rs.read(attack.block);
            rs.read(attack.miss);
            rs.read(attack.pressed);
            rs.read(attack.knocked);
            rs.read(attack.healthDamage);
            rs.read(attack.strength);
            rs.read(attack.damage);
            rs.read(attack.type);
            attack.attackAnimation = rs.readString();
        }

        void packCast(WriteStream& ws, const CastSpell& cast) const
        {
            ws.writeString(cast.spellId);
            ws.write(cast.targetGuid);
            ws.writeString(cast.targetRefId);
            ws.write(cast.success);
            ws.write(cast.release);
            ws.writeString(cast.castAnimation);
        }

        void unpackCast(ReadStream& rs, CastSpell& cast)
        {
            cast.spellId = rs.readString();
            rs.read(cast.targetGuid);
            cast.targetRefId = rs.readString();
            rs.read(cast.success);
            rs.read(cast.release);
            cast.castAnimation = rs.readString();
        }

        void packEquipment(WriteStream& ws, const std::vector<EquipmentItem>& equipment) const
        {
            const auto count = static_cast<uint16_t>(equipment.size());
            ws.write(count);
            for (const auto& entry : equipment)
            {
                ws.write(entry.slot);
                ws.writeString(entry.item.refId);
                ws.write(entry.item.count);
                ws.write(entry.item.charge);
                ws.write(entry.item.enchantmentCharge);
                ws.writeString(entry.item.soul);
            }
        }

        void unpackEquipment(ReadStream& rs, std::vector<EquipmentItem>& equipment)
        {
            uint16_t count = 0;
            rs.read(count);
            equipment.resize(count);
            for (auto& entry : equipment)
            {
                rs.read(entry.slot);
                entry.item.refId = rs.readString();
                rs.read(entry.item.count);
                rs.read(entry.item.charge);
                rs.read(entry.item.enchantmentCharge);
                entry.item.soul = rs.readString();
            }
        }

        void packAI(WriteStream& ws, const BaseActor::AIAction& ai) const
        {
            ws.write(static_cast<uint8_t>(ai.type));
            ws.writeString(ai.targetId);
            ws.write(ai.targetMpNum);
            ws.write(ai.duration);
            ws.write(ai.reset);
        }

        void unpackAI(ReadStream& rs, BaseActor::AIAction& ai)
        {
            uint8_t type = 0;
            rs.read(type);
            ai.type = static_cast<BaseActor::AIAction::Type>(type);
            ai.targetId = rs.readString();
            rs.read(ai.targetMpNum);
            rs.read(ai.duration);
            rs.read(ai.reset);
        }

        ActorList* mActorList;
    };
}

#endif // OPENMW_MP_ACTORPACKET_HPP
