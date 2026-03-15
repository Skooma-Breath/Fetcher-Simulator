#ifndef OPENMW_MP_PACKETPLAYERBASEINFO_HPP
#define OPENMW_MP_PACKETPLAYERBASEINFO_HPP

#include "PlayerPacket.hpp"
#include <components/esm/refid.hpp>

namespace mwmp
{
    // Sent by client → server once after handshake, and by server → all clients
    // when a new player joins so everyone can render the character correctly.
    class PacketPlayerBaseInfo : public PlayerPacket
    {
    public:
        PacketPlayerBaseInfo() : PlayerPacket(PacketType::PlayerBaseInfo) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
            ws.writeString(mPlayer->name);
            ws.writeString(mPlayer->race);
            ws.writeString(mPlayer->headMesh);
            ws.writeString(mPlayer->hairMesh);
            ws.write(mPlayer->isMale);
            ws.write(mPlayer->scale);
            // Class
            ws.writeString(mPlayer->charClass.mId.toString());
            ws.writeString(mPlayer->charClass.mName);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            mPlayer->name     = rs.readString();
            mPlayer->race     = rs.readString();
            mPlayer->headMesh = rs.readString();
            mPlayer->hairMesh = rs.readString();
            rs.read(mPlayer->isMale);
            rs.read(mPlayer->scale);
            mPlayer->charClass.mId = ESM::RefId::stringRefId(rs.readString());
            mPlayer->charClass.mName = rs.readString();
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYERBASEINFO_HPP
