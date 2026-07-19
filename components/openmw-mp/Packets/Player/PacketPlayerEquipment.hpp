#ifndef OPENMW_MP_PACKETPLAYEREQUIPMENT_HPP
#define OPENMW_MP_PACKETPLAYEREQUIPMENT_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    class PacketPlayerEquipment : public PlayerPacket
    {
    public:
        PacketPlayerEquipment() : PlayerPacket(PacketType::PlayerEquipment) {}

    protected:
        void packItem(WriteStream& ws, const Item& item)
        {
            ws.write(item.instanceId);
            ws.writeString(item.refId);
            ws.write(item.count);
            ws.write(item.charge);
            ws.write(item.enchantmentCharge);
            ws.writeString(item.soul);
        }
        void unpackItem(ReadStream& rs, Item& item)
        {
            rs.read(item.instanceId);
            item.refId = rs.readString();
            rs.read(item.count);
            rs.read(item.charge);
            rs.read(item.enchantmentCharge);
            item.soul = rs.readString();
        }

        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
            auto count = static_cast<uint8_t>(mPlayer->equipment.size());
            ws.write(count);
            for (const auto& slot : mPlayer->equipment)
            {
                ws.write(slot.slot);
                packItem(ws, slot.item);
            }
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            uint8_t count = 0;
            rs.read(count);
            const auto slots = std::min(static_cast<int>(count), BasePlayer::NUM_EQUIPMENT_SLOTS);
            for (int i = 0; i < slots; ++i)
            {
                auto& slot = mPlayer->equipment[i];
                rs.read(slot.slot);
                unpackItem(rs, slot.item);
            }
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYEREQUIPMENT_HPP
