#include <gtest/gtest.h>

#include <components/openmw-mp/Packets/Player/PacketPlayerJournal.hpp>

namespace
{
    TEST(PlayerJournalPacket, RoundTripsAuthoritativeEntryAndIndex)
    {
        mwmp::BasePlayer outgoing;
        outgoing.guid = 42;
        outgoing.journalChanges.action = mwmp::BasePlayer::JournalChanges::Action::Set;

        mwmp::BasePlayer::JournalItem entry;
        entry.type = mwmp::BasePlayer::JournalItem::Type::Entry;
        entry.quest = "a1_1_findspymaster";
        entry.infoId = "a1_1_findspymaster_10";
        entry.text = "Report to Caius Cosades.";
        entry.actorName = "Sellus Gravius";
        entry.index = 10;
        entry.hasTimestamp = true;
        entry.daysPassed = 3;
        entry.month = 6;
        entry.dayOfMonth = 19;
        outgoing.journalChanges.items.push_back(entry);

        mwmp::BasePlayer::JournalItem index;
        index.type = mwmp::BasePlayer::JournalItem::Type::Index;
        index.quest = entry.quest;
        index.index = 20;
        outgoing.journalChanges.items.push_back(index);

        mwmp::PacketPlayerJournal encoder;
        encoder.setPlayer(&outgoing);
        const std::vector<uint8_t> bytes = encoder.encode(7);

        mwmp::BasePlayer incoming;
        mwmp::PacketPlayerJournal decoder;
        decoder.setPlayer(&incoming);
        ASSERT_TRUE(decoder.decode(bytes));
        EXPECT_EQ(incoming.guid, 42u);
        EXPECT_EQ(incoming.journalChanges.action, mwmp::BasePlayer::JournalChanges::Action::Set);
        ASSERT_EQ(incoming.journalChanges.items.size(), 2u);

        const auto& decodedEntry = incoming.journalChanges.items[0];
        EXPECT_EQ(decodedEntry.type, mwmp::BasePlayer::JournalItem::Type::Entry);
        EXPECT_EQ(decodedEntry.quest, entry.quest);
        EXPECT_EQ(decodedEntry.infoId, entry.infoId);
        EXPECT_EQ(decodedEntry.text, entry.text);
        EXPECT_EQ(decodedEntry.actorName, entry.actorName);
        EXPECT_EQ(decodedEntry.index, entry.index);
        EXPECT_TRUE(decodedEntry.hasTimestamp);
        EXPECT_EQ(decodedEntry.daysPassed, entry.daysPassed);
        EXPECT_EQ(decodedEntry.month, entry.month);
        EXPECT_EQ(decodedEntry.dayOfMonth, entry.dayOfMonth);

        const auto& decodedIndex = incoming.journalChanges.items[1];
        EXPECT_EQ(decodedIndex.type, mwmp::BasePlayer::JournalItem::Type::Index);
        EXPECT_EQ(decodedIndex.quest, index.quest);
        EXPECT_EQ(decodedIndex.index, index.index);
    }
}
