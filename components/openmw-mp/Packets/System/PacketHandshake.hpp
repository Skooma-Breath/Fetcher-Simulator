#ifndef OPENMW_MP_PACKETSYSTEMHANDSHAKE_HPP
#define OPENMW_MP_PACKETSYSTEMHANDSHAKE_HPP

#include <components/openmw-mp/Packets/BasePacket.hpp>
#include <string>
#include <vector>

namespace mwmp
{
    // -----------------------------------------------------------------------
    // Handshake — first packet the client sends after GNS connection.
    // Contains version info + plugin checksums so the server can validate
    // compatibility before accepting the client into the game world.
    // -----------------------------------------------------------------------
    class PacketHandshake : public BasePacket
    {
    public:
        // Client fills these before encoding
        std::string clientVersion;        // e.g. "0.1.0"
        std::string playerName;
        std::string passwordHash;         // SHA-256(password+salt), hex string

        struct PluginEntry
        {
            std::string filename;
            uint32_t    crc32 = 0;
        };
        std::vector<PluginEntry> plugins;

        PacketHandshake() : BasePacket(PacketType::Handshake) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.writeString(clientVersion);
            ws.writeString(playerName);
            ws.writeString(passwordHash);

            auto count = static_cast<uint32_t>(plugins.size());
            ws.write(count);
            for (const auto& p : plugins)
            {
                ws.writeString(p.filename);
                ws.write(p.crc32);
            }
        }

        void unpack(ReadStream& rs) override
        {
            clientVersion = rs.readString();
            playerName    = rs.readString();
            passwordHash  = rs.readString();

            uint32_t count = 0;
            rs.read(count);
            plugins.resize(count);
            for (auto& p : plugins)
            {
                p.filename = rs.readString();
                rs.read(p.crc32);
            }
        }
    };

    // -----------------------------------------------------------------------
    // HandshakeResponse — server reply to Handshake.
    // On success: accepted=true, guid assigned, server version returned.
    // On failure: accepted=false, reason string set.
    // -----------------------------------------------------------------------
    class PacketHandshakeResponse : public BasePacket
    {
    public:
        bool        accepted        = false;
        uint32_t    assignedGuid    = 0;
        std::string serverVersion;
        std::string rejectReason;
        bool        isNewCharacter  = true;  ///< true → client must run chargen
        std::string spawnCell;               ///< cell to spawn in (empty = default)

        // Chargen restore — only meaningful when isNewCharacter=false
        std::string race;
        std::string headMesh;
        std::string hairMesh;
        bool        isMale     = true;
        std::string classId;
        std::string className;
        std::string birthSign;
        std::string classData; ///< CLDTstruct encoded as comma-separated ints
        float spawnX = 0.f, spawnY = 0.f, spawnZ = 0.f;       ///< saved position (0 for new chars)
        float spawnRotX = 0.f, spawnRotY = 0.f, spawnRotZ = 0.f; ///< saved rotation

        struct PluginMismatch
        {
            std::string filename;
            uint32_t    serverCrc = 0;
        };
        std::vector<PluginMismatch> pluginMismatches;

        PacketHandshakeResponse() : BasePacket(PacketType::HandshakeResponse) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(accepted);
            ws.write(assignedGuid);
            ws.writeString(serverVersion);
            ws.writeString(rejectReason);
            ws.write(isNewCharacter);
            ws.writeString(spawnCell);
            ws.writeString(race);
            ws.writeString(headMesh);
            ws.writeString(hairMesh);
            ws.write(isMale);
            ws.writeString(classId);
            ws.writeString(className);
            ws.writeString(birthSign);
            ws.writeString(classData);
            ws.write(spawnX);    ws.write(spawnY);    ws.write(spawnZ);
            ws.write(spawnRotX); ws.write(spawnRotY); ws.write(spawnRotZ);

            auto count = static_cast<uint32_t>(pluginMismatches.size());
            ws.write(count);
            for (const auto& m : pluginMismatches)
            {
                ws.writeString(m.filename);
                ws.write(m.serverCrc);
            }
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(accepted);
            rs.read(assignedGuid);
            serverVersion = rs.readString();
            rejectReason  = rs.readString();
            rs.read(isNewCharacter);
            spawnCell  = rs.readString();
            race       = rs.readString();
            headMesh   = rs.readString();
            hairMesh   = rs.readString();
            rs.read(isMale);
            classId    = rs.readString();
            className  = rs.readString();
            birthSign  = rs.readString();
            classData  = rs.readString();
            rs.read(spawnX);    rs.read(spawnY);    rs.read(spawnZ);
            rs.read(spawnRotX); rs.read(spawnRotY); rs.read(spawnRotZ);

            uint32_t count = 0;
            rs.read(count);
            pluginMismatches.resize(count);
            for (auto& m : pluginMismatches)
            {
                m.filename  = rs.readString();
                rs.read(m.serverCrc);
            }
        }
    };

    // -----------------------------------------------------------------------
    // Disconnect — either side sends this before closing the connection.
    // -----------------------------------------------------------------------
    class PacketDisconnect : public BasePacket
    {
    public:
        uint32_t    guid   = 0;
        std::string reason;

        PacketDisconnect() : BasePacket(PacketType::Disconnect) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(guid);
            ws.writeString(reason);
        }
        void unpack(ReadStream& rs) override
        {
            rs.read(guid);
            reason = rs.readString();
        }
    };

} // namespace mwmp

#endif // OPENMW_MP_PACKETSYSTEMHANDSHAKE_HPP
