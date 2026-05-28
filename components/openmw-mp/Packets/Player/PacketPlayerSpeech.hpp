#ifndef OPENMW_MP_PACKETPLAYERSPEECH_HPP
#define OPENMW_MP_PACKETPLAYERSPEECH_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    class PacketPlayerSpeech : public PlayerPacket
    {
    public:
        PacketPlayerSpeech()
            : PlayerPacket(PacketType::PlayerSpeech)
        {
        }

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
            ws.writeString(mPlayer->speechSound);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            mPlayer->speechSound = rs.readString();
        }
    };
}

#endif // OPENMW_MP_PACKETPLAYERSPEECH_HPP
