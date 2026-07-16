#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

#include "../../openmw-server/PlayerDatabase.hpp"

namespace
{
    struct TemporaryDatabase
    {
        std::filesystem::path path = std::filesystem::temp_directory_path()
            / ("openmw-journal-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                + ".db");

        ~TemporaryDatabase()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
            std::filesystem::remove(path.string() + "-wal", error);
            std::filesystem::remove(path.string() + "-shm", error);
        }
    };

    mwmp::BasePlayer::JournalItem makeEntry(
        std::string quest, std::string infoId, int index, std::string text)
    {
        mwmp::BasePlayer::JournalItem item;
        item.type = mwmp::BasePlayer::JournalItem::Type::Entry;
        item.quest = std::move(quest);
        item.infoId = std::move(infoId);
        item.index = index;
        item.text = std::move(text);
        item.hasTimestamp = true;
        item.daysPassed = 2;
        item.month = 1;
        item.dayOfMonth = 18;
        return item;
    }

    mwmp::BasePlayer::JournalItem makeIndex(std::string quest, int index)
    {
        mwmp::BasePlayer::JournalItem item;
        item.type = mwmp::BasePlayer::JournalItem::Type::Index;
        item.quest = std::move(quest);
        item.index = index;
        return item;
    }

    TEST(PlayerJournalDatabase, PersistsPerCharacterAndMergesSharedSources)
    {
        TemporaryDatabase temporary;
        mwmp::PlayerDatabase database(temporary.path.string());
        const int64_t aliceAccount = database.createAccount("alice");
        const int64_t bobAccount = database.createAccount("bob");
        const auto alice = database.createCharacter(aliceAccount, "Nerevar");
        const auto bob = database.createCharacter(bobAccount, "Hortator");

        database.saveCharacterJournalChanges(alice.characterId,
            { makeEntry("main_quest", "main_quest_10", 10, "Alice started it."),
                makeIndex("main_quest", 10) });
        database.saveCharacterJournalChanges(bob.characterId,
            { makeEntry("main_quest", "main_quest_20", 20, "Bob advanced it."),
                makeIndex("main_quest", 20) });

        const auto personal = database.loadCharacterJournals({ alice.characterId });
        ASSERT_EQ(personal.size(), 2u);
        EXPECT_EQ(personal[0].infoId, "main_quest_10");
        EXPECT_EQ(personal[1].type, mwmp::BasePlayer::JournalItem::Type::Index);
        EXPECT_EQ(personal[1].index, 10);

        const auto shared = database.loadCharacterJournals({ alice.characterId, bob.characterId });
        ASSERT_EQ(shared.size(), 3u);
        EXPECT_EQ(shared[0].infoId, "main_quest_10");
        EXPECT_EQ(shared[1].infoId, "main_quest_20");
        EXPECT_EQ(shared[2].type, mwmp::BasePlayer::JournalItem::Type::Index);
        EXPECT_EQ(shared[2].index, 20);

        const auto identities = database.listJournalCharacterIdentities();
        ASSERT_EQ(identities.size(), 2u);
        EXPECT_EQ(identities[0].accountName, "alice");
        EXPECT_EQ(identities[0].characterName, "Nerevar");
    }
}
