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
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT    UNIQUE NOT NULL,
    created_at    INTEGER NOT NULL DEFAULT 0,
    password_hash TEXT    NOT NULL DEFAULT ''
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

-- Ed25519 keypairs — one account may have many registered public keys.
-- Authentication via keypair is an alternative to password auth.
CREATE TABLE IF NOT EXISTS account_keypairs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    public_key  TEXT    NOT NULL UNIQUE,   -- base64-encoded Ed25519 public key (32 bytes)
    label       TEXT    NOT NULL DEFAULT '', -- e.g. "home PC", "laptop"
    created_at  INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_keypairs_account ON account_keypairs(account_id);
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
    // accounts table migrations
    "ALTER TABLE accounts ADD COLUMN password_hash TEXT NOT NULL DEFAULT ''",
    // unique character name per account (safe on existing single-char DBs)
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_chars_unique_name ON characters(account_id, name)",
    // Ed25519 keypairs table
    "CREATE TABLE IF NOT EXISTS account_keypairs ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,"
    "  public_key TEXT NOT NULL UNIQUE,"
    "  label TEXT NOT NULL DEFAULT '',"
    "  created_at INTEGER NOT NULL DEFAULT 0)",
    "CREATE INDEX IF NOT EXISTS idx_keypairs_account ON account_keypairs(account_id)",
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

int64_t PlayerDatabase::lookupAccount(std::string_view username)
{
    sqlite3_stmt* s = prepare("SELECT id FROM accounts WHERE username = ?1");
    sqlite3_bind_text(s, 1, username.data(), static_cast<int>(username.size()), SQLITE_STATIC);
    const int rc = sqlite3_step(s);
    const int64_t id = (rc == SQLITE_ROW) ? sqlite3_column_int64(s, 0) : -1;
    sqlite3_finalize(s);
    return id;
}

int64_t PlayerDatabase::createAccount(std::string_view username)
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

int64_t PlayerDatabase::lookupOrCreateAccount(std::string_view username)
{
    const int64_t id = lookupAccount(username);
    return (id >= 0) ? id : createAccount(username);
}

std::string PlayerDatabase::getPasswordHash(int64_t accountId)
{
    sqlite3_stmt* s = prepare("SELECT password_hash FROM accounts WHERE id = ?1");
    sqlite3_bind_int64(s, 1, accountId);
    std::string hash;
    if (sqlite3_step(s) == SQLITE_ROW)
    {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
        if (t) hash = t;
    }
    sqlite3_finalize(s);
    return hash;
}

void PlayerDatabase::setPasswordHash(int64_t accountId, std::string_view hash)
{
    sqlite3_stmt* s = prepare(
        "UPDATE accounts SET password_hash = ?1 WHERE id = ?2");
    sqlite3_bind_text(s, 1, hash.data(), static_cast<int>(hash.size()), SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, accountId);
    checkSqlite(sqlite3_step(s), mDb, "setPasswordHash");
    sqlite3_finalize(s);
}

std::optional<PlayerRecord> PlayerDatabase::lookupCharacter(int64_t accountId,
                                                              std::string_view charName)
{
    sqlite3_stmt* s = prepare(
        "SELECT id, name, cell, pos_x, pos_y, pos_z, rot_x, rot_y, rot_z, is_new,"
        " race, head_mesh, hair_mesh, is_male, class_id, class_name, birth_sign, class_data"
        " FROM characters WHERE account_id = ?1 AND name = ?2 LIMIT 1");
    sqlite3_bind_int64(s, 1, accountId);
    sqlite3_bind_text (s, 2, charName.data(), static_cast<int>(charName.size()), SQLITE_STATIC);
    const int rc = sqlite3_step(s);
    if (rc != SQLITE_ROW) { sqlite3_finalize(s); return std::nullopt; }

    PlayerRecord rec;
    rec.accountId   = accountId;
    rec.characterId = sqlite3_column_int64(s, 0);
    auto col = [&](int i) -> std::string {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
        return t ? t : "";
    };
    rec.playerName = col(1);
    rec.cell       = col(2);
    rec.posX = static_cast<float>(sqlite3_column_double(s, 3));
    rec.posY = static_cast<float>(sqlite3_column_double(s, 4));
    rec.posZ = static_cast<float>(sqlite3_column_double(s, 5));
    rec.rotX = static_cast<float>(sqlite3_column_double(s, 6));
    rec.rotY = static_cast<float>(sqlite3_column_double(s, 7));
    rec.rotZ = static_cast<float>(sqlite3_column_double(s, 8));
    rec.isNew     = sqlite3_column_int(s, 9) != 0;
    rec.race      = col(10);
    rec.headMesh  = col(11);
    rec.hairMesh  = col(12);
    rec.isMale    = sqlite3_column_int(s, 13) != 0;
    rec.classId   = col(14);
    rec.className = col(15);
    rec.birthSign = col(16);
    rec.classData = col(17);
    sqlite3_finalize(s);
    return rec;
}

PlayerRecord PlayerDatabase::createCharacter(int64_t accountId,
                                              std::string_view charName)
{
    sqlite3_stmt* s = prepare(
        "INSERT INTO characters(account_id, name, is_new, last_seen) VALUES(?1, ?2, 1, ?3)");
    sqlite3_bind_int64(s, 1, accountId);
    sqlite3_bind_text (s, 2, charName.data(), static_cast<int>(charName.size()), SQLITE_STATIC);
    sqlite3_bind_int64(s, 3, static_cast<int64_t>(std::time(nullptr)));
    checkSqlite(sqlite3_step(s), mDb, "insert character");
    sqlite3_finalize(s);

    PlayerRecord rec;
    rec.accountId   = accountId;
    rec.characterId = sqlite3_last_insert_rowid(mDb);
    rec.playerName  = std::string(charName);
    rec.isNew       = true;
    Log(Debug::Info) << "[PlayerDB] new character: '" << charName
                     << "' id=" << rec.characterId
                     << " account=" << accountId;
    return rec;
}

bool PlayerDatabase::characterNameTaken(int64_t accountId, std::string_view charName)
{
    sqlite3_stmt* s = prepare(
        "SELECT 1 FROM characters WHERE account_id = ?1 AND name = ?2 LIMIT 1");
    sqlite3_bind_int64(s, 1, accountId);
    sqlite3_bind_text (s, 2, charName.data(), static_cast<int>(charName.size()), SQLITE_STATIC);
    const bool found = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    return found;
}

PlayerRecord PlayerDatabase::lookupOrCreateCharacter(int64_t accountId,
                                                      std::string_view charName)
{
    auto existing = lookupCharacter(accountId, charName);
    return existing ? *existing : createCharacter(accountId, charName);
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

bool PlayerDatabase::deleteCharacter(int64_t accountId, std::string_view charName)
{
    sqlite3_stmt* s = prepare(
        "DELETE FROM characters WHERE account_id=?1 AND name=?2");
    sqlite3_bind_int64(s,  1, accountId);
    sqlite3_bind_text (s,  2, charName.data(), static_cast<int>(charName.size()), SQLITE_TRANSIENT);
    checkSqlite(sqlite3_step(s), mDb, "deleteCharacter");
    int changes = sqlite3_changes(mDb);
    sqlite3_finalize(s);
    return changes > 0;
}

int64_t PlayerDatabase::addKeypair(int64_t accountId,
                                    std::string_view publicKey,
                                    std::string_view label)
{
    sqlite3_stmt* s = prepare(
        "INSERT INTO account_keypairs(account_id, public_key, label, created_at)"
        " VALUES(?1, ?2, ?3, ?4)");
    sqlite3_bind_int64(s, 1, accountId);
    sqlite3_bind_text (s, 2, publicKey.data(), static_cast<int>(publicKey.size()), SQLITE_STATIC);
    sqlite3_bind_text (s, 3, label.data(),     static_cast<int>(label.size()),     SQLITE_STATIC);
    sqlite3_bind_int64(s, 4, static_cast<int64_t>(std::time(nullptr)));
    checkSqlite(sqlite3_step(s), mDb, "insert keypair");
    sqlite3_finalize(s);
    const int64_t id = sqlite3_last_insert_rowid(mDb);
    Log(Debug::Info) << "[PlayerDB] keypair registered for account=" << accountId
                     << " label='" << label << "'";
    return id;
}

std::vector<PlayerDatabase::KeypairEntry>
PlayerDatabase::listKeypairs(int64_t accountId)
{
    std::vector<KeypairEntry> results;
    sqlite3_stmt* s = prepare(
        "SELECT id, public_key, label FROM account_keypairs WHERE account_id=?1");
    sqlite3_bind_int64(s, 1, accountId);
    while (sqlite3_step(s) == SQLITE_ROW)
    {
        KeypairEntry e;
        e.id = sqlite3_column_int64(s, 0);
        auto col = [&](int i) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
            return t ? t : "";
        };
        e.publicKey = col(1);
        e.label     = col(2);
        results.push_back(std::move(e));
    }
    sqlite3_finalize(s);
    return results;
}

int64_t PlayerDatabase::lookupAccountByKeypair(std::string_view publicKey)
{
    sqlite3_stmt* s = prepare(
        "SELECT account_id FROM account_keypairs WHERE public_key=?1 LIMIT 1");
    sqlite3_bind_text(s, 1, publicKey.data(), static_cast<int>(publicKey.size()), SQLITE_STATIC);
    const int rc = sqlite3_step(s);
    const int64_t id = (rc == SQLITE_ROW) ? sqlite3_column_int64(s, 0) : -1;
    sqlite3_finalize(s);
    return id;
}

std::string PlayerDatabase::getUsernameForAccount(int64_t accountId)
{
    sqlite3_stmt* s = prepare("SELECT username FROM accounts WHERE id=?1 LIMIT 1");
    sqlite3_bind_int64(s, 1, accountId);
    const int rc = sqlite3_step(s);
    std::string name;
    if (rc == SQLITE_ROW)
    {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
        if (t) name = t;
    }
    sqlite3_finalize(s);
    return name;
}

void PlayerDatabase::removeKeypair(std::string_view publicKey)
{
    sqlite3_stmt* s = prepare(
        "DELETE FROM account_keypairs WHERE public_key=?1");
    sqlite3_bind_text(s, 1, publicKey.data(), static_cast<int>(publicKey.size()), SQLITE_STATIC);
    checkSqlite(sqlite3_step(s), mDb, "removeKeypair");
    sqlite3_finalize(s);
}

std::vector<PlayerDatabase::CharacterSummary>
PlayerDatabase::listCharacters(int64_t accountId)
{
    std::vector<CharacterSummary> results;
    sqlite3_stmt* s = prepare(
        "SELECT name, race, class_name, last_seen, is_new "
        "FROM characters WHERE account_id=?1 ORDER BY last_seen DESC");
    sqlite3_bind_int64(s, 1, accountId);
    while (sqlite3_step(s) == SQLITE_ROW)
    {
        CharacterSummary cs;
        auto col = [&](int i) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
            return t ? t : "";
        };
        cs.name      = col(0);
        cs.race      = col(1);
        cs.className = col(2);
        int64_t ts   = sqlite3_column_int64(s, 3);
        cs.lastSeen  = ts ? std::to_string(ts) : "";
        cs.isNew     = sqlite3_column_int(s, 4) != 0;
        results.push_back(std::move(cs));
    }
    sqlite3_finalize(s);
    return results;
}

} // namespace mwmp
