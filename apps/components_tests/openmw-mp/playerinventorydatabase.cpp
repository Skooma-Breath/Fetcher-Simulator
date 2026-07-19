#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

#include "../../openmw-server/PlayerDatabase.hpp"

namespace
{
    struct TemporaryInventoryDatabase
    {
        std::filesystem::path path = std::filesystem::temp_directory_path()
            / ("openmw-inventory-test-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");

        ~TemporaryInventoryDatabase()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
            std::filesystem::remove(path.string() + "-wal", error);
            std::filesystem::remove(path.string() + "-shm", error);
        }
    };

    TEST(PlayerInventoryDatabase, PersistsStableInventoryAndEquipmentIdentity)
    {
        TemporaryInventoryDatabase temporary;
        mwmp::PlayerDatabase database(temporary.path.string());
        const int64_t account = database.createAccount("inventory-test");
        const auto character = database.createCharacter(account, "Stack Keeper");

        mwmp::Item item;
        item.instanceId = 7723;
        item.refId = "server_custom_blade";
        item.count = 1;
        item.charge = 42;
        item.enchantmentCharge = 9.f;
        item.soul = "golden saint";
        database.saveCharacterInventory(character.characterId, { item });

        mwmp::EquipmentItem equipment;
        equipment.slot = 0;
        equipment.item = item;
        database.saveCharacterEquipment(character.characterId, { equipment });

        const auto loadedInventory = database.loadCharacterInventory(character.characterId);
        ASSERT_EQ(loadedInventory.size(), 1u);
        EXPECT_EQ(loadedInventory.front().instanceId, 7723u);
        EXPECT_EQ(loadedInventory.front().refId, "server_custom_blade");

        const auto loadedEquipment = database.loadCharacterEquipment(character.characterId);
        ASSERT_EQ(loadedEquipment.size(), 1u);
        EXPECT_EQ(loadedEquipment.front().item.instanceId, 7723u);
    }
}
