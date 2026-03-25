#ifndef OPENMW_MP_PACKETPLAYERINVENTORY_HPP
#define OPENMW_MP_PACKETPLAYERINVENTORY_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PacketPlayerInventory — cosmetic inventory delta.
    //
    // Allows other clients to see what items a player picks up or drops so
    // their equipment packet stays consistent with their visible inventory.
    // The authoritative inventory is stored server-side in SQLite; this
    // packet is a visual aid only — clients do NOT trust it for game logic.
    //
    // Action::Set   → replace remote NPC ContainerStore entirely
    // Action::Add   → add items to remote NPC ContainerStore
    // Action::Remove→ remove items from remote NPC ContainerStore
    //
    // Server: pure relay to all clients in the same cell.
    // -----------------------------------------------------------------------
    class PacketPlayerInventory : public PlayerPacket
    {
    public:
        PacketPlayerInventory() : PlayerPacket(PacketType::PlayerInventory) {}

    protected:
        void packItem(WriteStream& ws, const Item& item)
        {
            ws.writeString(item.refId);
            ws.write(item.count);
            ws.write(item.charge);
            ws.write(item.enchantmentCharge);
            ws.writeString(item.soul);
        }

        void unpackItem(ReadStream& rs, Item& item)
        {
            item.refId             = rs.readString();
            rs.read(item.count);
            rs.read(item.charge);
            rs.read(item.enchantmentCharge);
            item.soul              = rs.readString();
        }

        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
            auto action = static_cast<uint8_t>(mPlayer->inventoryChanges.action);
            ws.write(action);
            auto count = static_cast<uint16_t>(mPlayer->inventoryChanges.items.size());
            ws.write(count);
            for (const auto& item : mPlayer->inventoryChanges.items)
                packItem(ws, item);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);
            uint8_t action = 0;
            rs.read(action);
            mPlayer->inventoryChanges.action =
                static_cast<BasePlayer::InventoryChanges::Action>(action);
            uint16_t count = 0;
            rs.read(count);
            mPlayer->inventoryChanges.items.resize(count);
            for (auto& item : mPlayer->inventoryChanges.items)
                unpackItem(rs, item);
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYERINVENTORY_HPP
