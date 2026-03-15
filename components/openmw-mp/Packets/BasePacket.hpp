#ifndef OPENMW_MP_BASEPACKET_HPP
#define OPENMW_MP_BASEPACKET_HPP

#include <cstdint>
#include <vector>

#include <components/openmw-mp/NetworkMessages.hpp>
#include <components/openmw-mp/Packets/Serialization.hpp>

namespace mwmp
{
    // -----------------------------------------------------------------------
    // Wire header prepended to every packet.
    // Total: 2 + 4 + 4 = 10 bytes
    // -----------------------------------------------------------------------
    struct PacketHeader
    {
        uint16_t type     = 0;   // PacketType cast to uint16_t
        uint32_t payloadSize = 0;
        uint32_t sequence = 0;   // monotonic counter; aids debugging

        static constexpr size_t WIRE_SIZE = sizeof(type) + sizeof(payloadSize) + sizeof(sequence);

        void serialize(WriteStream& ws) const
        {
            ws.write(type);
            ws.write(payloadSize);
            ws.write(sequence);
        }

        bool deserialize(ReadStream& rs)
        {
            try {
                rs.read(type);
                rs.read(payloadSize);
                rs.read(sequence);
                return true;
            } catch (...) {
                return false;
            }
        }
    };

    // -----------------------------------------------------------------------
    // BasePacket — every concrete packet type inherits from this.
    //
    // Subclasses override:
    //   void pack(WriteStream&)   — write their fields
    //   void unpack(ReadStream&)  — read their fields
    //
    // encode() / decode() handle the full wire format including header.
    // -----------------------------------------------------------------------
    class BasePacket
    {
    public:
        explicit BasePacket(PacketType type) : mType(type) {}
        virtual ~BasePacket() = default;

        PacketType  getType()     const { return mType; }
        uint32_t    getSequence() const { return mHeader.sequence; }

        // Encode to a complete wire buffer (header + payload).
        std::vector<uint8_t> encode(uint32_t sequence = 0)
        {
            WriteStream payload;
            pack(payload);

            auto payloadBytes = payload.take();

            mHeader.type        = static_cast<uint16_t>(mType);
            mHeader.payloadSize = static_cast<uint32_t>(payloadBytes.size());
            mHeader.sequence    = sequence;

            WriteStream ws;
            mHeader.serialize(ws);
            const auto& hdr = ws.buffer();

            std::vector<uint8_t> wire;
            wire.reserve(hdr.size() + payloadBytes.size());
            wire.insert(wire.end(), hdr.begin(), hdr.end());
            wire.insert(wire.end(), payloadBytes.begin(), payloadBytes.end());
            return wire;
        }

        // Decode from raw bytes (must include header).
        // Returns false on any parse error.
        bool decode(const uint8_t* data, size_t size)
        {
            ReadStream rs(data, size);
            if (!mHeader.deserialize(rs))
                return false;
            if (static_cast<PacketType>(mHeader.type) != mType)
                return false;
            try {
                unpack(rs);
                return true;
            } catch (...) {
                return false;
            }
        }

        bool decode(const std::vector<uint8_t>& buf)
        {
            return decode(buf.data(), buf.size());
        }

        // Peek just the header from a raw buffer to determine packet type.
        static bool peekHeader(const uint8_t* data, size_t size, PacketHeader& out)
        {
            if (size < PacketHeader::WIRE_SIZE)
                return false;
            ReadStream rs(data, size);
            return out.deserialize(rs);
        }

    protected:
        virtual void pack(WriteStream& ws)   = 0;
        virtual void unpack(ReadStream& rs)  = 0;

        PacketType   mType;
        PacketHeader mHeader;
    };

} // namespace mwmp

#endif // OPENMW_MP_BASEPACKET_HPP
