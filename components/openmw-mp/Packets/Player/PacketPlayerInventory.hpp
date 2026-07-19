#ifndef OPENMW_MP_PACKETPLAYERINVENTORY_HPP
#define OPENMW_MP_PACKETPLAYERINVENTORY_HPP

#include "PlayerPacket.hpp"

namespace mwmp
{
    // -----------------------------------------------------------------------
    // PacketPlayerInventory — player inventory sync payload.
    //
    // Clients use this to keep remote players' visible inventory in sync. The
    // server also folds Set/Add/Remove updates into its authoritative per-
    // player inventory mirror before persisting to SQLite and exposing the
    // merged snapshot to server Lua. Clients still do NOT trust remote
    // inventory packets for game logic.
    //
    // Action::Set   → replace remote NPC ContainerStore entirely
    // Action::Add   → add items to remote NPC ContainerStore
    // Action::Remove→ remove items from remote NPC ContainerStore
    //
    // Server: merge into authoritative inventory state, then relay to clients
    // in the same cell.
    // -----------------------------------------------------------------------
    class PacketPlayerInventory : public PlayerPacket
    {
    public:
        PacketPlayerInventory() : PlayerPacket(PacketType::PlayerInventory) {}

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
