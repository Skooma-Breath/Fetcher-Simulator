#ifndef OPENMW_MWMP_NETWORK_PROTOCOL_HPP
#define OPENMW_MWMP_NETWORK_PROTOCOL_HPP

#include <cstdint>
#include <chrono>
#include <functional>
#include <unordered_map>
#include <vector>

#include <components/debug/debuglog.hpp>
#include <components/openmw-mp/NetworkMessages.hpp>
#include <components/openmw-mp/Packets/BasePacket.hpp>

namespace mwmp
{
    // -----------------------------------------------------------------------
    // Protocol - sits between NetworkClient (raw bytes) and the game logic.
    //
    // Responsibilities:
    //   - Peek the PacketHeader from each incoming buffer
    //   - Route to the correct registered handler
    //   - Provide helper to wrap an encoded packet for sending
    //
    // Handlers are registered with registerHandler<T>(...).
    // The outbound side is just BasePacket::encode() + NetworkClient::send*.
    // -----------------------------------------------------------------------
    class Protocol
    {
    public:
        using Handler = std::function<void(const uint8_t* data, size_t size)>;

        Protocol() = default;

        // Register a handler for a specific packet type.
        // The handler receives the full raw buffer (header + payload) so it
        // can construct and decode a typed packet.
        void registerHandler(PacketType type, Handler handler)
        {
            mHandlers[static_cast<uint16_t>(type)] = std::move(handler);
        }

        // Called by NetworkClient's message callback with each received buffer.
        void dispatch(const uint8_t* data, size_t size)
        {
            PacketHeader hdr;
            if (!BasePacket::peekHeader(data, size, hdr))
            {
                Log(Debug::Warning) << "[MP:Protocol] Received malformed packet (too short)";
                return;
            }

            Log(Debug::Verbose) << "[MP:Protocol] rx type=" << hdr.type;
            auto it = mHandlers.find(hdr.type);
            if (it != mHandlers.end())
            {
                const auto started = std::chrono::steady_clock::now();
                it->second(data, size);
                const double elapsedMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
                if (elapsedMs >= 8.0)
                {
                    const auto packetType = static_cast<PacketType>(hdr.type);
                    const char* packetName = "Unknown";
                    switch (packetType)
                    {
                        case PacketType::PlayerPosition: packetName = "PlayerPosition"; break;
                        case PacketType::PlayerAnimFlags: packetName = "PlayerAnimFlags"; break;
                        case PacketType::PlayerEquipment: packetName = "PlayerEquipment"; break;
                        case PacketType::PlayerInventory: packetName = "PlayerInventory"; break;
                        case PacketType::ActorList: packetName = "ActorList"; break;
                        case PacketType::ActorIdentity: packetName = "ActorIdentity"; break;
                        case PacketType::ActorPositionV2: packetName = "ActorPositionV2"; break;
                        case PacketType::ActorPresentationV2: packetName = "ActorPresentationV2"; break;
                        default: break;
                    }
                    Log(elapsedMs >= 50.0 ? Debug::Warning : Debug::Info) << "[MPDIAG] Slow packet handler"
                                     << " type=" << hdr.type
                                     << " name=" << packetName
                                     << " bytes=" << size
                                     << " elapsedMs=" << elapsedMs
                                     << " frameBudgets60Hz=" << (elapsedMs / (1000.0 / 60.0));
                }
            }
            else
                Log(Debug::Warning) << "[MP:Protocol] No handler for packet type " << hdr.type;
        }

        // Convenience: encode a packet and return the wire bytes.
        static std::vector<uint8_t> encode(BasePacket& pkt, uint32_t seq = 0)
        {
            return pkt.encode(seq);
        }

    private:
        std::unordered_map<uint16_t, Handler> mHandlers;
    };

} // namespace mwmp

#endif // OPENMW_MWMP_NETWORK_PROTOCOL_HPP
