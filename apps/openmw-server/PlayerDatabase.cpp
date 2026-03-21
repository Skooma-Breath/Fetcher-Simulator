#include "PlayerDatabase.hpp"

#include <sqlite3.h>
#include <components/debug/debuglog.hpp>

#include <ctime>
#include <stdexcept>
#include <string>

namespace mwmp
{

// ============================================================================
//  Schema
// ============================================================================

static const char* kSchema = R"SQL(
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS accounts (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    username    TEXT    UNIQUE NOT NULL,
    created_at  INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS characters (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    name        TEXT    NOT NULL,
    cell        TEXT    NOT NULL DEFAULT '',
    pos_x       REAL    NOT NULL DEFAULT 0,
    pos_y       REAL    NOT NULL DEFAULT 0,
    pos_z       REAL    NOT NULL DEFAULT 0,
    rot_x       REAL    NOT NULL DEFAULT 0,
    rot_y       REAL    NOT NULL DEFAULT 0,
    rot_z       REAL    NOT NULL DEFAULT 0,
    is_new      INTEGER NOT NULL DEFAULT 1,
    last_seen   INTEGER NOT NULL DEFAULT 0,
    race        TEXT    NOT NULL DEFAULT '',
    head_mesh   TEXT    NOT NULL DEFAULT '',
    hair_mesh   TEXT    NOT NULL DEFAULT '',
    is_male     INTEGER NOT NULL DEFAULT 1,
    class_id    TEXT    NOT NULL DEFAULT '',
    class_name  TEXT    NOT NULL DEFAULT '',
    birth_sign  TEXT    NOT NULL DEFAULT '',
    class_data  TEXT    NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_chars_account ON characters(account_id);
)SQL";

// Migration: add chargen columns to databases created before they existed.
static const char* kMigrations[] = {
    "ALTER TABLE characters ADD COLUMN race       TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE characters ADD COLUMN head_mesh  TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE characters ADD COLUMN hair_mesh  TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE characters ADD COLUMN is_male    INTEGER NOT NULL DEFAULT 1",
    "ALTER TABLE characters ADD COLUMN class_id   TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE characters ADD COLUMN class_name TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE characters ADD COLUMN birth_sign TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE characters ADD COLUMN class_data TEXT NOT NULL DEFAULT ''",
};

// ============================================================================
//  Helpers
// ============================================================================

namespace
{
    void checkSqlite(int rc, sqlite3* db, const char* op)
    {
        if (rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE)
            throw std::runtime_error(
                std::string("[PlayerDB] ") + op + ": " + sqlite3_errmsg(db));
    }
}

// ============================================================================
//  Constructor / Destructor
// ============================================================================

PlayerDatabase::PlayerDatabase(std::string_view path)
{
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (const int rc = sqlite3_open_v2(std::string(path).c_str(), &mDb, flags, nullptr);
        rc != SQLITE_OK)
    {
        const std::string msg = mDb ? sqlite3_errmsg(mDb) : "unknown error";
        sqlite3_close(mDb);
        mDb = nullptr;
        throw std::runtime_error("[PlayerDB] open '" + std::string(path) + "': " + msg);
    }

    exec(kSchema);

    // Run migrations — ALTER TABLE errors on "duplicate column name" for columns
    // that already exist; we ignore those errors so this is idempotent.
    for (const char* sql : kMigrations)
    {
        char* errmsg = nullptr;
        sqlite3_exec(mDb, sql, nullptr, nullptr, &errmsg);
        if (errmsg) sqlite3_free(errmsg); // ignore — column may already exist
    }

    Log(Debug::Info) << "[PlayerDB] opened: " << path;
}

PlayerDatabase::~PlayerDatabase()
{
    if (mDb)
    {
        sqlite3_close_v2(mDb);
        mDb = nullptr;
    }
}

// ============================================================================
//  Private helpers
// ============================================================================

void PlayerDatabase::exec(const char* sql)
{
    char* errmsg = nullptr;
    const int rc = sqlite3_exec(mDb, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK)
    {
        std::string msg = errmsg ? errmsg : "unknown";
        sqlite3_free(errmsg);
        throw std::runtime_error(std::string("[PlayerDB] exec: ") + msg);
    }
}

sqlite3_stmt* PlayerDatabase::prepare(const char* sql)
{
    sqlite3_stmt* stmt = nullptr;
    checkSqlite(sqlite3_prepare_v2(mDb, sql, -1, &stmt, nullptr), mDb, "prepare");
    return stmt;
}

// ============================================================================
//  Public API
// ============================================================================

int64_t PlayerDatabase::lookupOrCreateAccount(std::string_view username)
{
    // Try lookup first
    {
        sqlite3_stmt* s = prepare("SELECT id FROM accounts WHERE username = ?1");
        sqlite3_bind_text(s, 1, username.data(), static_cast<int>(username.size()), SQLITE_STATIC);
        const int rc = sqlite3_step(s);
        if (rc == SQLITE_ROW)
        {
            const int64_t id = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
            return id;
        }
        sqlite3_finalize(s);
    }

    // Create
    {
        sqlite3_stmt* s = prepare(
            "INSERT INTO accounts(username, created_at) VALUES(?1, ?2)");
        sqlite3_bind_text(s, 1, username.data(), static_cast<int>(username.size()), SQLITE_STATIC);
        sqlite3_bind_int64(s, 2, static_cast<int64_t>(std::time(nullptr)));
        checkSqlite(sqlite3_step(s), mDb, "insert account");
        sqlite3_finalize(s);

        const int64_t id = sqlite3_last_insert_rowid(mDb);
        Log(Debug::Info) << "[PlayerDB] new account: '" << username << "' id=" << id;
        return id;
    }
}

PlayerRecord PlayerDatabase::lookupOrCreateCharacter(int64_t accountId,
                                                      std::string_view username)
{
    // Try lookup
    {
        sqlite3_stmt* s = prepare(
            "SELECT id, name, cell, pos_x, pos_y, pos_z, rot_x, rot_y, rot_z, is_new,"
            " race, head_mesh, hair_mesh, is_male, class_id, class_name, birth_sign, class_data"
            " FROM characters WHERE account_id = ?1 ORDER BY id LIMIT 1");
        sqlite3_bind_int64(s, 1, accountId);
        const int rc = sqlite3_step(s);
        if (rc == SQLITE_ROW)
        {
            PlayerRecord rec;
            rec.accountId   = accountId;
            rec.characterId = sqlite3_column_int64(s, 0);
            const char* nm  = reinterpret_cast<const char*>(sqlite3_column_text(s, 1));
            rec.playerName  = nm ? nm : std::string(username);
            const char* cl  = reinterpret_cast<const char*>(sqlite3_column_text(s, 2));
            rec.cell        = cl ? cl : "";
            rec.posX = static_cast<float>(sqlite3_column_double(s, 3));
            rec.posY = static_cast<float>(sqlite3_column_double(s, 4));
            rec.posZ = static_cast<float>(sqlite3_column_double(s, 5));
            rec.rotX = static_cast<float>(sqlite3_column_double(s, 6));
            rec.rotY = static_cast<float>(sqlite3_column_double(s, 7));
            rec.rotZ = static_cast<float>(sqlite3_column_double(s, 8));
            rec.isNew = sqlite3_column_int(s, 9) != 0;
            auto col = [&](int i) -> std::string {
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
                return t ? t : "";
            };
            rec.race      = col(10);
            rec.headMesh  = col(11);
            rec.hairMesh  = col(12);
            rec.isMale    = sqlite3_column_int(s, 13) != 0;
            rec.classId   = col(14);
            rec.className = col(15);
            rec.birthSign = col(16);
            rec.classData = col(17);
            rec.classData = col(17);
            sqlite3_finalize(s);
            return rec;
        }
        sqlite3_finalize(s);
    }

    // Create new character record
    {
        sqlite3_stmt* s = prepare(
            "INSERT INTO characters(account_id, name, is_new, last_seen)"
            " VALUES(?1, ?2, 1, ?3)");
        sqlite3_bind_int64(s, 1, accountId);
        sqlite3_bind_text(s, 2, username.data(), static_cast<int>(username.size()), SQLITE_STATIC);
        sqlite3_bind_int64(s, 3, static_cast<int64_t>(std::time(nullptr)));
        checkSqlite(sqlite3_step(s), mDb, "insert character");
        sqlite3_finalize(s);

        PlayerRecord rec;
        rec.accountId   = accountId;
        rec.characterId = sqlite3_last_insert_rowid(mDb);
        rec.playerName  = std::string(username);
        rec.isNew       = true;
        Log(Debug::Info) << "[PlayerDB] new character: '" << username
                          << "' id=" << rec.characterId;
        return rec;
    }
}

void PlayerDatabase::savePosition(int64_t characterId,
                                   std::string_view cell,
                                   float x, float y, float z,
                                   float rx, float ry, float rz)
{
    sqlite3_stmt* s = prepare(
        "UPDATE characters SET cell=?1, pos_x=?2, pos_y=?3, pos_z=?4,"
        " rot_x=?5, rot_y=?6, rot_z=?7, last_seen=?8 WHERE id=?9");
    sqlite3_bind_text(s, 1, cell.data(), static_cast<int>(cell.size()), SQLITE_STATIC);
    sqlite3_bind_double(s, 2, x);
    sqlite3_bind_double(s, 3, y);
    sqlite3_bind_double(s, 4, z);
    sqlite3_bind_double(s, 5, rx);
    sqlite3_bind_double(s, 6, ry);
    sqlite3_bind_double(s, 7, rz);
    sqlite3_bind_int64(s, 8, static_cast<int64_t>(std::time(nullptr)));
    sqlite3_bind_int64(s, 9, characterId);
    checkSqlite(sqlite3_step(s), mDb, "savePosition");
    sqlite3_finalize(s);
}

void PlayerDatabase::saveChargenData(int64_t characterId,
                                      const std::string& race,
                                      const std::string& headMesh,
                                      const std::string& hairMesh,
                                      bool isMale,
                                      const std::string& classId,
                                      const std::string& className,
                                      const std::string& birthSign,
                                      const std::string& classData)
{
    sqlite3_stmt* s = prepare(
        "UPDATE characters SET race=?1, head_mesh=?2, hair_mesh=?3, is_male=?4,"
        " class_id=?5, class_name=?6, birth_sign=?7, class_data=?8 WHERE id=?9");
    sqlite3_bind_text(s, 1, race.c_str(),      -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, headMesh.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, hairMesh.c_str(),  -1, SQLITE_STATIC);
    sqlite3_bind_int (s, 4, isMale ? 1 : 0);
    sqlite3_bind_text(s, 5, classId.c_str(),   -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 6, className.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 7, birthSign.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 8, classData.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 9, characterId);
    checkSqlite(sqlite3_step(s), mDb, "saveChargenData");
    sqlite3_finalize(s);
    Log(Debug::Info) << "[PlayerDB] chargen data saved for char id=" << characterId
                     << " race=" << race << " class=" << classId
                     << " birthSign=" << birthSign;
}

void PlayerDatabase::markChargenComplete(int64_t characterId)
{
    sqlite3_stmt* s = prepare(
        "UPDATE characters SET is_new=0, last_seen=?1 WHERE id=?2");
    sqlite3_bind_int64(s, 1, static_cast<int64_t>(std::time(nullptr)));
    sqlite3_bind_int64(s, 2, characterId);
    checkSqlite(sqlite3_step(s), mDb, "markChargenComplete");
    sqlite3_finalize(s);
    Log(Debug::Info) << "[PlayerDB] chargen complete for char id=" << characterId;
}

void PlayerDatabase::touch(int64_t characterId)
{
    sqlite3_stmt* s = prepare(
        "UPDATE characters SET last_seen=?1 WHERE id=?2");
    sqlite3_bind_int64(s, 1, static_cast<int64_t>(std::time(nullptr)));
    sqlite3_bind_int64(s, 2, characterId);
    checkSqlite(sqlite3_step(s), mDb, "touch");
    sqlite3_finalize(s);
}

} // namespace mwmp
