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

        /// Look up or create the account for username.
        /// Returns the account id.
        int64_t lookupOrCreateAccount(std::string_view username);

        /// Look up or create the character record for accountId.
        /// Returns a filled PlayerRecord.
        PlayerRecord lookupOrCreateCharacter(int64_t accountId,
                                             std::string_view username);

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

    private:
        void exec(const char* sql);
        sqlite3_stmt* prepare(const char* sql);

        sqlite3* mDb = nullptr;
    };

} // namespace mwmp
