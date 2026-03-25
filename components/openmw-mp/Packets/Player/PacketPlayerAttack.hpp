#ifndef OPENMW_MP_PACKETPLAYERATTACK_HPP
#define OPENMW_MP_PACKETPLAYERATTACK_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PacketPlayerAttack — reliable attack event (melee / bow / thrown).
    //
    // Sent once on attack release (not per-frame).
    // Uses BasePlayer::attack (Attack struct from BaseStructs.hpp).
    //
    // Server: pure relay — decodes into c.player.attack, rebroadcasts raw
    // bytes to all clients in the same cell.
    //
    // Client receive:
    //   1. Resolve attacker NPC Ptr from guid.
    //   2. Resolve target: if targetMpNum != 0 look up remote player NPC;
    //      otherwise look up world object by refId.
    //   3. If hit == true, apply damage via MWMechanics combat functions.
    //   4. Trigger "attack" AnimPlay on the attacker NPC if not already firing.
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
            ws.write(mPlayer->attack.hit);
            ws.write(mPlayer->attack.block);
            ws.write(mPlayer->attack.miss);
            ws.write(mPlayer->attack.pressed);
            ws.write(mPlayer->attack.knocked);
            ws.write(mPlayer->attack.strength);
            ws.write(mPlayer->attack.type);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            mPlayer->attack.target     = rs.readString();
            rs.read(mPlayer->attack.targetMpNum);
            rs.read(mPlayer->attack.hit);
            rs.read(mPlayer->attack.block);
            rs.read(mPlayer->attack.miss);
            rs.read(mPlayer->attack.pressed);
            rs.read(mPlayer->attack.knocked);
            rs.read(mPlayer->attack.strength);
            rs.read(mPlayer->attack.type);
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYERATTACK_HPP
