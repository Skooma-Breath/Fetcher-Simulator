#pragma once

///
/// PlayerDatabase — lightweight SQLite wrapper for the OpenMW-MP game server.
///
/// One database file per server instance (default: playerdata.db next to the binary).
/// All public methods are synchronous and must be called from the server's main thread.
///
/// Schema (auto-created on first open):
///
///   accounts(id INTEGER PK, username TEXT UNIQUE, created_at INTEGER)
///
///   characters(id INTEGER PK,
///              account_id INTEGER REFERENCES accounts(id),
///              name TEXT,
///              cell TEXT,
///              pos_x REAL, pos_y REAL, pos_z REAL,
///              rot_x REAL, rot_y REAL, rot_z REAL,
///              is_new INTEGER DEFAULT 1,    -- 1 until the player completes chargen
///              last_seen INTEGER)
///

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <components/openmw-mp/Base/BasePlayer.hpp>
#include <components/openmw-mp/Base/BaseObject.hpp>
#include <components/openmw-mp/Packets/Object/PacketDoorState.hpp>

#include "PlayerMark.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace mwmp
{
    /// Flat record returned by lookupOrCreateCharacter().
    struct PlayerRecord
    {
        int64_t     accountId    = 0;
        int64_t     characterId  = 0;
        std::string playerName;    ///< same as the login username for now
        std::string cell;
        float       posX = 0.f, posY = 0.f, posZ = 0.f;
        float       rotX = 0.f, rotY = 0.f, rotZ = 0.f;
        bool        isNew = true;  ///< true → client must go through chargen

        // Chargen result — populated once isNew becomes false
        std::string race;
        std::string headMesh;
        std::string hairMesh;
        bool        isMale    = true;
        std::string classId;
        std::string className;
        std::string birthSign;
        std::string classData; ///< CLDTstruct encoded as comma-separated ints
        std::string nickname;  ///< cosmetic display name; empty = use slot name
        bool        hasSavedInventory = false;
        bool        hasSavedEquipment = false;
    };

    class PlayerDatabase
    {
    public:
        /// Open (or create) the database at path.
        /// Throws std::runtime_error on failure.
        explicit PlayerDatabase(std::string_view path);
        ~PlayerDatabase();

        // Non-copyable, movable
        PlayerDatabase(const PlayerDatabase&)            = delete;
        PlayerDatabase& operator=(const PlayerDatabase&) = delete;

        /// Look up account id for username. Returns -1 if not found.
        int64_t lookupAccount(std::string_view username);

        /// Create a new account. Returns the new account id.
        int64_t createAccount(std::string_view username);

        /// Look up or create the account for username.
        /// Returns the account id.
        int64_t lookupOrCreateAccount(std::string_view username);

        /// Get the stored bcrypt password hash for an account (empty = not set).
        std::string getPasswordHash(int64_t accountId);

        /// Set (or update) the bcrypt password hash for an account.
        void setPasswordHash(int64_t accountId, std::string_view hash);

        /// Look up a character by account + name. Returns nullopt if not found.
        std::optional<PlayerRecord> lookupCharacter(int64_t accountId,
                                                    std::string_view charName);

        /// Create a new character slot. Caller must check characterNameTaken first.
        PlayerRecord createCharacter(int64_t accountId, std::string_view charName);

        /// Returns true if (accountId, charName) already exists.
        bool characterNameTaken(int64_t accountId, std::string_view charName);

        /// Legacy convenience — lookup then create if absent. Prefer the above for new code.
        PlayerRecord lookupOrCreateCharacter(int64_t accountId,
                                             std::string_view charName);

        /// Persist the player's last known position/cell.
        void savePosition(int64_t characterId,
                          std::string_view cell,
                          float x, float y, float z,
                          float rx, float ry, float rz);

        /// Persist race/class/birthsign chosen during character creation.
        void saveChargenData(int64_t characterId,
                             const std::string& race,
                             const std::string& headMesh,
                             const std::string& hairMesh,
                             bool isMale,
                             const std::string& classId,
                             const std::string& className,
                             const std::string& birthSign,
                             const std::string& classData);

        /// Mark the character as no longer new (chargen complete).
        void markChargenComplete(int64_t characterId);

        /// Update last_seen timestamp.
        void touch(int64_t characterId);

        /// Delete a character slot by (accountId, charName).
        /// Returns true if a row was actually deleted, false if not found.
        bool deleteCharacter(int64_t accountId, std::string_view charName);

        /// Set (or clear) the nickname for a character slot.
        /// Pass an empty string to revert to the slot name.
        void setNickname(int64_t characterId, std::string_view nickname);

        /// Load the saved named mark/recall locations for a character.
        std::vector<PlayerMark> loadCharacterMarks(int64_t characterId);

        /// Insert or update one named mark/recall location for a character.
        void upsertCharacterMark(int64_t characterId, const PlayerMark& mark);

        /// Delete one named mark/recall location for a character.
        void deleteCharacterMark(int64_t characterId, std::string_view name);

        /// Load the last persisted inventory snapshot for a character.
        std::vector<Item> loadCharacterInventory(int64_t characterId);

        /// Replace the persisted inventory snapshot for a character.
        void saveCharacterInventory(int64_t characterId, const std::vector<Item>& items);

        /// Load the last persisted equipment snapshot for a character.
        std::vector<EquipmentItem> loadCharacterEquipment(int64_t characterId);

        /// Replace the persisted equipment snapshot for a character.
        void saveCharacterEquipment(int64_t characterId, const std::vector<EquipmentItem>& equipment);

        /// Load persisted multiplayer-placed world objects.
        std::vector<PlacedObject> loadWorldObjects();

        /// Insert or update one multiplayer-placed world object.
        void upsertWorldObject(const PlacedObject& object);

        /// Delete one multiplayer-placed world object by mpNum.
        void deleteWorldObject(uint32_t mpNum);

        /// Load server-authoritative container inventories.
        std::vector<ContainerRecord> loadContainerRecords();

        /// Insert or update one server-authoritative container inventory.
        void upsertContainerRecord(const ContainerRecord& record);

        /// Load persisted door states.
        std::vector<DoorEntry> loadDoorStates();

        /// Insert or update one persisted door state entry.
        void upsertDoorState(const DoorEntry& entry);

        /// Lightweight summary used to build PacketCharacterList.
        struct CharacterSummary
        {
            std::string name;
            std::string race;
            std::string className;
            std::string lastSeen; ///< epoch seconds as string, or ""
            bool        isNew = true;
        };

        /// Return all characters for an account (may be empty for brand-new accounts).
        std::vector<CharacterSummary> listCharacters(int64_t accountId);

        // ── Ed25519 keypair management ────────────────────────────────────
        struct KeypairEntry
        {
            int64_t     id         = 0;
            std::string publicKey; ///< base64-encoded Ed25519 public key
            std::string label;
        };

        /// Register a new public key for an account. Returns the new row id.
        /// Throws if the public key is already registered (globally unique).
        int64_t addKeypair(int64_t accountId,
                           std::string_view publicKey,
                           std::string_view label = "");

        /// Returns all keypairs registered for an account.
        std::vector<KeypairEntry> listKeypairs(int64_t accountId);

        /// Look up which account owns a given public key. Returns -1 if not found.
        int64_t lookupAccountByKeypair(std::string_view publicKey);

        /// Get the username for an account id. Returns empty string if not found.
        std::string getUsernameForAccount(int64_t accountId);

        /// Remove a keypair by public key. No-op if not found.
        void removeKeypair(std::string_view publicKey);

    private:
        void exec(const char* sql);
        sqlite3_stmt* prepare(const char* sql);

        sqlite3* mDb = nullptr;
    };

} // namespace mwmp
