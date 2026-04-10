#ifndef OPENMW_MP_PACKETLUASTORAGE_HPP
#define OPENMW_MP_PACKETLUASTORAGE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include <components/lua/serialization.hpp>

#include "../BasePacket.hpp"

namespace mwmp
{
    enum class LuaStorageAction : uint8_t
    {
        Snapshot = 0,
        Delta = 1,
        ResetSection = 2,
    };

    struct LuaStorageEntry
    {
        std::string section;
        std::string key;
        LuaUtil::BinaryData value;
    };

    class PacketLuaStorage : public BasePacket
    {
    public:
        PacketLuaStorage()
            : BasePacket(PacketType::PacketLuaStorage)
        {
        }

        LuaStorageAction action = LuaStorageAction::Delta;
        std::string section;
        std::vector<LuaStorageEntry> entries;

    protected:
        void pack(WriteStream& ws) override
        {
            const uint8_t wireAction = static_cast<uint8_t>(action);
            ws.write(wireAction);
            ws.writeString(section);

            const auto count = static_cast<uint32_t>(entries.size());
            ws.write(count);
            for (const auto& entry : entries)
            {
                ws.writeString(entry.section);
                ws.writeString(entry.key);
                ws.writeBytes(entry.value);
            }
        }

        void unpack(ReadStream& rs) override
        {
            uint8_t wireAction = 0;
            rs.read(wireAction);
            action = static_cast<LuaStorageAction>(wireAction);
            section = rs.readString();

            uint32_t count = 0;
            rs.read(count);
            entries.clear();
            entries.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                LuaStorageEntry entry;
                entry.section = rs.readString();
                entry.key = rs.readString();
                entry.value = rs.readBytes();
                entries.push_back(std::move(entry));
            }
        }
    };
}

#endif // OPENMW_MP_PACKETLUASTORAGE_HPP
