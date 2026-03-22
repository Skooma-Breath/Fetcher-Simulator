#ifndef OPENMW_MP_PACKETCHATMESSAGE_HPP
#define OPENMW_MP_PACKETCHATMESSAGE_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    class PacketChatMessage : public PlayerPacket
    {
    public:
        PacketChatMessage() : PlayerPacket(PacketType::ChatMessage) {}

        // Extra fields beyond BasePlayer
        std::string message;
        std::string channel;  // "" = global, otherwise channel name

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
            ws.writeString(mPlayer->name);
            ws.writeString(message);
            ws.writeString(channel);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            mPlayer->name = rs.readString();
            message       = rs.readString();
            channel       = rs.readString();
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETCHATMESSAGE_HPP
