#ifndef OPENMW_MP_PACKETPLAYERCELLCHANGE_HPP
#define OPENMW_MP_PACKETPLAYERCELLCHANGE_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    class PacketPlayerCellChange : public PlayerPacket
    {
    public:
        PacketPlayerCellChange() : PlayerPacket(PacketType::PlayerCellChange) {}

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

#endif // OPENMW_MP_PACKETPLAYERCELLCHANGE_HPP
