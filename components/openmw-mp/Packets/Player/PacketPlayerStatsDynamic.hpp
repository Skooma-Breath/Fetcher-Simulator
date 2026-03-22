#ifndef OPENMW_MP_PACKETPLAYERSTATSDYNAMIC_HPP
#define OPENMW_MP_PACKETPLAYERSTATSDYNAMIC_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    class PacketPlayerStatsDynamic : public PlayerPacket
    {
    public:
        PacketPlayerStatsDynamic() : PlayerPacket(PacketType::PlayerStatsDynamic) {}

    protected:
        void packStat(WriteStream& ws, const DynamicStat& s)
        {
            ws.write(s.base);
            ws.write(s.current);
            ws.write(s.mod);
        }
        void unpackStat(ReadStream& rs, DynamicStat& s)
        {
            rs.read(s.base);
            rs.read(s.current);
            rs.read(s.mod);
        }

        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
            packStat(ws, mPlayer->dynamicStats.health);
            packStat(ws, mPlayer->dynamicStats.magicka);
            packStat(ws, mPlayer->dynamicStats.fatigue);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            unpackStat(rs, mPlayer->dynamicStats.health);
            unpackStat(rs, mPlayer->dynamicStats.magicka);
            unpackStat(rs, mPlayer->dynamicStats.fatigue);
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYERSTATSDYNAMIC_HPP
