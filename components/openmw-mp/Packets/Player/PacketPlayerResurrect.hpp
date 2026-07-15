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
            packCellId(ws, mPlayer->cell);
            packPosition(ws, mPlayer->position);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            unpackCellId(rs, mPlayer->cell);
            unpackPosition(rs, mPlayer->position);
        }
    };
} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYERRESURRECT_HPP
