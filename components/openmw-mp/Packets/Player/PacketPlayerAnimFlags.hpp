#ifndef OPENMW_MP_PACKETPLAYERANIMFLAGS_HPP
#define OPENMW_MP_PACKETPLAYERANIMFLAGS_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PacketPlayerAnimFlags — per-frame movement / action state bitmask.
    //
    // Sent unreliably at the same cadence as PlayerPosition (~20 Hz).
    // Encodes which CharacterState the local player is in: walk/run/sneak/
    // swim/jump/idle, weapon-drawn, spell-ready, strafing, etc.
    //
    // On the receiving client these flags are applied to the remote NPC's
    // CharacterController via Movement::mPosition / mRotation so the vanilla
    // animation system drives the result naturally — no direct anim calls.
    // -----------------------------------------------------------------------
    class PacketPlayerAnimFlags : public PlayerPacket
    {
    public:
        PacketPlayerAnimFlags() : PlayerPacket(PacketType::PlayerAnimFlags) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
            ws.write(mPlayer->animFlags.movementFlags);
            ws.write(mPlayer->animFlags.actionFlags);
            ws.write(mPlayer->animFlags.movementType);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            rs.read(mPlayer->animFlags.movementFlags);
            rs.read(mPlayer->animFlags.actionFlags);
            rs.read(mPlayer->animFlags.movementType);
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYERANIMFLAGS_HPP
