#ifndef OPENMW_MP_PACKETPLAYERDEATH_HPP
#define OPENMW_MP_PACKETPLAYERDEATH_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    class PacketPlayerDeath : public PlayerPacket
    {
    public:
        PacketPlayerDeath() : PlayerPacket(PacketType::PlayerDeath) {}

        std::string killerRefId;    // empty if environmental / unknown
        uint32_t    killerGuid = 0; // 0 if not another player

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
            ws.writeString(killerRefId);
            ws.write(killerGuid);
        }
        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            killerRefId = rs.readString();
            rs.read(killerGuid);
        }
    };
} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYERDEATH_HPP
