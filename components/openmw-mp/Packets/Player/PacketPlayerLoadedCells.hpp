#ifndef OPENMW_MP_PACKETPLAYERLOADEDCELLS_HPP
#define OPENMW_MP_PACKETPLAYERLOADEDCELLS_HPP

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <components/openmw-mp/Packets/BasePacket.hpp>

namespace mwmp
{
    class PacketPlayerLoadedCells : public BasePacket
    {
    public:
        static constexpr uint16_t MaxWireLoadedCells = 64;

        PacketPlayerLoadedCells()
            : BasePacket(PacketType::PlayerLoadedCells)
        {
        }

        uint32_t sequence = 0;
        std::string activeCellId;
        std::vector<std::string> loadedCellIds;

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(sequence);
            ws.writeString(activeCellId);

            const std::size_t cappedSize = loadedCellIds.size() > MaxWireLoadedCells
                ? MaxWireLoadedCells : loadedCellIds.size();
            const auto count = static_cast<uint16_t>(cappedSize);
            ws.write(count);
            for (std::size_t i = 0; i < cappedSize; ++i)
                ws.writeString(loadedCellIds[i]);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(sequence);
            activeCellId = rs.readString();

            uint16_t count = 0;
            rs.read(count);
            if (count > MaxWireLoadedCells)
                throw std::runtime_error("PacketPlayerLoadedCells: too many cells");

            loadedCellIds.clear();
            loadedCellIds.reserve(count);
            for (uint16_t i = 0; i < count; ++i)
                loadedCellIds.push_back(rs.readString());
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETPLAYERLOADEDCELLS_HPP
