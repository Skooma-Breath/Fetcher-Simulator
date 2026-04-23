#ifndef OPENMW_MP_PACKETRECORDDYNAMIC_HPP
#define OPENMW_MP_PACKETRECORDDYNAMIC_HPP

#include <cstdint>
#include <string>
#include <vector>

#include <components/openmw-mp/Base/DynamicRecord.hpp>
#include <components/openmw-mp/Packets/BasePacket.hpp>

namespace mwmp
{
    class PacketRecordDynamic : public BasePacket
    {
    public:
        PacketRecordDynamic()
            : BasePacket(PacketType::RecordDynamic)
        {
        }

        DynamicRecordAction action = DynamicRecordAction::Upsert;
        std::string recordType;
        std::vector<DynamicRecordEntry> entries;

    protected:
        void pack(WriteStream& ws) override
        {
            const uint8_t wireAction = static_cast<uint8_t>(action);
            ws.write(wireAction);
            ws.writeString(recordType);

            const auto count = static_cast<uint32_t>(entries.size());
            ws.write(count);
            for (const auto& entry : entries)
            {
                ws.writeString(entry.recordId);
                ws.writeBytes(entry.data);
            }
        }

        void unpack(ReadStream& rs) override
        {
            uint8_t wireAction = 0;
            rs.read(wireAction);
            action = static_cast<DynamicRecordAction>(wireAction);
            recordType = rs.readString();

            uint32_t count = 0;
            rs.read(count);
            entries.clear();
            entries.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                DynamicRecordEntry entry;
                entry.recordId = rs.readString();
                entry.data = rs.readBytes();
                entries.push_back(std::move(entry));
            }
        }
    };
}

#endif // OPENMW_MP_PACKETRECORDDYNAMIC_HPP
