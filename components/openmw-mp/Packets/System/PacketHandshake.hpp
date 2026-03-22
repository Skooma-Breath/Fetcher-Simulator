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
        std::string passwordHash;         // SHA-256(password), hex string
        bool        isRegistration = false; // true → create account, false → login

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
            ws.write(isRegistration);

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
            clientVersion  = rs.readString();
            playerName     = rs.readString();
            passwordHash   = rs.readString();
            rs.read(isRegistration);

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
    // Chargen/position data is NOT included here — it arrives via
    // PacketCharacterData after the player selects a character.
    // -----------------------------------------------------------------------
    class PacketHandshakeResponse : public BasePacket
    {
    public:
        bool        accepted        = false;
        uint32_t    assignedGuid    = 0;
        std::string serverVersion;
        std::string rejectReason;

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
    // PacketCharacterList — sent by server immediately after accepting the
    // handshake.  Contains a summary of every character on this account so
    // the client can populate the CharacterSelectDialog.
    // -----------------------------------------------------------------------
    struct CharacterEntry
    {
        std::string name;
        std::string race;
        std::string className;
        std::string lastSeen;  ///< ISO-8601 string or empty
        bool        isNew = true; ///< true → still needs chargen
    };

    class PacketCharacterList : public BasePacket
    {
    public:
        std::vector<CharacterEntry> characters;

        PacketCharacterList() : BasePacket(PacketType::CharacterList) {}

    protected:
        void pack(WriteStream& ws) override
        {
            auto count = static_cast<uint32_t>(characters.size());
            ws.write(count);
            for (const auto& c : characters)
            {
                ws.writeString(c.name);
                ws.writeString(c.race);
                ws.writeString(c.className);
                ws.writeString(c.lastSeen);
                ws.write(c.isNew);
            }
        }

        void unpack(ReadStream& rs) override
        {
            uint32_t count = 0;
            rs.read(count);
            characters.resize(count);
            for (auto& c : characters)
            {
                c.name      = rs.readString();
                c.race      = rs.readString();
                c.className = rs.readString();
                c.lastSeen  = rs.readString();
                rs.read(c.isNew);
            }
        }
    };

    // -----------------------------------------------------------------------
    // PacketCharacterSelect — sent by client when the player picks a character
    // or clicks "New Character".
    // charName == "" means the player wants to create a new character.
    // -----------------------------------------------------------------------
    class PacketCharacterSelect : public BasePacket
    {
    public:
        std::string charName; ///< character name — always non-empty
        bool        isNew = false; ///< true → create this as a new slot

        PacketCharacterSelect() : BasePacket(PacketType::CharacterSelect) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.writeString(charName);
            ws.write(isNew);
        }
        void unpack(ReadStream& rs) override
        {
            charName = rs.readString();
            rs.read(isNew);
        }
    };

    // -----------------------------------------------------------------------
    // PacketCharacterSelectError — server rejects a CharacterSelect request.
    // reason is a human-readable string shown in the dialog.
    // -----------------------------------------------------------------------
    class PacketCharacterSelectError : public BasePacket
    {
    public:
        std::string reason;

        PacketCharacterSelectError() : BasePacket(PacketType::CharacterSelectError) {}

    protected:
        void pack(WriteStream& ws) override  { ws.writeString(reason); }
        void unpack(ReadStream& rs) override { reason = rs.readString(); }
    };

    // -----------------------------------------------------------------------
    // PacketCharacterData — sent by server after it receives PacketCharacterSelect.
    // Contains everything CharacterSelectDialog needs to enter the world or
    // run chargen (mirrors the old extra fields in PacketHandshakeResponse).
    // -----------------------------------------------------------------------
    class PacketCharacterData : public BasePacket
    {
    public:
        bool        isNewCharacter  = true;
        std::string spawnCell;
        float spawnX = 0.f, spawnY = 0.f, spawnZ = 0.f;
        float spawnRotX = 0.f, spawnRotY = 0.f, spawnRotZ = 0.f;

        // Chargen restore — only meaningful when isNewCharacter=false
        std::string race;
        std::string headMesh;
        std::string hairMesh;
        bool        isMale     = true;
        std::string classId;
        std::string className;
        std::string birthSign;
        std::string classData; ///< CLDTstruct as 15 comma-separated ints
        std::string characterName; ///< the character slot name (shown in-world as player name)

        PacketCharacterData() : BasePacket(PacketType::CharacterData) {}

    protected:
        void pack(WriteStream& ws) override
        {
            ws.write(isNewCharacter);
            ws.writeString(spawnCell);
            ws.write(spawnX);    ws.write(spawnY);    ws.write(spawnZ);
            ws.write(spawnRotX); ws.write(spawnRotY); ws.write(spawnRotZ);
            ws.writeString(race);
            ws.writeString(headMesh);
            ws.writeString(hairMesh);
            ws.write(isMale);
            ws.writeString(classId);
            ws.writeString(className);
            ws.writeString(birthSign);
            ws.writeString(classData);
            ws.writeString(characterName);
        }

        void unpack(ReadStream& rs) override
        {
            rs.read(isNewCharacter);
            spawnCell  = rs.readString();
            rs.read(spawnX);    rs.read(spawnY);    rs.read(spawnZ);
            rs.read(spawnRotX); rs.read(spawnRotY); rs.read(spawnRotZ);
            race       = rs.readString();
            headMesh   = rs.readString();
            hairMesh   = rs.readString();
            rs.read(isMale);
            classId    = rs.readString();
            className  = rs.readString();
            birthSign  = rs.readString();
            classData  = rs.readString();
            characterName = rs.readString();
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
