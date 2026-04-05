#ifndef OPENMW_MP_PACKETPLAYERRESURRECT_HPP
#define OPENMW_MP_PACKETPLAYERRESURRECT_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    class PacketPlayerResurrect : public PlayerPacket
    {
    public:
        PacketPlayerResurrect() : PlayerPacket(PacketType::PlayerResurrect) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
        }
    };
} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYERRESURRECT_HPP
