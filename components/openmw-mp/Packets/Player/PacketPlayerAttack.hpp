#ifndef OPENMW_MP_PACKETPLAYERATTACK_HPP
#define OPENMW_MP_PACKETPLAYERATTACK_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PacketPlayerAttack — reliable attack event (melee / bow / thrown).
    //
    // Sent on attack press/release edges, plus authoritative local hit resolution.
    // Uses BasePlayer::attack (Attack struct from BaseStructs.hpp).
    //
    // Server: pure relay — decodes into c.player.attack, rebroadcasts raw
    // bytes to all clients in the same cell.
    //
    // Client receive:
    //   1. Resolve attacker NPC Ptr from guid.
    //   2. Resolve target using Attack::targetKind:
    //      TargetPlayer -> local/remote player by guid
    //      TargetActor  -> spawned actor by mpNum
    //      TargetNone   -> refId-only cosmetic fallback
    //   3. If hit == true, apply the authoritative resolved hit locally.
    //   4. Keep the remote attack animation in sync using the chosen attack type.
    // -----------------------------------------------------------------------
    class PacketPlayerAttack : public PlayerPacket
    {
    public:
        PacketPlayerAttack() : PlayerPacket(PacketType::PlayerAttack) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
            ws.writeString(mPlayer->attack.target);
            ws.write(mPlayer->attack.targetMpNum);
            ws.write(mPlayer->attack.targetKind);
            ws.write(mPlayer->attack.hitPos[0]);
            ws.write(mPlayer->attack.hitPos[1]);
            ws.write(mPlayer->attack.hitPos[2]);
            ws.write(mPlayer->attack.hit);
            ws.write(mPlayer->attack.block);
            ws.write(mPlayer->attack.miss);
            ws.write(mPlayer->attack.pressed);
            ws.write(mPlayer->attack.knocked);
            ws.write(mPlayer->attack.healthDamage);
            ws.write(mPlayer->attack.strength);
            ws.write(mPlayer->attack.damage);
            ws.write(mPlayer->attack.type);
            ws.writeString(mPlayer->attack.attackAnimation);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            mPlayer->attack.target     = rs.readString();
            rs.read(mPlayer->attack.targetMpNum);
            rs.read(mPlayer->attack.targetKind);
            rs.read(mPlayer->attack.hitPos[0]);
            rs.read(mPlayer->attack.hitPos[1]);
            rs.read(mPlayer->attack.hitPos[2]);
            rs.read(mPlayer->attack.hit);
            rs.read(mPlayer->attack.block);
            rs.read(mPlayer->attack.miss);
            rs.read(mPlayer->attack.pressed);
            rs.read(mPlayer->attack.knocked);
            rs.read(mPlayer->attack.healthDamage);
            rs.read(mPlayer->attack.strength);
            rs.read(mPlayer->attack.damage);
            rs.read(mPlayer->attack.type);
            mPlayer->attack.attackAnimation = rs.readString();
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYERATTACK_HPP
