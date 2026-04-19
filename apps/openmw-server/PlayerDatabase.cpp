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
    class_data  TEXT    NOT NULL DEFAULT '',
    nickname    TEXT    NOT NULL DEFAULT '',
    inventory_saved INTEGER NOT NULL DEFAULT 0,
    equipment_saved INTEGER NOT NULL DEFAULT 0
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

CREATE TABLE IF NOT EXISTS world_objects (
    mp_num      INTEGER PRIMARY KEY,
    cell_id     TEXT    NOT NULL,
    ref_id      TEXT    NOT NULL,
    item_count  INTEGER NOT NULL DEFAULT 1,
    pos_x       REAL    NOT NULL DEFAULT 0,
    pos_y       REAL    NOT NULL DEFAULT 0,
    pos_z       REAL    NOT NULL DEFAULT 0,
    rot_x       REAL    NOT NULL DEFAULT 0,
    rot_y       REAL    NOT NULL DEFAULT 0,
    rot_z       REAL    NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_world_objects_cell ON world_objects(cell_id);

CREATE TABLE IF NOT EXISTS world_containers (
    cell_id        TEXT    NOT NULL,
    ref_id         TEXT    NOT NULL,
    ref_num        INTEGER NOT NULL DEFAULT 0,
    mp_num         INTEGER NOT NULL DEFAULT 0,
    has_authority  INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(cell_id, ref_id, ref_num)
);

CREATE TABLE IF NOT EXISTS world_container_items (
    cell_id      TEXT    NOT NULL,
    ref_id       TEXT    NOT NULL,
    ref_num      INTEGER NOT NULL DEFAULT 0,
    item_index   INTEGER NOT NULL,
    item_ref_id  TEXT    NOT NULL,
    item_count   INTEGER NOT NULL DEFAULT 0,
    charge       INTEGER NOT NULL DEFAULT -1,
    PRIMARY KEY(cell_id, ref_id, ref_num, item_index),
    FOREIGN KEY(cell_id, ref_id, ref_num)
        REFERENCES world_containers(cell_id, ref_id, ref_num)
        ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_world_container_items_parent
    ON world_container_items(cell_id, ref_id, ref_num);

CREATE TABLE IF NOT EXISTS world_doors (
    cell_id      TEXT    NOT NULL,
    ref_id       TEXT    NOT NULL,
    ref_num      INTEGER NOT NULL DEFAULT 0,
    mp_num       INTEGER NOT NULL DEFAULT 0,
    is_open      INTEGER NOT NULL DEFAULT 0,
    is_locked    INTEGER NOT NULL DEFAULT 0,
    lock_level   INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(cell_id, ref_id, ref_num)
);

CREATE INDEX IF NOT EXISTS idx_world_doors_cell ON world_doors(cell_id);

CREATE TABLE IF NOT EXISTS character_inventory (
    character_id          INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
    item_index            INTEGER NOT NULL,
    ref_id                TEXT    NOT NULL,
    item_count            INTEGER NOT NULL DEFAULT 0,
    charge                INTEGER NOT NULL DEFAULT -1,
    enchantment_charge    REAL    NOT NULL DEFAULT -1,
    soul                  TEXT    NOT NULL DEFAULT '',
    PRIMARY KEY(character_id, item_index)
);

CREATE TABLE IF NOT EXISTS character_equipment (
    character_id          INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
    slot                  INTEGER NOT NULL,
    ref_id                TEXT    NOT NULL,
    item_count            INTEGER NOT NULL DEFAULT 0,
    charge                INTEGER NOT NULL DEFAULT -1,
    enchantment_charge    REAL    NOT NULL DEFAULT -1,
    soul                  TEXT    NOT NULL DEFAULT '',
    PRIMARY KEY(character_id, slot)
);

CREATE TABLE IF NOT EXISTS character_marks (
    character_id          INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
    mark_name             TEXT    NOT NULL,
    cell                  TEXT    NOT NULL DEFAULT '',
    pos_x                 REAL    NOT NULL DEFAULT 0,
    pos_y                 REAL    NOT NULL DEFAULT 0,
    pos_z                 REAL    NOT NULL DEFAULT 0,
    rot_x                 REAL    NOT NULL DEFAULT 0,
    rot_y                 REAL    NOT NULL DEFAULT 0,
    rot_z                 REAL    NOT NULL DEFAULT 0,
    PRIMARY KEY(character_id, mark_name)
);

CREATE INDEX IF NOT EXISTS idx_character_marks_character
    ON character_marks(character_id);
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
    "ALTER TABLE characters ADD COLUMN nickname TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE characters ADD COLUMN inventory_saved INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE characters ADD COLUMN equipment_saved INTEGER NOT NULL DEFAULT 0",
    "CREATE TABLE IF NOT EXISTS character_inventory ("
    "  character_id INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,"
    "  item_index INTEGER NOT NULL,"
    "  ref_id TEXT NOT NULL,"
    "  item_count INTEGER NOT NULL DEFAULT 0,"
    "  charge INTEGER NOT NULL DEFAULT -1,"
    "  enchantment_charge REAL NOT NULL DEFAULT -1,"
    "  soul TEXT NOT NULL DEFAULT '',"
    "  PRIMARY KEY(character_id, item_index))",
    "CREATE TABLE IF NOT EXISTS character_equipment ("
    "  character_id INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,"
    "  slot INTEGER NOT NULL,"
    "  ref_id TEXT NOT NULL,"
    "  item_count INTEGER NOT NULL DEFAULT 0,"
    "  charge INTEGER NOT NULL DEFAULT -1,"
    "  enchantment_charge REAL NOT NULL DEFAULT -1,"
    "  soul TEXT NOT NULL DEFAULT '',"
    "  PRIMARY KEY(character_id, slot))",
    "CREATE TABLE IF NOT EXISTS character_marks ("
    "  character_id INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,"
    "  mark_name TEXT NOT NULL,"
    "  cell TEXT NOT NULL DEFAULT '',"
    "  pos_x REAL NOT NULL DEFAULT 0,"
    "  pos_y REAL NOT NULL DEFAULT 0,"
    "  pos_z REAL NOT NULL DEFAULT 0,"
    "  rot_x REAL NOT NULL DEFAULT 0,"
    "  rot_y REAL NOT NULL DEFAULT 0,"
    "  rot_z REAL NOT NULL DEFAULT 0,"
    "  PRIMARY KEY(character_id, mark_name))",
    "CREATE INDEX IF NOT EXISTS idx_character_marks_character ON character_marks(character_id)",
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
        " race, head_mesh, hair_mesh, is_male, class_id, class_name, birth_sign, class_data, nickname,"
        " inventory_saved, equipment_saved"
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
    rec.nickname  = col(18);
    rec.hasSavedInventory = sqlite3_column_int(s, 19) != 0;
    rec.hasSavedEquipment = sqlite3_column_int(s, 20) != 0;
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

void PlayerDatabase::setNickname(int64_t characterId, std::string_view nickname)
{
    sqlite3_stmt* s = prepare(
        "UPDATE characters SET nickname=?1 WHERE id=?2");
    sqlite3_bind_text (s, 1, nickname.data(), static_cast<int>(nickname.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, characterId);
    checkSqlite(sqlite3_step(s), mDb, "setNickname");
    sqlite3_finalize(s);
}

std::vector<PlayerMark> PlayerDatabase::loadCharacterMarks(int64_t characterId)
{
    sqlite3_stmt* s = prepare(
        "SELECT mark_name, cell, pos_x, pos_y, pos_z, rot_x, rot_y, rot_z"
        " FROM character_marks WHERE character_id=?1 ORDER BY mark_name");
    sqlite3_bind_int64(s, 1, characterId);

    std::vector<PlayerMark> marks;
    while (sqlite3_step(s) == SQLITE_ROW)
    {
        PlayerMark mark;
        auto col = [&](int i) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
            return t ? t : "";
        };

        mark.name = col(0);
        mark.cell = col(1);
        mark.position.pos[0] = static_cast<float>(sqlite3_column_double(s, 2));
        mark.position.pos[1] = static_cast<float>(sqlite3_column_double(s, 3));
        mark.position.pos[2] = static_cast<float>(sqlite3_column_double(s, 4));
        mark.position.rot[0] = static_cast<float>(sqlite3_column_double(s, 5));
        mark.position.rot[1] = static_cast<float>(sqlite3_column_double(s, 6));
        mark.position.rot[2] = static_cast<float>(sqlite3_column_double(s, 7));
        marks.push_back(std::move(mark));
    }

    sqlite3_finalize(s);
    return marks;
}

void PlayerDatabase::upsertCharacterMark(int64_t characterId, const PlayerMark& mark)
{
    sqlite3_stmt* s = prepare(
        "INSERT INTO character_marks(character_id, mark_name, cell, pos_x, pos_y, pos_z, rot_x, rot_y, rot_z)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"
        " ON CONFLICT(character_id, mark_name) DO UPDATE SET"
        " cell=excluded.cell,"
        " pos_x=excluded.pos_x,"
        " pos_y=excluded.pos_y,"
        " pos_z=excluded.pos_z,"
        " rot_x=excluded.rot_x,"
        " rot_y=excluded.rot_y,"
        " rot_z=excluded.rot_z");
    sqlite3_bind_int64(s, 1, characterId);
    sqlite3_bind_text(s, 2, mark.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, mark.cell.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(s, 4, mark.position.pos[0]);
    sqlite3_bind_double(s, 5, mark.position.pos[1]);
    sqlite3_bind_double(s, 6, mark.position.pos[2]);
    sqlite3_bind_double(s, 7, mark.position.rot[0]);
    sqlite3_bind_double(s, 8, mark.position.rot[1]);
    sqlite3_bind_double(s, 9, mark.position.rot[2]);
    checkSqlite(sqlite3_step(s), mDb, "upsertCharacterMark");
    sqlite3_finalize(s);
}

void PlayerDatabase::deleteCharacterMark(int64_t characterId, std::string_view name)
{
    sqlite3_stmt* s = prepare(
        "DELETE FROM character_marks WHERE character_id=?1 AND mark_name=?2");
    sqlite3_bind_int64(s, 1, characterId);
    sqlite3_bind_text(s, 2, name.data(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    checkSqlite(sqlite3_step(s), mDb, "deleteCharacterMark");
    sqlite3_finalize(s);
}

std::vector<Item> PlayerDatabase::loadCharacterInventory(int64_t characterId)
{
    sqlite3_stmt* s = prepare(
        "SELECT ref_id, item_count, charge, enchantment_charge, soul"
        " FROM character_inventory WHERE character_id=?1 ORDER BY item_index");
    sqlite3_bind_int64(s, 1, characterId);

    std::vector<Item> items;
    while (sqlite3_step(s) == SQLITE_ROW)
    {
        Item item;
        auto col = [&](int i) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
            return t ? t : "";
        };

        item.refId = col(0);
        item.count = sqlite3_column_int(s, 1);
        item.charge = sqlite3_column_int(s, 2);
        item.enchantmentCharge = static_cast<float>(sqlite3_column_double(s, 3));
        item.soul = col(4);
        items.push_back(std::move(item));
    }

    sqlite3_finalize(s);
    return items;
}

void PlayerDatabase::saveCharacterInventory(int64_t characterId, const std::vector<Item>& items)
{
    exec("BEGIN");
    try
    {
        sqlite3_stmt* clear = prepare("DELETE FROM character_inventory WHERE character_id=?1");
        sqlite3_bind_int64(clear, 1, characterId);
        checkSqlite(sqlite3_step(clear), mDb, "clearCharacterInventory");
        sqlite3_finalize(clear);

        sqlite3_stmt* insert = prepare(
            "INSERT INTO character_inventory(character_id, item_index, ref_id, item_count, charge, enchantment_charge, soul)"
            " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");

        for (std::size_t i = 0; i < items.size(); ++i)
        {
            const Item& item = items[i];
            sqlite3_bind_int64(insert, 1, characterId);
            sqlite3_bind_int(insert, 2, static_cast<int>(i));
            sqlite3_bind_text(insert, 3, item.refId.c_str(), static_cast<int>(item.refId.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int(insert, 4, item.count);
            sqlite3_bind_int(insert, 5, item.charge);
            sqlite3_bind_double(insert, 6, item.enchantmentCharge);
            sqlite3_bind_text(insert, 7, item.soul.c_str(), static_cast<int>(item.soul.size()), SQLITE_TRANSIENT);
            checkSqlite(sqlite3_step(insert), mDb, "insertCharacterInventory");
            sqlite3_reset(insert);
            sqlite3_clear_bindings(insert);
        }
        sqlite3_finalize(insert);

        sqlite3_stmt* mark = prepare(
            "UPDATE characters SET inventory_saved=1, last_seen=?1 WHERE id=?2");
        sqlite3_bind_int64(mark, 1, static_cast<int64_t>(std::time(nullptr)));
        sqlite3_bind_int64(mark, 2, characterId);
        checkSqlite(sqlite3_step(mark), mDb, "markCharacterInventorySaved");
        sqlite3_finalize(mark);

        exec("COMMIT");
    }
    catch (...)
    {
        try { exec("ROLLBACK"); } catch (...) {}
        throw;
    }
}

std::vector<EquipmentItem> PlayerDatabase::loadCharacterEquipment(int64_t characterId)
{
    sqlite3_stmt* s = prepare(
        "SELECT slot, ref_id, item_count, charge, enchantment_charge, soul"
        " FROM character_equipment WHERE character_id=?1 ORDER BY slot");
    sqlite3_bind_int64(s, 1, characterId);

    std::vector<EquipmentItem> equipment;
    while (sqlite3_step(s) == SQLITE_ROW)
    {
        EquipmentItem entry;
        auto col = [&](int i) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
            return t ? t : "";
        };

        entry.slot = sqlite3_column_int(s, 0);
        entry.item.refId = col(1);
        entry.item.count = sqlite3_column_int(s, 2);
        entry.item.charge = sqlite3_column_int(s, 3);
        entry.item.enchantmentCharge = static_cast<float>(sqlite3_column_double(s, 4));
        entry.item.soul = col(5);
        equipment.push_back(std::move(entry));
    }

    sqlite3_finalize(s);
    return equipment;
}

void PlayerDatabase::saveCharacterEquipment(int64_t characterId, const std::vector<EquipmentItem>& equipment)
{
    exec("BEGIN");
    try
    {
        sqlite3_stmt* clear = prepare("DELETE FROM character_equipment WHERE character_id=?1");
        sqlite3_bind_int64(clear, 1, characterId);
        checkSqlite(sqlite3_step(clear), mDb, "clearCharacterEquipment");
        sqlite3_finalize(clear);

        sqlite3_stmt* insert = prepare(
            "INSERT INTO character_equipment(character_id, slot, ref_id, item_count, charge, enchantment_charge, soul)"
            " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");

        for (const EquipmentItem& entry : equipment)
        {
            if (entry.item.refId.empty())
                continue;

            sqlite3_bind_int64(insert, 1, characterId);
            sqlite3_bind_int(insert, 2, entry.slot);
            sqlite3_bind_text(insert, 3, entry.item.refId.c_str(), static_cast<int>(entry.item.refId.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int(insert, 4, entry.item.count);
            sqlite3_bind_int(insert, 5, entry.item.charge);
            sqlite3_bind_double(insert, 6, entry.item.enchantmentCharge);
            sqlite3_bind_text(insert, 7, entry.item.soul.c_str(), static_cast<int>(entry.item.soul.size()), SQLITE_TRANSIENT);
            checkSqlite(sqlite3_step(insert), mDb, "insertCharacterEquipment");
            sqlite3_reset(insert);
            sqlite3_clear_bindings(insert);
        }
        sqlite3_finalize(insert);

        sqlite3_stmt* mark = prepare(
            "UPDATE characters SET equipment_saved=1, last_seen=?1 WHERE id=?2");
        sqlite3_bind_int64(mark, 1, static_cast<int64_t>(std::time(nullptr)));
        sqlite3_bind_int64(mark, 2, characterId);
        checkSqlite(sqlite3_step(mark), mDb, "markCharacterEquipmentSaved");
        sqlite3_finalize(mark);

        exec("COMMIT");
    }
    catch (...)
    {
        try { exec("ROLLBACK"); } catch (...) {}
        throw;
    }
}

std::vector<PlacedObject> PlayerDatabase::loadWorldObjects()
{
    sqlite3_stmt* s = prepare(
        "SELECT mp_num, cell_id, ref_id, item_count, pos_x, pos_y, pos_z, rot_x, rot_y, rot_z"
        " FROM world_objects ORDER BY cell_id, mp_num");

    std::vector<PlacedObject> objects;
    while (sqlite3_step(s) == SQLITE_ROW)
    {
        PlacedObject object;
        object.mpNum = static_cast<uint32_t>(sqlite3_column_int64(s, 0));

        auto col = [&](int i) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
            return t ? t : "";
        };

        object.cellId = col(1);
        object.refId = col(2);
        object.count = sqlite3_column_int(s, 3);
        object.position.pos[0] = static_cast<float>(sqlite3_column_double(s, 4));
        object.position.pos[1] = static_cast<float>(sqlite3_column_double(s, 5));
        object.position.pos[2] = static_cast<float>(sqlite3_column_double(s, 6));
        object.position.rot[0] = static_cast<float>(sqlite3_column_double(s, 7));
        object.position.rot[1] = static_cast<float>(sqlite3_column_double(s, 8));
        object.position.rot[2] = static_cast<float>(sqlite3_column_double(s, 9));
        objects.push_back(std::move(object));
    }

    sqlite3_finalize(s);
    return objects;
}

void PlayerDatabase::upsertWorldObject(const PlacedObject& object)
{
    sqlite3_stmt* s = prepare(
        "INSERT INTO world_objects(mp_num, cell_id, ref_id, item_count, pos_x, pos_y, pos_z, rot_x, rot_y, rot_z)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)"
        " ON CONFLICT(mp_num) DO UPDATE SET"
        " cell_id=excluded.cell_id,"
        " ref_id=excluded.ref_id,"
        " item_count=excluded.item_count,"
        " pos_x=excluded.pos_x,"
        " pos_y=excluded.pos_y,"
        " pos_z=excluded.pos_z,"
        " rot_x=excluded.rot_x,"
        " rot_y=excluded.rot_y,"
        " rot_z=excluded.rot_z");

    sqlite3_bind_int64(s, 1, object.mpNum);
    sqlite3_bind_text(s, 2, object.cellId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, object.refId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 4, object.count);
    sqlite3_bind_double(s, 5, object.position.pos[0]);
    sqlite3_bind_double(s, 6, object.position.pos[1]);
    sqlite3_bind_double(s, 7, object.position.pos[2]);
    sqlite3_bind_double(s, 8, object.position.rot[0]);
    sqlite3_bind_double(s, 9, object.position.rot[1]);
    sqlite3_bind_double(s, 10, object.position.rot[2]);
    checkSqlite(sqlite3_step(s), mDb, "upsertWorldObject");
    sqlite3_finalize(s);
}

void PlayerDatabase::deleteWorldObject(uint32_t mpNum)
{
    sqlite3_stmt* s = prepare("DELETE FROM world_objects WHERE mp_num=?1");
    sqlite3_bind_int64(s, 1, mpNum);
    checkSqlite(sqlite3_step(s), mDb, "deleteWorldObject");
    sqlite3_finalize(s);
}

std::vector<ContainerRecord> PlayerDatabase::loadContainerRecords()
{
    sqlite3_stmt* s = prepare(
        "SELECT cell_id, ref_id, ref_num, mp_num, has_authority"
        " FROM world_containers ORDER BY cell_id, ref_id, ref_num");

    std::vector<ContainerRecord> records;
    while (sqlite3_step(s) == SQLITE_ROW)
    {
        ContainerRecord record;
        auto col = [&](int i) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
            return t ? t : "";
        };

        record.cellId = col(0);
        record.refId = col(1);
        record.refNum = static_cast<uint32_t>(sqlite3_column_int64(s, 2));
        record.mpNum = static_cast<uint32_t>(sqlite3_column_int64(s, 3));
        record.hasAuthority = sqlite3_column_int(s, 4) != 0;
        records.push_back(std::move(record));
    }
    sqlite3_finalize(s);

    sqlite3_stmt* items = prepare(
        "SELECT cell_id, ref_id, ref_num, item_ref_id, item_count, charge"
        " FROM world_container_items"
        " ORDER BY cell_id, ref_id, ref_num, item_index");

    while (sqlite3_step(items) == SQLITE_ROW)
    {
        auto col = [&](int i) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(items, i));
            return t ? t : "";
        };

        const std::string cellId = col(0);
        const std::string refId = col(1);
        const uint32_t refNum = static_cast<uint32_t>(sqlite3_column_int64(items, 2));

        for (auto& record : records)
        {
            if (record.cellId == cellId && record.refId == refId && record.refNum == refNum)
            {
                ContainerItem item;
                item.refId = col(3);
                item.count = sqlite3_column_int(items, 4);
                item.charge = sqlite3_column_int(items, 5);
                record.items.push_back(std::move(item));
                break;
            }
        }
    }
    sqlite3_finalize(items);

    return records;
}

void PlayerDatabase::upsertContainerRecord(const ContainerRecord& record)
{
    exec("BEGIN");
    try
    {
        sqlite3_stmt* parent = prepare(
            "INSERT INTO world_containers(cell_id, ref_id, ref_num, mp_num, has_authority)"
            " VALUES(?1, ?2, ?3, ?4, ?5)"
            " ON CONFLICT(cell_id, ref_id, ref_num) DO UPDATE SET"
            " mp_num=excluded.mp_num,"
            " has_authority=excluded.has_authority");
        sqlite3_bind_text(parent, 1, record.cellId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(parent, 2, record.refId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(parent, 3, record.refNum);
        sqlite3_bind_int64(parent, 4, record.mpNum);
        sqlite3_bind_int(parent, 5, record.hasAuthority ? 1 : 0);
        checkSqlite(sqlite3_step(parent), mDb, "upsertContainerRecord(parent)");
        sqlite3_finalize(parent);

        sqlite3_stmt* clearItems = prepare(
            "DELETE FROM world_container_items WHERE cell_id=?1 AND ref_id=?2 AND ref_num=?3");
        sqlite3_bind_text(clearItems, 1, record.cellId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(clearItems, 2, record.refId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(clearItems, 3, record.refNum);
        checkSqlite(sqlite3_step(clearItems), mDb, "upsertContainerRecord(clearItems)");
        sqlite3_finalize(clearItems);

        sqlite3_stmt* itemStmt = prepare(
            "INSERT INTO world_container_items(cell_id, ref_id, ref_num, item_index, item_ref_id, item_count, charge)"
            " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");

        for (std::size_t i = 0; i < record.items.size(); ++i)
        {
            const auto& item = record.items[i];
            sqlite3_bind_text(itemStmt, 1, record.cellId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(itemStmt, 2, record.refId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(itemStmt, 3, record.refNum);
            sqlite3_bind_int(itemStmt, 4, static_cast<int>(i));
            sqlite3_bind_text(itemStmt, 5, item.refId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(itemStmt, 6, item.count);
            sqlite3_bind_int(itemStmt, 7, item.charge);
            checkSqlite(sqlite3_step(itemStmt), mDb, "upsertContainerRecord(item)");
            sqlite3_reset(itemStmt);
            sqlite3_clear_bindings(itemStmt);
        }
        sqlite3_finalize(itemStmt);

        exec("COMMIT");
    }
    catch (...)
    {
        try { exec("ROLLBACK"); } catch (...) {}
        throw;
    }
}

std::vector<DoorEntry> PlayerDatabase::loadDoorStates()
{
    sqlite3_stmt* s = prepare(
        "SELECT cell_id, ref_id, ref_num, mp_num, is_open, is_locked, lock_level"
        " FROM world_doors ORDER BY cell_id, ref_id, ref_num");

    std::vector<DoorEntry> entries;
    while (sqlite3_step(s) == SQLITE_ROW)
    {
        DoorEntry entry;
        auto col = [&](int i) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
            return t ? t : "";
        };

        entry.cellId = col(0);
        entry.refId = col(1);
        entry.refNum = static_cast<uint32_t>(sqlite3_column_int64(s, 2));
        entry.mpNum = static_cast<uint32_t>(sqlite3_column_int64(s, 3));
        entry.isOpen = sqlite3_column_int(s, 4) != 0;
        entry.isLocked = sqlite3_column_int(s, 5) != 0;
        entry.lockLevel = sqlite3_column_int(s, 6);
        entries.push_back(std::move(entry));
    }
    sqlite3_finalize(s);
    return entries;
}

void PlayerDatabase::upsertDoorState(const DoorEntry& entry)
{
    sqlite3_stmt* s = prepare(
        "INSERT INTO world_doors(cell_id, ref_id, ref_num, mp_num, is_open, is_locked, lock_level)"
        " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)"
        " ON CONFLICT(cell_id, ref_id, ref_num) DO UPDATE SET"
        " mp_num=excluded.mp_num,"
        " is_open=excluded.is_open,"
        " is_locked=excluded.is_locked,"
        " lock_level=excluded.lock_level");
    sqlite3_bind_text(s, 1, entry.cellId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, entry.refId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 3, entry.refNum);
    sqlite3_bind_int64(s, 4, entry.mpNum);
    sqlite3_bind_int(s, 5, entry.isOpen ? 1 : 0);
    sqlite3_bind_int(s, 6, entry.isLocked ? 1 : 0);
    sqlite3_bind_int(s, 7, entry.lockLevel);
    checkSqlite(sqlite3_step(s), mDb, "upsertDoorState");
    sqlite3_finalize(s);
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
