#ifndef OPENMW_MP_PACKETPLAYERANIMPLAY_HPP
#define OPENMW_MP_PACKETPLAYERANIMPLAY_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PacketPlayerAnimPlay — reliable one-shot animation trigger.
    //
    // Used for animations that need explicit triggering on remote clients:
    // death animations, hit reactions, spell casts, emotes.
    // Sent reliably — missing one causes a visible glitch.
    //
    // Receiving client calls:
    //   MWBase::World::getAnimation(npcPtr)->play(groupName, priority,
    //       MWRender::Animation::BlendMask_All, false, 1.f,
    //       startKey, stopKey, 1.f, loops)
    // -----------------------------------------------------------------------
    class PacketPlayerAnimPlay : public PlayerPacket
    {
    public:
        PacketPlayerAnimPlay() : PlayerPacket(PacketType::PlayerAnimPlay) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
            ws.writeString(mPlayer->animPlay.groupName);
            ws.write(mPlayer->animPlay.priority);
            ws.write(mPlayer->animPlay.loops);
            ws.writeString(mPlayer->animPlay.startKey);
            ws.writeString(mPlayer->animPlay.stopKey);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            mPlayer->animPlay.groupName = rs.readString();
            rs.read(mPlayer->animPlay.priority);
            rs.read(mPlayer->animPlay.loops);
            mPlayer->animPlay.startKey = rs.readString();
            mPlayer->animPlay.stopKey  = rs.readString();
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYERANIMPLAY_HPP
