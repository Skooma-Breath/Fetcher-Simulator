#ifndef OPENMW_MP_PACKETPLAYERJOURNAL_HPP
#define OPENMW_MP_PACKETPLAYERJOURNAL_HPP

#include <algorithm>
#include <stdexcept>

#include "PlayerPacket.hpp"

namespace mwmp
{
    // Quest journal delta/snapshot. Applying a received packet mutates the
    // journal directly; it never evaluates the MWScript command that produced
    // the original entry.
    class PacketPlayerJournal : public PlayerPacket
    {
    public:
        static constexpr uint16_t MaxItems = 4096;

        PacketPlayerJournal()
            : PlayerPacket(PacketType::PlayerJournal)
        {
        }

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(mPlayer->guid);
            ws.write(static_cast<uint8_t>(mPlayer->journalChanges.action));

            const auto count = static_cast<uint16_t>(
                std::min<std::size_t>(mPlayer->journalChanges.items.size(), MaxItems));
            ws.write(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                const BasePlayer::JournalItem& item = mPlayer->journalChanges.items[i];
                ws.write(static_cast<uint8_t>(item.type));
                ws.writeString(item.quest);
                ws.write(item.index);
                ws.writeString(item.infoId);
                ws.writeString(item.text);
                ws.writeString(item.actorName);
                ws.write(item.hasTimestamp);
                ws.write(item.daysPassed);
                ws.write(item.month);
                ws.write(item.dayOfMonth);
            }
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(mPlayer->guid);

            uint8_t action = 0;
            rs.read(action);
            if (action > static_cast<uint8_t>(BasePlayer::JournalChanges::Action::Append))
                throw std::runtime_error("PacketPlayerJournal: invalid action");
            mPlayer->journalChanges.action = static_cast<BasePlayer::JournalChanges::Action>(action);

            uint16_t count = 0;
            rs.read(count);
            if (count > MaxItems)
                throw std::runtime_error("PacketPlayerJournal: too many items");

            mPlayer->journalChanges.items.resize(count);
            for (BasePlayer::JournalItem& item : mPlayer->journalChanges.items)
            {
                uint8_t type = 0;
                rs.read(type);
                if (type > static_cast<uint8_t>(BasePlayer::JournalItem::Type::Index))
                    throw std::runtime_error("PacketPlayerJournal: invalid item type");
                item.type = static_cast<BasePlayer::JournalItem::Type>(type);
                item.quest = rs.readString();
                rs.read(item.index);
                item.infoId = rs.readString();
                item.text = rs.readString();
                item.actorName = rs.readString();
                rs.read(item.hasTimestamp);
                rs.read(item.daysPassed);
                rs.read(item.month);
                rs.read(item.dayOfMonth);
            }
        }
    };
}

#endif
