#include <gtest/gtest.h>

#include <components/openmw-mp/Packets/Player/PacketPlayerEquipment.hpp>
#include <components/openmw-mp/Packets/Player/PacketPlayerInventory.hpp>

namespace
{
    mwmp::Item makeItem(uint32_t instanceId, std::string refId, int count)
    {
        mwmp::Item item;
        item.instanceId = instanceId;
        item.refId = std::move(refId);
        item.count = count;
        item.charge = 37;
        item.enchantmentCharge = 12.5f;
        item.soul = "golden saint";
        return item;
    }

    TEST(PlayerInventoryPacket, RoundTripsStableInstanceIdentity)
    {
        mwmp::BasePlayer outgoing;
        outgoing.guid = 42;
        outgoing.inventoryChanges.action = mwmp::BasePlayer::InventoryChanges::Action::Set;
        outgoing.inventoryChanges.items.push_back(makeItem(9001, "server_custom_blade", 1));

        mwmp::PacketPlayerInventory encoder;
        encoder.setPlayer(&outgoing);
        const std::vector<uint8_t> bytes = encoder.encode(7);

        mwmp::BasePlayer incoming;
        mwmp::PacketPlayerInventory decoder;
        decoder.setPlayer(&incoming);
        ASSERT_TRUE(decoder.decode(bytes));
        ASSERT_EQ(incoming.inventoryChanges.items.size(), 1u);
        EXPECT_EQ(incoming.inventoryChanges.items.front().instanceId, 9001u);
        EXPECT_EQ(incoming.inventoryChanges.items.front().refId, "server_custom_blade");
    }

    TEST(PlayerEquipmentPacket, RoundTripsInventoryInstanceIdentity)
    {
        mwmp::BasePlayer outgoing;
        outgoing.guid = 7;
        outgoing.equipment[0].slot = 0;
        outgoing.equipment[0].item = makeItem(1234, "steel_katana", 1);

        mwmp::PacketPlayerEquipment encoder;
        encoder.setPlayer(&outgoing);
        const std::vector<uint8_t> bytes = encoder.encode(3);

        mwmp::BasePlayer incoming;
        mwmp::PacketPlayerEquipment decoder;
        decoder.setPlayer(&incoming);
        ASSERT_TRUE(decoder.decode(bytes));
        EXPECT_EQ(incoming.equipment[0].item.instanceId, 1234u);
        EXPECT_EQ(incoming.equipment[0].item.refId, "steel_katana");
    }
}
