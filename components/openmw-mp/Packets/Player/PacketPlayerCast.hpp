#ifndef OPENMW_MP_PACKETPLAYERCAST_HPP
#define OPENMW_MP_PACKETPLAYERCAST_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PacketPlayerCast — reliable spell cast event.
    //
    // Uses BasePlayer::castSpell (CastSpell struct from BaseStructs.hpp).
    //
    // Server: pure relay — rebroadcasts to all clients in the same cell.
    //
    // Client receive:
    //   Apply spell effects to the target NPC via MWMechanics.
    //   Projectile visuals: play the correct animation group on the caster NPC.
    //   targetGuid != 0  → remote player target (look up in PlayerList)
    //   targetGuid == 0  → world object target (look up by targetRefId)
    // -----------------------------------------------------------------------
    class PacketPlayerCast : public PlayerPacket
    {
    public:
        PacketPlayerCast() : PlayerPacket(PacketType::PlayerCast) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
            ws.writeString(mPlayer->castSpell.spellId);
            ws.write(mPlayer->castSpell.targetGuid);
            ws.writeString(mPlayer->castSpell.targetRefId);
            ws.write(mPlayer->castSpell.success);
            ws.writeString(mPlayer->castSpell.castAnimation);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            mPlayer->castSpell.spellId     = rs.readString();
            rs.read(mPlayer->castSpell.targetGuid);
            mPlayer->castSpell.targetRefId = rs.readString();
            rs.read(mPlayer->castSpell.success);
            mPlayer->castSpell.castAnimation = rs.readString();
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYERCAST_HPP
