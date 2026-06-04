#include "PlayerDatabase.hpp"

#include <algorithm>
#include <components/debug/debuglog.hpp>
#include <sqlite3.h>

#include <ctime>
#include <limits>
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
    equipment_saved INTEGER NOT NULL DEFAULT 0,
    stats_saved INTEGER NOT NULL DEFAULT 0,
    level INTEGER NOT NULL DEFAULT 1,
    level_progress REAL NOT NULL DEFAULT 0
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

CREATE TABLE IF NOT EXISTS world_spawned_actors (
    mp_num            INTEGER PRIMARY KEY,
    cell_id           TEXT    NOT NULL,
    ref_id            TEXT    NOT NULL,
    ref_num           INTEGER NOT NULL DEFAULT 0,
    persistent        INTEGER NOT NULL DEFAULT 1,
    pos_x             REAL    NOT NULL DEFAULT 0,
    pos_y             REAL    NOT NULL DEFAULT 0,
    pos_z             REAL    NOT NULL DEFAULT 0,
    rot_x             REAL    NOT NULL DEFAULT 0,
    rot_y             REAL    NOT NULL DEFAULT 0,
    rot_z             REAL    NOT NULL DEFAULT 0,
    health_base       REAL    NOT NULL DEFAULT 0,
    health_current    REAL    NOT NULL DEFAULT 0,
    health_mod        REAL    NOT NULL DEFAULT 0,
    magicka_base      REAL    NOT NULL DEFAULT 0,
    magicka_current   REAL    NOT NULL DEFAULT 0,
    magicka_mod       REAL    NOT NULL DEFAULT 0,
    fatigue_base      REAL    NOT NULL DEFAULT 0,
    fatigue_current   REAL    NOT NULL DEFAULT 0,
    fatigue_mod       REAL    NOT NULL DEFAULT 0,
    is_dead           INTEGER NOT NULL DEFAULT 0,
    death_state       INTEGER NOT NULL DEFAULT 0,
    death_anim_group  TEXT    NOT NULL DEFAULT '',
    created_at        INTEGER NOT NULL DEFAULT 0,
    updated_at        INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_world_spawned_actors_cell
    ON world_spawned_actors(cell_id);

CREATE TABLE IF NOT EXISTS world_dead_vanilla_actors (
    ref_id            TEXT    NOT NULL,
    ref_num           INTEGER NOT NULL DEFAULT 0,
    cell_id           TEXT    NOT NULL,
    pos_x             REAL    NOT NULL DEFAULT 0,
    pos_y             REAL    NOT NULL DEFAULT 0,
    pos_z             REAL    NOT NULL DEFAULT 0,
    rot_x             REAL    NOT NULL DEFAULT 0,
    rot_y             REAL    NOT NULL DEFAULT 0,
    rot_z             REAL    NOT NULL DEFAULT 0,
    health_base       REAL    NOT NULL DEFAULT 0,
    health_current    REAL    NOT NULL DEFAULT 0,
    health_mod        REAL    NOT NULL DEFAULT 0,
    magicka_base      REAL    NOT NULL DEFAULT 0,
    magicka_current   REAL    NOT NULL DEFAULT 0,
    magicka_mod       REAL    NOT NULL DEFAULT 0,
    fatigue_base      REAL    NOT NULL DEFAULT 0,
    fatigue_current   REAL    NOT NULL DEFAULT 0,
    fatigue_mod       REAL    NOT NULL DEFAULT 0,
    is_dead           INTEGER NOT NULL DEFAULT 1,
    death_state       INTEGER NOT NULL DEFAULT 0,
    is_instant_death  INTEGER NOT NULL DEFAULT 1,
    death_anim_group  TEXT    NOT NULL DEFAULT '',
    created_at        INTEGER NOT NULL DEFAULT 0,
    updated_at        INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(ref_id, ref_num)
);

CREATE INDEX IF NOT EXISTS idx_world_dead_vanilla_actors_cell
    ON world_dead_vanilla_actors(cell_id);

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

CREATE TABLE IF NOT EXISTS world_metadata (
    key    TEXT    PRIMARY KEY,
    value  INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS world_dynamic_records (
    record_type   TEXT    NOT NULL,
    record_id     TEXT    NOT NULL,
    record_scope  TEXT    NOT NULL DEFAULT 'permanent',
    record_data   BLOB    NOT NULL,
    created_at    INTEGER NOT NULL DEFAULT 0,
    updated_at    INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(record_type, record_id)
);

CREATE INDEX IF NOT EXISTS idx_world_dynamic_records_scope
    ON world_dynamic_records(record_scope);

CREATE TABLE IF NOT EXISTS world_dynamic_record_catalog (
    record_type    TEXT    NOT NULL,
    record_id      TEXT    NOT NULL,
    record_scope   TEXT    NOT NULL DEFAULT 'permanent',
    is_persistent  INTEGER NOT NULL DEFAULT 1,
    created_at     INTEGER NOT NULL DEFAULT 0,
    updated_at     INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(record_type, record_id)
);

CREATE INDEX IF NOT EXISTS idx_world_dynamic_record_catalog_persistent
    ON world_dynamic_record_catalog(is_persistent);

CREATE TABLE IF NOT EXISTS world_dynamic_record_links (
    record_id    TEXT    NOT NULL,
    link_kind    TEXT    NOT NULL,
    owner_a      TEXT    NOT NULL DEFAULT '',
    owner_b      TEXT    NOT NULL DEFAULT '',
    owner_c      TEXT    NOT NULL DEFAULT '',
    owner_index  INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(record_id, link_kind, owner_a, owner_b, owner_c, owner_index)
);

CREATE INDEX IF NOT EXISTS idx_world_dynamic_record_links_record
    ON world_dynamic_record_links(record_id);

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

CREATE TABLE IF NOT EXISTS character_dynamic_stats (
    character_id          INTEGER PRIMARY KEY REFERENCES characters(id) ON DELETE CASCADE,
    health_base           REAL    NOT NULL DEFAULT 0,
    health_current        REAL    NOT NULL DEFAULT 0,
    health_mod            REAL    NOT NULL DEFAULT 0,
    magicka_base          REAL    NOT NULL DEFAULT 0,
    magicka_current       REAL    NOT NULL DEFAULT 0,
    magicka_mod           REAL    NOT NULL DEFAULT 0,
    fatigue_base          REAL    NOT NULL DEFAULT 0,
    fatigue_current       REAL    NOT NULL DEFAULT 0,
    fatigue_mod           REAL    NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS character_attributes (
    character_id          INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
    attribute_index       INTEGER NOT NULL,
    base                  INTEGER NOT NULL DEFAULT 0,
    mod                   REAL    NOT NULL DEFAULT 0,
    damage                REAL    NOT NULL DEFAULT 0,
    PRIMARY KEY(character_id, attribute_index)
);

CREATE TABLE IF NOT EXISTS character_skills (
    character_id          INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
    skill_index           INTEGER NOT NULL,
    base                  REAL    NOT NULL DEFAULT 0,
    mod                   REAL    NOT NULL DEFAULT 0,
    damage                REAL    NOT NULL DEFAULT 0,
    progress              REAL    NOT NULL DEFAULT 0,
    increases             INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(character_id, skill_index)
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

CREATE TABLE IF NOT EXISTS character_lua_storage (
    character_id        INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
    storage_namespace  TEXT    NOT NULL,
    storage_key        TEXT    NOT NULL,
    value              BLOB    NOT NULL,
    updated_at         INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(character_id, storage_namespace, storage_key)
);

CREATE INDEX IF NOT EXISTS idx_character_lua_storage_namespace
    ON character_lua_storage(storage_namespace, storage_key);
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
        "ALTER TABLE characters ADD COLUMN stats_saved INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE characters ADD COLUMN level INTEGER NOT NULL DEFAULT 1",
        "ALTER TABLE characters ADD COLUMN level_progress REAL NOT NULL DEFAULT 0",
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
        "CREATE TABLE IF NOT EXISTS character_dynamic_stats ("
        "  character_id INTEGER PRIMARY KEY REFERENCES characters(id) ON DELETE CASCADE,"
        "  health_base REAL NOT NULL DEFAULT 0,"
        "  health_current REAL NOT NULL DEFAULT 0,"
        "  health_mod REAL NOT NULL DEFAULT 0,"
        "  magicka_base REAL NOT NULL DEFAULT 0,"
        "  magicka_current REAL NOT NULL DEFAULT 0,"
        "  magicka_mod REAL NOT NULL DEFAULT 0,"
        "  fatigue_base REAL NOT NULL DEFAULT 0,"
        "  fatigue_current REAL NOT NULL DEFAULT 0,"
        "  fatigue_mod REAL NOT NULL DEFAULT 0)",
        "CREATE TABLE IF NOT EXISTS character_attributes ("
        "  character_id INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,"
        "  attribute_index INTEGER NOT NULL,"
        "  base INTEGER NOT NULL DEFAULT 0,"
        "  mod REAL NOT NULL DEFAULT 0,"
        "  damage REAL NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(character_id, attribute_index))",
        "CREATE TABLE IF NOT EXISTS character_skills ("
        "  character_id INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,"
        "  skill_index INTEGER NOT NULL,"
        "  base REAL NOT NULL DEFAULT 0,"
        "  mod REAL NOT NULL DEFAULT 0,"
        "  damage REAL NOT NULL DEFAULT 0,"
        "  progress REAL NOT NULL DEFAULT 0,"
        "  increases INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(character_id, skill_index))",
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
        "CREATE TABLE IF NOT EXISTS character_lua_storage ("
        "  character_id INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,"
        "  storage_namespace TEXT NOT NULL,"
        "  storage_key TEXT NOT NULL,"
        "  value BLOB NOT NULL,"
        "  updated_at INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(character_id, storage_namespace, storage_key))",
        "CREATE INDEX IF NOT EXISTS idx_character_lua_storage_namespace"
        " ON character_lua_storage(storage_namespace, storage_key)",
        "CREATE TABLE IF NOT EXISTS world_dynamic_records ("
        "  record_type TEXT NOT NULL,"
        "  record_id TEXT NOT NULL,"
        "  record_scope TEXT NOT NULL DEFAULT 'permanent',"
        "  record_data BLOB NOT NULL,"
        "  created_at INTEGER NOT NULL DEFAULT 0,"
        "  updated_at INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(record_type, record_id))",
        "CREATE INDEX IF NOT EXISTS idx_world_dynamic_records_scope ON world_dynamic_records(record_scope)",
        "CREATE TABLE IF NOT EXISTS world_dynamic_record_catalog ("
        "  record_type TEXT NOT NULL,"
        "  record_id TEXT NOT NULL,"
        "  record_scope TEXT NOT NULL DEFAULT 'permanent',"
        "  is_persistent INTEGER NOT NULL DEFAULT 1,"
        "  created_at INTEGER NOT NULL DEFAULT 0,"
        "  updated_at INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(record_type, record_id))",
        "CREATE INDEX IF NOT EXISTS idx_world_dynamic_record_catalog_persistent"
        " ON world_dynamic_record_catalog(is_persistent)",
        "CREATE TABLE IF NOT EXISTS world_dynamic_record_links ("
        "  record_id TEXT NOT NULL,"
        "  link_kind TEXT NOT NULL,"
        "  owner_a TEXT NOT NULL DEFAULT '',"
        "  owner_b TEXT NOT NULL DEFAULT '',"
        "  owner_c TEXT NOT NULL DEFAULT '',"
        "  owner_index INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(record_id, link_kind, owner_a, owner_b, owner_c, owner_index))",
        "CREATE INDEX IF NOT EXISTS idx_world_dynamic_record_links_record"
        " ON world_dynamic_record_links(record_id)",
        "CREATE TABLE IF NOT EXISTS world_metadata ("
        "  key TEXT PRIMARY KEY,"
        "  value INTEGER NOT NULL DEFAULT 0)",
        "CREATE TABLE IF NOT EXISTS world_spawned_actors ("
        "  mp_num INTEGER PRIMARY KEY,"
        "  cell_id TEXT NOT NULL,"
        "  ref_id TEXT NOT NULL,"
        "  ref_num INTEGER NOT NULL DEFAULT 0,"
        "  persistent INTEGER NOT NULL DEFAULT 1,"
        "  pos_x REAL NOT NULL DEFAULT 0,"
        "  pos_y REAL NOT NULL DEFAULT 0,"
        "  pos_z REAL NOT NULL DEFAULT 0,"
        "  rot_x REAL NOT NULL DEFAULT 0,"
        "  rot_y REAL NOT NULL DEFAULT 0,"
        "  rot_z REAL NOT NULL DEFAULT 0,"
        "  health_base REAL NOT NULL DEFAULT 0,"
        "  health_current REAL NOT NULL DEFAULT 0,"
        "  health_mod REAL NOT NULL DEFAULT 0,"
        "  magicka_base REAL NOT NULL DEFAULT 0,"
        "  magicka_current REAL NOT NULL DEFAULT 0,"
        "  magicka_mod REAL NOT NULL DEFAULT 0,"
        "  fatigue_base REAL NOT NULL DEFAULT 0,"
        "  fatigue_current REAL NOT NULL DEFAULT 0,"
        "  fatigue_mod REAL NOT NULL DEFAULT 0,"
        "  is_dead INTEGER NOT NULL DEFAULT 0,"
        "  death_state INTEGER NOT NULL DEFAULT 0,"
        "  death_anim_group TEXT NOT NULL DEFAULT '',"
        "  created_at INTEGER NOT NULL DEFAULT 0,"
        "  updated_at INTEGER NOT NULL DEFAULT 0)",
        "CREATE INDEX IF NOT EXISTS idx_world_spawned_actors_cell ON world_spawned_actors(cell_id)",
        "CREATE TABLE IF NOT EXISTS world_dead_vanilla_actors ("
        "  ref_id TEXT NOT NULL,"
        "  ref_num INTEGER NOT NULL DEFAULT 0,"
        "  cell_id TEXT NOT NULL,"
        "  pos_x REAL NOT NULL DEFAULT 0,"
        "  pos_y REAL NOT NULL DEFAULT 0,"
        "  pos_z REAL NOT NULL DEFAULT 0,"
        "  rot_x REAL NOT NULL DEFAULT 0,"
        "  rot_y REAL NOT NULL DEFAULT 0,"
        "  rot_z REAL NOT NULL DEFAULT 0,"
        "  health_base REAL NOT NULL DEFAULT 0,"
        "  health_current REAL NOT NULL DEFAULT 0,"
        "  health_mod REAL NOT NULL DEFAULT 0,"
        "  magicka_base REAL NOT NULL DEFAULT 0,"
        "  magicka_current REAL NOT NULL DEFAULT 0,"
        "  magicka_mod REAL NOT NULL DEFAULT 0,"
        "  fatigue_base REAL NOT NULL DEFAULT 0,"
        "  fatigue_current REAL NOT NULL DEFAULT 0,"
        "  fatigue_mod REAL NOT NULL DEFAULT 0,"
        "  is_dead INTEGER NOT NULL DEFAULT 1,"
        "  death_state INTEGER NOT NULL DEFAULT 0,"
        "  is_instant_death INTEGER NOT NULL DEFAULT 1,"
        "  death_anim_group TEXT NOT NULL DEFAULT '',"
        "  created_at INTEGER NOT NULL DEFAULT 0,"
        "  updated_at INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(ref_id, ref_num))",
        "CREATE INDEX IF NOT EXISTS idx_world_dead_vanilla_actors_cell"
        " ON world_dead_vanilla_actors(cell_id)",
    };

    // ============================================================================
    //  Helpers
    // ============================================================================

    namespace
    {
        struct BrowsableTableDef
        {
            const char* name;
            const char* orderBy;
        };

        static const BrowsableTableDef kBrowsableTableDefs[] = {
            { "accounts", "id" },
            { "characters", "id" },
            { "account_keypairs", "id" },
            { "world_objects", "mp_num" },
            { "world_spawned_actors", "cell_id, mp_num" },
            { "world_dead_vanilla_actors", "cell_id, ref_id, ref_num" },
            { "world_containers", "cell_id, ref_id, ref_num" },
            { "world_container_items", "cell_id, ref_id, ref_num, item_index" },
            { "world_doors", "cell_id, ref_id, ref_num" },
            { "world_metadata", "key" },
            { "world_dynamic_records", "created_at, record_type, record_id" },
            { "world_dynamic_record_catalog", "created_at, record_type, record_id" },
            { "world_dynamic_record_links", "record_id, link_kind, owner_a, owner_b, owner_c, owner_index" },
            { "character_inventory", "character_id, item_index" },
            { "character_equipment", "character_id, slot" },
            { "character_dynamic_stats", "character_id" },
            { "character_attributes", "character_id, attribute_index" },
            { "character_skills", "character_id, skill_index" },
            { "character_marks", "character_id, mark_name" },
            { "character_lua_storage", "character_id, storage_namespace, storage_key" },
        };

        void checkSqlite(int rc, sqlite3* db, const char* op)
        {
            if (rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE)
                throw std::runtime_error(std::string("[PlayerDB] ") + op + ": " + sqlite3_errmsg(db));
        }

        void clearDynamicRecordLinksForOwner(sqlite3* db, sqlite3_stmt* stmt, std::string_view linkKind,
            std::string_view ownerA, std::string_view ownerB, std::string_view ownerC)
        {
            sqlite3_bind_text(stmt, 1, linkKind.data(), static_cast<int>(linkKind.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, ownerA.data(), static_cast<int>(ownerA.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, ownerB.data(), static_cast<int>(ownerB.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, ownerC.data(), static_cast<int>(ownerC.size()), SQLITE_TRANSIENT);
            checkSqlite(sqlite3_step(stmt), db, "clearDynamicRecordLinksForOwner");
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }

        void insertDynamicRecordLink(sqlite3* db, sqlite3_stmt* stmt, std::string_view recordId,
            std::string_view linkKind, std::string_view ownerA, std::string_view ownerB, std::string_view ownerC,
            int64_t ownerIndex)
        {
            if (recordId.empty())
                return;

            sqlite3_bind_text(stmt, 1, recordId.data(), static_cast<int>(recordId.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, linkKind.data(), static_cast<int>(linkKind.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, ownerA.data(), static_cast<int>(ownerA.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, ownerB.data(), static_cast<int>(ownerB.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, ownerC.data(), static_cast<int>(ownerC.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 6, ownerIndex);
            checkSqlite(sqlite3_step(stmt), db, "insertDynamicRecordLink");
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }

        const BrowsableTableDef* findBrowsableTableDef(std::string_view tableName)
        {
            for (const BrowsableTableDef& entry : kBrowsableTableDefs)
            {
                if (tableName == entry.name)
                    return &entry;
            }
            return nullptr;
        }

        std::optional<std::string> sqliteColumnToString(sqlite3_stmt* stmt, int index)
        {
            if (sqlite3_column_type(stmt, index) == SQLITE_NULL)
                return std::nullopt;

            const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
            if (!text)
                return std::string();

            return std::string(text, static_cast<std::size_t>(sqlite3_column_bytes(stmt, index)));
        }
    }

    // ============================================================================
    //  Constructor / Destructor
    // ============================================================================

    PlayerDatabase::PlayerDatabase(std::string_view path)
    {
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        if (const int rc = sqlite3_open_v2(std::string(path).c_str(), &mDb, flags, nullptr); rc != SQLITE_OK)
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
            if (errmsg)
                sqlite3_free(errmsg); // ignore — column may already exist
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
        sqlite3_stmt* s = prepare("INSERT INTO accounts(username, created_at) VALUES(?1, ?2)");
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
            if (t)
                hash = t;
        }
        sqlite3_finalize(s);
        return hash;
    }

    void PlayerDatabase::setPasswordHash(int64_t accountId, std::string_view hash)
    {
        sqlite3_stmt* s = prepare("UPDATE accounts SET password_hash = ?1 WHERE id = ?2");
        sqlite3_bind_text(s, 1, hash.data(), static_cast<int>(hash.size()), SQLITE_STATIC);
        sqlite3_bind_int64(s, 2, accountId);
        checkSqlite(sqlite3_step(s), mDb, "setPasswordHash");
        sqlite3_finalize(s);
    }

    std::optional<PlayerRecord> PlayerDatabase::lookupCharacter(int64_t accountId, std::string_view charName)
    {
        sqlite3_stmt* s = prepare(
            "SELECT id, name, cell, pos_x, pos_y, pos_z, rot_x, rot_y, rot_z, is_new,"
            " race, head_mesh, hair_mesh, is_male, class_id, class_name, birth_sign, class_data, nickname,"
            " inventory_saved, equipment_saved, stats_saved"
            " FROM characters WHERE account_id = ?1 AND name = ?2 LIMIT 1");
        sqlite3_bind_int64(s, 1, accountId);
        sqlite3_bind_text(s, 2, charName.data(), static_cast<int>(charName.size()), SQLITE_STATIC);
        const int rc = sqlite3_step(s);
        if (rc != SQLITE_ROW)
        {
            sqlite3_finalize(s);
            return std::nullopt;
        }

        PlayerRecord rec;
        rec.accountId = accountId;
        rec.characterId = sqlite3_column_int64(s, 0);
        auto col = [&](int i) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
            return t ? t : "";
        };
        rec.playerName = col(1);
        rec.cell = col(2);
        rec.posX = static_cast<float>(sqlite3_column_double(s, 3));
        rec.posY = static_cast<float>(sqlite3_column_double(s, 4));
        rec.posZ = static_cast<float>(sqlite3_column_double(s, 5));
        rec.rotX = static_cast<float>(sqlite3_column_double(s, 6));
        rec.rotY = static_cast<float>(sqlite3_column_double(s, 7));
        rec.rotZ = static_cast<float>(sqlite3_column_double(s, 8));
        rec.isNew = sqlite3_column_int(s, 9) != 0;
        rec.race = col(10);
        rec.headMesh = col(11);
        rec.hairMesh = col(12);
        rec.isMale = sqlite3_column_int(s, 13) != 0;
        rec.classId = col(14);
        rec.className = col(15);
        rec.birthSign = col(16);
        rec.classData = col(17);
        rec.nickname = col(18);
        rec.hasSavedInventory = sqlite3_column_int(s, 19) != 0;
        rec.hasSavedEquipment = sqlite3_column_int(s, 20) != 0;
        rec.hasSavedStats = sqlite3_column_int(s, 21) != 0;
        sqlite3_finalize(s);
        return rec;
    }

    PlayerRecord PlayerDatabase::createCharacter(int64_t accountId, std::string_view charName)
    {
        sqlite3_stmt* s = prepare("INSERT INTO characters(account_id, name, is_new, last_seen) VALUES(?1, ?2, 1, ?3)");
        sqlite3_bind_int64(s, 1, accountId);
        sqlite3_bind_text(s, 2, charName.data(), static_cast<int>(charName.size()), SQLITE_STATIC);
        sqlite3_bind_int64(s, 3, static_cast<int64_t>(std::time(nullptr)));
        checkSqlite(sqlite3_step(s), mDb, "insert character");
        sqlite3_finalize(s);

        PlayerRecord rec;
        rec.accountId = accountId;
        rec.characterId = sqlite3_last_insert_rowid(mDb);
        rec.playerName = std::string(charName);
        rec.isNew = true;
        Log(Debug::Info) << "[PlayerDB] new character: '" << charName << "' id=" << rec.characterId
                         << " account=" << accountId;
        return rec;
    }

    bool PlayerDatabase::characterNameTaken(int64_t accountId, std::string_view charName)
    {
        sqlite3_stmt* s = prepare("SELECT 1 FROM characters WHERE account_id = ?1 AND name = ?2 LIMIT 1");
        sqlite3_bind_int64(s, 1, accountId);
        sqlite3_bind_text(s, 2, charName.data(), static_cast<int>(charName.size()), SQLITE_STATIC);
        const bool found = sqlite3_step(s) == SQLITE_ROW;
        sqlite3_finalize(s);
        return found;
    }

    PlayerRecord PlayerDatabase::lookupOrCreateCharacter(int64_t accountId, std::string_view charName)
    {
        auto existing = lookupCharacter(accountId, charName);
        return existing ? *existing : createCharacter(accountId, charName);
    }

    void PlayerDatabase::savePosition(
        int64_t characterId, std::string_view cell, float x, float y, float z, float rx, float ry, float rz)
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

    void PlayerDatabase::saveChargenData(int64_t characterId, const std::string& race, const std::string& headMesh,
        const std::string& hairMesh, bool isMale, const std::string& classId, const std::string& className,
        const std::string& birthSign, const std::string& classData)
    {
        sqlite3_stmt* s = prepare(
            "UPDATE characters SET race=?1, head_mesh=?2, hair_mesh=?3, is_male=?4,"
            " class_id=?5, class_name=?6, birth_sign=?7, class_data=?8 WHERE id=?9");
        sqlite3_bind_text(s, 1, race.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, headMesh.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 3, hairMesh.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(s, 4, isMale ? 1 : 0);
        sqlite3_bind_text(s, 5, classId.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 6, className.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 7, birthSign.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 8, classData.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(s, 9, characterId);
        checkSqlite(sqlite3_step(s), mDb, "saveChargenData");
        sqlite3_finalize(s);
        Log(Debug::Info) << "[PlayerDB] chargen data saved for char id=" << characterId << " race=" << race
                         << " class=" << classId << " birthSign=" << birthSign;
    }

    void PlayerDatabase::markChargenComplete(int64_t characterId)
    {
        sqlite3_stmt* s = prepare("UPDATE characters SET is_new=0, last_seen=?1 WHERE id=?2");
        sqlite3_bind_int64(s, 1, static_cast<int64_t>(std::time(nullptr)));
        sqlite3_bind_int64(s, 2, characterId);
        checkSqlite(sqlite3_step(s), mDb, "markChargenComplete");
        sqlite3_finalize(s);
        Log(Debug::Info) << "[PlayerDB] chargen complete for char id=" << characterId;
    }

    void PlayerDatabase::touch(int64_t characterId)
    {
        sqlite3_stmt* s = prepare("UPDATE characters SET last_seen=?1 WHERE id=?2");
        sqlite3_bind_int64(s, 1, static_cast<int64_t>(std::time(nullptr)));
        sqlite3_bind_int64(s, 2, characterId);
        checkSqlite(sqlite3_step(s), mDb, "touch");
        sqlite3_finalize(s);
    }

    bool PlayerDatabase::deleteCharacter(int64_t accountId, std::string_view charName)
    {
        sqlite3_stmt* s = prepare("DELETE FROM characters WHERE account_id=?1 AND name=?2");
        sqlite3_bind_int64(s, 1, accountId);
        sqlite3_bind_text(s, 2, charName.data(), static_cast<int>(charName.size()), SQLITE_TRANSIENT);
        checkSqlite(sqlite3_step(s), mDb, "deleteCharacter");
        int changes = sqlite3_changes(mDb);
        sqlite3_finalize(s);
        return changes > 0;
    }

    void PlayerDatabase::setNickname(int64_t characterId, std::string_view nickname)
    {
        sqlite3_stmt* s = prepare("UPDATE characters SET nickname=?1 WHERE id=?2");
        sqlite3_bind_text(s, 1, nickname.data(), static_cast<int>(nickname.size()), SQLITE_TRANSIENT);
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
        sqlite3_stmt* s = prepare("DELETE FROM character_marks WHERE character_id=?1 AND mark_name=?2");
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

    void PlayerDatabase::saveCharacterInventory(int64_t characterId, const std::vector<Item>& items, bool touchLastSeen)
    {
        exec("BEGIN");
        try
        {
            const std::string characterKey = std::to_string(characterId);
            sqlite3_stmt* clear = prepare("DELETE FROM character_inventory WHERE character_id=?1");
            sqlite3_bind_int64(clear, 1, characterId);
            checkSqlite(sqlite3_step(clear), mDb, "clearCharacterInventory");
            sqlite3_finalize(clear);

            sqlite3_stmt* insert = prepare(
                "INSERT INTO character_inventory(character_id, item_index, ref_id, item_count, charge, "
                "enchantment_charge, soul)"
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

            sqlite3_stmt* clearLinks = prepare(
                "DELETE FROM world_dynamic_record_links"
                " WHERE link_kind=?1 AND owner_a=?2 AND owner_b=?3 AND owner_c=?4");
            clearDynamicRecordLinksForOwner(mDb, clearLinks, "inventory_item", characterKey, "", "");
            sqlite3_finalize(clearLinks);

            sqlite3_stmt* insertLink = prepare(
                "INSERT OR REPLACE INTO world_dynamic_record_links(record_id, link_kind, owner_a, owner_b, owner_c, "
                "owner_index)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
            for (std::size_t i = 0; i < items.size(); ++i)
                insertDynamicRecordLink(
                    mDb, insertLink, items[i].refId, "inventory_item", characterKey, "", "", static_cast<int64_t>(i));
            sqlite3_finalize(insertLink);

            sqlite3_stmt* mark
                = prepare(touchLastSeen ? "UPDATE characters SET inventory_saved=1, last_seen=?1 WHERE id=?2"
                                        : "UPDATE characters SET inventory_saved=1 WHERE id=?1");
            if (touchLastSeen)
            {
                sqlite3_bind_int64(mark, 1, static_cast<int64_t>(std::time(nullptr)));
                sqlite3_bind_int64(mark, 2, characterId);
            }
            else
            {
                sqlite3_bind_int64(mark, 1, characterId);
            }
            checkSqlite(sqlite3_step(mark), mDb, "markCharacterInventorySaved");
            sqlite3_finalize(mark);

            exec("COMMIT");
        }
        catch (...)
        {
            try
            {
                exec("ROLLBACK");
            }
            catch (...)
            {
            }
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

    void PlayerDatabase::saveCharacterEquipment(
        int64_t characterId, const std::vector<EquipmentItem>& equipment, bool touchLastSeen)
    {
        exec("BEGIN");
        try
        {
            const std::string characterKey = std::to_string(characterId);
            sqlite3_stmt* clear = prepare("DELETE FROM character_equipment WHERE character_id=?1");
            sqlite3_bind_int64(clear, 1, characterId);
            checkSqlite(sqlite3_step(clear), mDb, "clearCharacterEquipment");
            sqlite3_finalize(clear);

            sqlite3_stmt* insert = prepare(
                "INSERT INTO character_equipment(character_id, slot, ref_id, item_count, charge, enchantment_charge, "
                "soul)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");

            for (const EquipmentItem& entry : equipment)
            {
                if (entry.item.refId.empty())
                    continue;

                sqlite3_bind_int64(insert, 1, characterId);
                sqlite3_bind_int(insert, 2, entry.slot);
                sqlite3_bind_text(
                    insert, 3, entry.item.refId.c_str(), static_cast<int>(entry.item.refId.size()), SQLITE_TRANSIENT);
                sqlite3_bind_int(insert, 4, entry.item.count);
                sqlite3_bind_int(insert, 5, entry.item.charge);
                sqlite3_bind_double(insert, 6, entry.item.enchantmentCharge);
                sqlite3_bind_text(
                    insert, 7, entry.item.soul.c_str(), static_cast<int>(entry.item.soul.size()), SQLITE_TRANSIENT);
                checkSqlite(sqlite3_step(insert), mDb, "insertCharacterEquipment");
                sqlite3_reset(insert);
                sqlite3_clear_bindings(insert);
            }
            sqlite3_finalize(insert);

            sqlite3_stmt* clearLinks = prepare(
                "DELETE FROM world_dynamic_record_links"
                " WHERE link_kind=?1 AND owner_a=?2 AND owner_b=?3 AND owner_c=?4");
            clearDynamicRecordLinksForOwner(mDb, clearLinks, "equipment_item", characterKey, "", "");
            sqlite3_finalize(clearLinks);

            sqlite3_stmt* insertLink = prepare(
                "INSERT OR REPLACE INTO world_dynamic_record_links(record_id, link_kind, owner_a, owner_b, owner_c, "
                "owner_index)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
            for (const EquipmentItem& entry : equipment)
            {
                if (entry.item.refId.empty())
                    continue;
                insertDynamicRecordLink(
                    mDb, insertLink, entry.item.refId, "equipment_item", characterKey, "", "", entry.slot);
            }
            sqlite3_finalize(insertLink);

            sqlite3_stmt* mark
                = prepare(touchLastSeen ? "UPDATE characters SET equipment_saved=1, last_seen=?1 WHERE id=?2"
                                        : "UPDATE characters SET equipment_saved=1 WHERE id=?1");
            if (touchLastSeen)
            {
                sqlite3_bind_int64(mark, 1, static_cast<int64_t>(std::time(nullptr)));
                sqlite3_bind_int64(mark, 2, characterId);
            }
            else
            {
                sqlite3_bind_int64(mark, 1, characterId);
            }
            checkSqlite(sqlite3_step(mark), mDb, "markCharacterEquipmentSaved");
            sqlite3_finalize(mark);

            exec("COMMIT");
        }
        catch (...)
        {
            try
            {
                exec("ROLLBACK");
            }
            catch (...)
            {
            }
            throw;
        }
    }

    bool PlayerDatabase::loadCharacterStats(int64_t characterId, BasePlayer& player)
    {
        sqlite3_stmt* meta = prepare("SELECT stats_saved, level, level_progress FROM characters WHERE id=?1 LIMIT 1");
        sqlite3_bind_int64(meta, 1, characterId);
        const int metaRc = sqlite3_step(meta);
        if (metaRc != SQLITE_ROW || sqlite3_column_int(meta, 0) == 0)
        {
            sqlite3_finalize(meta);
            return false;
        }
        player.level = sqlite3_column_int(meta, 1);
        player.levelProgress = static_cast<float>(sqlite3_column_double(meta, 2));
        sqlite3_finalize(meta);

        sqlite3_stmt* dyn = prepare(
            "SELECT health_base, health_current, health_mod, magicka_base, magicka_current, magicka_mod,"
            " fatigue_base, fatigue_current, fatigue_mod"
            " FROM character_dynamic_stats WHERE character_id=?1 LIMIT 1");
        sqlite3_bind_int64(dyn, 1, characterId);
        if (sqlite3_step(dyn) == SQLITE_ROW)
        {
            player.dynamicStats.health.base = static_cast<float>(sqlite3_column_double(dyn, 0));
            player.dynamicStats.health.current = static_cast<float>(sqlite3_column_double(dyn, 1));
            player.dynamicStats.health.mod = static_cast<float>(sqlite3_column_double(dyn, 2));
            player.dynamicStats.magicka.base = static_cast<float>(sqlite3_column_double(dyn, 3));
            player.dynamicStats.magicka.current = static_cast<float>(sqlite3_column_double(dyn, 4));
            player.dynamicStats.magicka.mod = static_cast<float>(sqlite3_column_double(dyn, 5));
            player.dynamicStats.fatigue.base = static_cast<float>(sqlite3_column_double(dyn, 6));
            player.dynamicStats.fatigue.current = static_cast<float>(sqlite3_column_double(dyn, 7));
            player.dynamicStats.fatigue.mod = static_cast<float>(sqlite3_column_double(dyn, 8));
        }
        sqlite3_finalize(dyn);

        sqlite3_stmt* attrs = prepare(
            "SELECT attribute_index, base, mod, damage"
            " FROM character_attributes WHERE character_id=?1 ORDER BY attribute_index");
        sqlite3_bind_int64(attrs, 1, characterId);
        while (sqlite3_step(attrs) == SQLITE_ROW)
        {
            const int index = sqlite3_column_int(attrs, 0);
            if (index < 0 || index >= BasePlayer::NUM_ATTRIBUTES)
                continue;
            Attribute& attribute = player.attributes[static_cast<std::size_t>(index)];
            attribute.base = sqlite3_column_int(attrs, 1);
            attribute.mod = static_cast<float>(sqlite3_column_double(attrs, 2));
            attribute.damage = static_cast<float>(sqlite3_column_double(attrs, 3));
        }
        sqlite3_finalize(attrs);

        sqlite3_stmt* skills = prepare(
            "SELECT skill_index, base, mod, damage, progress, increases"
            " FROM character_skills WHERE character_id=?1 ORDER BY skill_index");
        sqlite3_bind_int64(skills, 1, characterId);
        while (sqlite3_step(skills) == SQLITE_ROW)
        {
            const int index = sqlite3_column_int(skills, 0);
            if (index < 0 || index >= BasePlayer::NUM_SKILLS)
                continue;
            Skill& skill = player.skills[static_cast<std::size_t>(index)];
            skill.base = static_cast<float>(sqlite3_column_double(skills, 1));
            skill.mod = static_cast<float>(sqlite3_column_double(skills, 2));
            skill.damage = static_cast<float>(sqlite3_column_double(skills, 3));
            skill.progress = static_cast<float>(sqlite3_column_double(skills, 4));
            skill.increases = sqlite3_column_int(skills, 5);
        }
        sqlite3_finalize(skills);

        player.hasSavedStats = true;
        return true;
    }

    void PlayerDatabase::saveCharacterStats(int64_t characterId, const BasePlayer& player, bool touchLastSeen)
    {
        exec("BEGIN");
        try
        {
            sqlite3_stmt* dyn = prepare(
                "INSERT INTO character_dynamic_stats(character_id, health_base, health_current, health_mod,"
                " magicka_base, magicka_current, magicka_mod, fatigue_base, fatigue_current, fatigue_mod)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)"
                " ON CONFLICT(character_id) DO UPDATE SET"
                " health_base=excluded.health_base,"
                " health_current=excluded.health_current,"
                " health_mod=excluded.health_mod,"
                " magicka_base=excluded.magicka_base,"
                " magicka_current=excluded.magicka_current,"
                " magicka_mod=excluded.magicka_mod,"
                " fatigue_base=excluded.fatigue_base,"
                " fatigue_current=excluded.fatigue_current,"
                " fatigue_mod=excluded.fatigue_mod");
            sqlite3_bind_int64(dyn, 1, characterId);
            sqlite3_bind_double(dyn, 2, player.dynamicStats.health.base);
            sqlite3_bind_double(dyn, 3, player.dynamicStats.health.current);
            sqlite3_bind_double(dyn, 4, player.dynamicStats.health.mod);
            sqlite3_bind_double(dyn, 5, player.dynamicStats.magicka.base);
            sqlite3_bind_double(dyn, 6, player.dynamicStats.magicka.current);
            sqlite3_bind_double(dyn, 7, player.dynamicStats.magicka.mod);
            sqlite3_bind_double(dyn, 8, player.dynamicStats.fatigue.base);
            sqlite3_bind_double(dyn, 9, player.dynamicStats.fatigue.current);
            sqlite3_bind_double(dyn, 10, player.dynamicStats.fatigue.mod);
            checkSqlite(sqlite3_step(dyn), mDb, "upsertCharacterDynamicStats");
            sqlite3_finalize(dyn);

            sqlite3_stmt* clearAttrs = prepare("DELETE FROM character_attributes WHERE character_id=?1");
            sqlite3_bind_int64(clearAttrs, 1, characterId);
            checkSqlite(sqlite3_step(clearAttrs), mDb, "clearCharacterAttributes");
            sqlite3_finalize(clearAttrs);

            sqlite3_stmt* insertAttr = prepare(
                "INSERT INTO character_attributes(character_id, attribute_index, base, mod, damage)"
                " VALUES(?1, ?2, ?3, ?4, ?5)");
            for (std::size_t i = 0; i < player.attributes.size(); ++i)
            {
                const Attribute& attribute = player.attributes[i];
                sqlite3_bind_int64(insertAttr, 1, characterId);
                sqlite3_bind_int(insertAttr, 2, static_cast<int>(i));
                sqlite3_bind_int(insertAttr, 3, attribute.base);
                sqlite3_bind_double(insertAttr, 4, attribute.mod);
                sqlite3_bind_double(insertAttr, 5, attribute.damage);
                checkSqlite(sqlite3_step(insertAttr), mDb, "insertCharacterAttribute");
                sqlite3_reset(insertAttr);
                sqlite3_clear_bindings(insertAttr);
            }
            sqlite3_finalize(insertAttr);

            sqlite3_stmt* clearSkills = prepare("DELETE FROM character_skills WHERE character_id=?1");
            sqlite3_bind_int64(clearSkills, 1, characterId);
            checkSqlite(sqlite3_step(clearSkills), mDb, "clearCharacterSkills");
            sqlite3_finalize(clearSkills);

            sqlite3_stmt* insertSkill = prepare(
                "INSERT INTO character_skills(character_id, skill_index, base, mod, damage, progress, increases)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
            for (std::size_t i = 0; i < player.skills.size(); ++i)
            {
                const Skill& skill = player.skills[i];
                sqlite3_bind_int64(insertSkill, 1, characterId);
                sqlite3_bind_int(insertSkill, 2, static_cast<int>(i));
                sqlite3_bind_double(insertSkill, 3, skill.base);
                sqlite3_bind_double(insertSkill, 4, skill.mod);
                sqlite3_bind_double(insertSkill, 5, skill.damage);
                sqlite3_bind_double(insertSkill, 6, skill.progress);
                sqlite3_bind_int(insertSkill, 7, skill.increases);
                checkSqlite(sqlite3_step(insertSkill), mDb, "insertCharacterSkill");
                sqlite3_reset(insertSkill);
                sqlite3_clear_bindings(insertSkill);
            }
            sqlite3_finalize(insertSkill);

            sqlite3_stmt* mark = prepare(touchLastSeen
                    ? "UPDATE characters SET stats_saved=1, level=?1, level_progress=?2, last_seen=?3 WHERE id=?4"
                    : "UPDATE characters SET stats_saved=1, level=?1, level_progress=?2 WHERE id=?3");
            sqlite3_bind_int(mark, 1, player.level);
            sqlite3_bind_double(mark, 2, player.levelProgress);
            if (touchLastSeen)
            {
                sqlite3_bind_int64(mark, 3, static_cast<int64_t>(std::time(nullptr)));
                sqlite3_bind_int64(mark, 4, characterId);
            }
            else
            {
                sqlite3_bind_int64(mark, 3, characterId);
            }
            checkSqlite(sqlite3_step(mark), mDb, "markCharacterStatsSaved");
            sqlite3_finalize(mark);

            exec("COMMIT");
        }
        catch (...)
        {
            try
            {
                exec("ROLLBACK");
            }
            catch (...)
            {
            }
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
        exec("BEGIN");
        try
        {
            const std::string ownerA = std::to_string(object.mpNum);

            sqlite3_stmt* s = prepare(
                "INSERT INTO world_objects(mp_num, cell_id, ref_id, item_count, pos_x, pos_y, pos_z, rot_x, rot_y, "
                "rot_z)"
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

            sqlite3_stmt* clearLinks = prepare(
                "DELETE FROM world_dynamic_record_links"
                " WHERE link_kind=?1 AND owner_a=?2 AND owner_b=?3 AND owner_c=?4");
            clearDynamicRecordLinksForOwner(mDb, clearLinks, "placed_object", ownerA, object.cellId, "");
            sqlite3_finalize(clearLinks);

            sqlite3_stmt* insertLink = prepare(
                "INSERT OR REPLACE INTO world_dynamic_record_links(record_id, link_kind, owner_a, owner_b, owner_c, "
                "owner_index)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
            insertDynamicRecordLink(mDb, insertLink, object.refId, "placed_object", ownerA, object.cellId, "", 0);
            sqlite3_finalize(insertLink);

            exec("COMMIT");
        }
        catch (...)
        {
            try
            {
                exec("ROLLBACK");
            }
            catch (...)
            {
            }
            throw;
        }
    }

    void PlayerDatabase::deleteWorldObject(uint32_t mpNum)
    {
        exec("BEGIN");
        try
        {
            const std::string ownerA = std::to_string(mpNum);

            sqlite3_stmt* s = prepare("DELETE FROM world_objects WHERE mp_num=?1");
            sqlite3_bind_int64(s, 1, mpNum);
            checkSqlite(sqlite3_step(s), mDb, "deleteWorldObject");
            sqlite3_finalize(s);

            sqlite3_stmt* clearLinks
                = prepare("DELETE FROM world_dynamic_record_links WHERE link_kind=?1 AND owner_a=?2");
            sqlite3_bind_text(clearLinks, 1, "placed_object", -1, SQLITE_STATIC);
            sqlite3_bind_text(clearLinks, 2, ownerA.c_str(), -1, SQLITE_TRANSIENT);
            checkSqlite(sqlite3_step(clearLinks), mDb, "deleteWorldObject(link)");
            sqlite3_finalize(clearLinks);

            exec("COMMIT");
        }
        catch (...)
        {
            try
            {
                exec("ROLLBACK");
            }
            catch (...)
            {
            }
            throw;
        }
    }

    std::size_t PlayerDatabase::deleteWorldObjectsForCell(std::string_view cellId)
    {
        if (cellId.empty())
            return 0;

        exec("BEGIN");
        try
        {
            sqlite3_stmt* s = prepare("DELETE FROM world_objects WHERE cell_id=?1");
            sqlite3_bind_text(s, 1, cellId.data(), static_cast<int>(cellId.size()), SQLITE_TRANSIENT);
            checkSqlite(sqlite3_step(s), mDb, "deleteWorldObjectsForCell");
            const std::size_t removed = static_cast<std::size_t>(sqlite3_changes(mDb));
            sqlite3_finalize(s);

            sqlite3_stmt* clearLinks = prepare(
                "DELETE FROM world_dynamic_record_links WHERE link_kind=?1 AND owner_b=?2");
            sqlite3_bind_text(clearLinks, 1, "placed_object", -1, SQLITE_STATIC);
            sqlite3_bind_text(clearLinks, 2, cellId.data(), static_cast<int>(cellId.size()), SQLITE_TRANSIENT);
            checkSqlite(sqlite3_step(clearLinks), mDb, "deleteWorldObjectsForCell(link)");
            sqlite3_finalize(clearLinks);

            exec("COMMIT");
            return removed;
        }
        catch (...)
        {
            try
            {
                exec("ROLLBACK");
            }
            catch (...)
            {
            }
            throw;
        }
    }

    std::vector<PersistedSpawnedActor> PlayerDatabase::loadSpawnedActors()
    {
        sqlite3_stmt* s = prepare(
            "SELECT mp_num, cell_id, ref_id, ref_num, persistent,"
            " pos_x, pos_y, pos_z, rot_x, rot_y, rot_z,"
            " health_base, health_current, health_mod,"
            " magicka_base, magicka_current, magicka_mod,"
            " fatigue_base, fatigue_current, fatigue_mod,"
            " is_dead, death_state, death_anim_group, created_at, updated_at"
            " FROM world_spawned_actors WHERE persistent != 0 ORDER BY cell_id, mp_num");

        std::vector<PersistedSpawnedActor> records;
        while (sqlite3_step(s) == SQLITE_ROW)
        {
            PersistedSpawnedActor record;
            BaseActor& actor = record.actor;
            actor.mpNum = static_cast<uint32_t>(sqlite3_column_int64(s, 0));

            auto col = [&](int i) -> std::string {
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
                return t ? t : "";
            };

            actor.cellId = col(1);
            actor.refId = col(2);
            actor.refNum = static_cast<uint32_t>(sqlite3_column_int64(s, 3));
            record.persistent = sqlite3_column_int(s, 4) != 0;
            actor.position.pos[0] = static_cast<float>(sqlite3_column_double(s, 5));
            actor.position.pos[1] = static_cast<float>(sqlite3_column_double(s, 6));
            actor.position.pos[2] = static_cast<float>(sqlite3_column_double(s, 7));
            actor.position.rot[0] = static_cast<float>(sqlite3_column_double(s, 8));
            actor.position.rot[1] = static_cast<float>(sqlite3_column_double(s, 9));
            actor.position.rot[2] = static_cast<float>(sqlite3_column_double(s, 10));
            actor.dynamicStats.health.base = static_cast<float>(sqlite3_column_double(s, 11));
            actor.dynamicStats.health.current = static_cast<float>(sqlite3_column_double(s, 12));
            actor.dynamicStats.health.mod = static_cast<float>(sqlite3_column_double(s, 13));
            actor.dynamicStats.magicka.base = static_cast<float>(sqlite3_column_double(s, 14));
            actor.dynamicStats.magicka.current = static_cast<float>(sqlite3_column_double(s, 15));
            actor.dynamicStats.magicka.mod = static_cast<float>(sqlite3_column_double(s, 16));
            actor.dynamicStats.fatigue.base = static_cast<float>(sqlite3_column_double(s, 17));
            actor.dynamicStats.fatigue.current = static_cast<float>(sqlite3_column_double(s, 18));
            actor.dynamicStats.fatigue.mod = static_cast<float>(sqlite3_column_double(s, 19));
            actor.isDead = sqlite3_column_int(s, 20) != 0;
            actor.deathState = static_cast<uint8_t>(sqlite3_column_int(s, 21));
            actor.deathAnimGroup = col(22);
            record.createdAt = sqlite3_column_int64(s, 23);
            record.updatedAt = sqlite3_column_int64(s, 24);
            actor.equipment.resize(BaseActor::NUM_EQUIPMENT_SLOTS);
            records.push_back(std::move(record));
        }

        sqlite3_finalize(s);
        return records;
    }

    void PlayerDatabase::upsertSpawnedActor(const PersistedSpawnedActor& record)
    {
        const BaseActor& actor = record.actor;
        if (actor.mpNum == 0 || actor.refId.empty() || actor.cellId.empty())
            return;

        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        const int64_t createdAt = record.createdAt != 0 ? record.createdAt : now;

        sqlite3_stmt* s = prepare(
            "INSERT INTO world_spawned_actors("
            " mp_num, cell_id, ref_id, ref_num, persistent,"
            " pos_x, pos_y, pos_z, rot_x, rot_y, rot_z,"
            " health_base, health_current, health_mod,"
            " magicka_base, magicka_current, magicka_mod,"
            " fatigue_base, fatigue_current, fatigue_mod,"
            " is_dead, death_state, death_anim_group, created_at, updated_at)"
            " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11,"
            " ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23, ?24, ?25)"
            " ON CONFLICT(mp_num) DO UPDATE SET"
            " cell_id=excluded.cell_id,"
            " ref_id=excluded.ref_id,"
            " ref_num=excluded.ref_num,"
            " persistent=excluded.persistent,"
            " pos_x=excluded.pos_x,"
            " pos_y=excluded.pos_y,"
            " pos_z=excluded.pos_z,"
            " rot_x=excluded.rot_x,"
            " rot_y=excluded.rot_y,"
            " rot_z=excluded.rot_z,"
            " health_base=excluded.health_base,"
            " health_current=excluded.health_current,"
            " health_mod=excluded.health_mod,"
            " magicka_base=excluded.magicka_base,"
            " magicka_current=excluded.magicka_current,"
            " magicka_mod=excluded.magicka_mod,"
            " fatigue_base=excluded.fatigue_base,"
            " fatigue_current=excluded.fatigue_current,"
            " fatigue_mod=excluded.fatigue_mod,"
            " is_dead=excluded.is_dead,"
            " death_state=excluded.death_state,"
            " death_anim_group=excluded.death_anim_group,"
            " updated_at=excluded.updated_at");

        sqlite3_bind_int64(s, 1, actor.mpNum);
        sqlite3_bind_text(s, 2, actor.cellId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, actor.refId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 4, actor.refNum);
        sqlite3_bind_int(s, 5, record.persistent ? 1 : 0);
        sqlite3_bind_double(s, 6, actor.position.pos[0]);
        sqlite3_bind_double(s, 7, actor.position.pos[1]);
        sqlite3_bind_double(s, 8, actor.position.pos[2]);
        sqlite3_bind_double(s, 9, actor.position.rot[0]);
        sqlite3_bind_double(s, 10, actor.position.rot[1]);
        sqlite3_bind_double(s, 11, actor.position.rot[2]);
        sqlite3_bind_double(s, 12, actor.dynamicStats.health.base);
        sqlite3_bind_double(s, 13, actor.dynamicStats.health.current);
        sqlite3_bind_double(s, 14, actor.dynamicStats.health.mod);
        sqlite3_bind_double(s, 15, actor.dynamicStats.magicka.base);
        sqlite3_bind_double(s, 16, actor.dynamicStats.magicka.current);
        sqlite3_bind_double(s, 17, actor.dynamicStats.magicka.mod);
        sqlite3_bind_double(s, 18, actor.dynamicStats.fatigue.base);
        sqlite3_bind_double(s, 19, actor.dynamicStats.fatigue.current);
        sqlite3_bind_double(s, 20, actor.dynamicStats.fatigue.mod);
        sqlite3_bind_int(s, 21, actor.isDead ? 1 : 0);
        sqlite3_bind_int(s, 22, actor.deathState);
        sqlite3_bind_text(s, 23, actor.deathAnimGroup.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 24, createdAt);
        sqlite3_bind_int64(s, 25, now);
        checkSqlite(sqlite3_step(s), mDb, "upsertSpawnedActor");
        sqlite3_finalize(s);
    }

    void PlayerDatabase::deleteSpawnedActor(uint32_t mpNum)
    {
        if (mpNum == 0)
            return;

        sqlite3_stmt* s = prepare("DELETE FROM world_spawned_actors WHERE mp_num=?1");
        sqlite3_bind_int64(s, 1, mpNum);
        checkSqlite(sqlite3_step(s), mDb, "deleteSpawnedActor");
        sqlite3_finalize(s);
    }

    std::size_t PlayerDatabase::deleteSpawnedActorsForCell(std::string_view cellId)
    {
        if (cellId.empty())
            return 0;

        sqlite3_stmt* s = prepare("DELETE FROM world_spawned_actors WHERE cell_id=?1");
        sqlite3_bind_text(s, 1, cellId.data(), static_cast<int>(cellId.size()), SQLITE_TRANSIENT);
        checkSqlite(sqlite3_step(s), mDb, "deleteSpawnedActorsForCell");
        const std::size_t removed = static_cast<std::size_t>(sqlite3_changes(mDb));
        sqlite3_finalize(s);
        return removed;
    }

    std::vector<BaseActor> PlayerDatabase::loadDeadVanillaActors()
    {
        sqlite3_stmt* s = prepare(
            "SELECT ref_id, ref_num, cell_id,"
            " pos_x, pos_y, pos_z, rot_x, rot_y, rot_z,"
            " health_base, health_current, health_mod,"
            " magicka_base, magicka_current, magicka_mod,"
            " fatigue_base, fatigue_current, fatigue_mod,"
            " is_dead, death_state, is_instant_death, death_anim_group, created_at, updated_at"
            " FROM world_dead_vanilla_actors WHERE is_dead != 0 ORDER BY cell_id, ref_id, ref_num");

        std::vector<BaseActor> actors;
        while (sqlite3_step(s) == SQLITE_ROW)
        {
            BaseActor actor;
            auto col = [&](int i) -> std::string {
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
                return t ? t : "";
            };

            actor.refId = col(0);
            actor.refNum = static_cast<uint32_t>(sqlite3_column_int64(s, 1));
            actor.mpNum = 0;
            actor.cellId = col(2);
            actor.position.pos[0] = static_cast<float>(sqlite3_column_double(s, 3));
            actor.position.pos[1] = static_cast<float>(sqlite3_column_double(s, 4));
            actor.position.pos[2] = static_cast<float>(sqlite3_column_double(s, 5));
            actor.position.rot[0] = static_cast<float>(sqlite3_column_double(s, 6));
            actor.position.rot[1] = static_cast<float>(sqlite3_column_double(s, 7));
            actor.position.rot[2] = static_cast<float>(sqlite3_column_double(s, 8));
            actor.dynamicStats.health.base = static_cast<float>(sqlite3_column_double(s, 9));
            actor.dynamicStats.health.current = static_cast<float>(sqlite3_column_double(s, 10));
            actor.dynamicStats.health.mod = static_cast<float>(sqlite3_column_double(s, 11));
            actor.dynamicStats.magicka.base = static_cast<float>(sqlite3_column_double(s, 12));
            actor.dynamicStats.magicka.current = static_cast<float>(sqlite3_column_double(s, 13));
            actor.dynamicStats.magicka.mod = static_cast<float>(sqlite3_column_double(s, 14));
            actor.dynamicStats.fatigue.base = static_cast<float>(sqlite3_column_double(s, 15));
            actor.dynamicStats.fatigue.current = static_cast<float>(sqlite3_column_double(s, 16));
            actor.dynamicStats.fatigue.mod = static_cast<float>(sqlite3_column_double(s, 17));
            actor.isDead = sqlite3_column_int(s, 18) != 0;
            actor.deathState = static_cast<uint8_t>(sqlite3_column_int(s, 19));
            actor.isInstantDeath = sqlite3_column_int(s, 20) != 0;
            actor.deathAnimGroup = col(21);
            actor.equipment.resize(BaseActor::NUM_EQUIPMENT_SLOTS);
            actors.push_back(std::move(actor));
        }

        sqlite3_finalize(s);
        return actors;
    }

    void PlayerDatabase::upsertDeadVanillaActor(const BaseActor& actor)
    {
        if (actor.mpNum != 0 || actor.refId.empty() || actor.cellId.empty() || !actor.isDead)
            return;

        const int64_t now = static_cast<int64_t>(std::time(nullptr));

        sqlite3_stmt* s = prepare(
            "INSERT INTO world_dead_vanilla_actors("
            " ref_id, ref_num, cell_id,"
            " pos_x, pos_y, pos_z, rot_x, rot_y, rot_z,"
            " health_base, health_current, health_mod,"
            " magicka_base, magicka_current, magicka_mod,"
            " fatigue_base, fatigue_current, fatigue_mod,"
            " is_dead, death_state, is_instant_death, death_anim_group, created_at, updated_at)"
            " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9,"
            " ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18,"
            " ?19, ?20, ?21, ?22, ?23, ?24)"
            " ON CONFLICT(ref_id, ref_num) DO UPDATE SET"
            " cell_id=excluded.cell_id,"
            " pos_x=excluded.pos_x,"
            " pos_y=excluded.pos_y,"
            " pos_z=excluded.pos_z,"
            " rot_x=excluded.rot_x,"
            " rot_y=excluded.rot_y,"
            " rot_z=excluded.rot_z,"
            " health_base=excluded.health_base,"
            " health_current=excluded.health_current,"
            " health_mod=excluded.health_mod,"
            " magicka_base=excluded.magicka_base,"
            " magicka_current=excluded.magicka_current,"
            " magicka_mod=excluded.magicka_mod,"
            " fatigue_base=excluded.fatigue_base,"
            " fatigue_current=excluded.fatigue_current,"
            " fatigue_mod=excluded.fatigue_mod,"
            " is_dead=excluded.is_dead,"
            " death_state=excluded.death_state,"
            " is_instant_death=excluded.is_instant_death,"
            " death_anim_group=excluded.death_anim_group,"
            " updated_at=excluded.updated_at");

        sqlite3_bind_text(s, 1, actor.refId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, actor.refNum);
        sqlite3_bind_text(s, 3, actor.cellId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(s, 4, actor.position.pos[0]);
        sqlite3_bind_double(s, 5, actor.position.pos[1]);
        sqlite3_bind_double(s, 6, actor.position.pos[2]);
        sqlite3_bind_double(s, 7, actor.position.rot[0]);
        sqlite3_bind_double(s, 8, actor.position.rot[1]);
        sqlite3_bind_double(s, 9, actor.position.rot[2]);
        sqlite3_bind_double(s, 10, actor.dynamicStats.health.base);
        sqlite3_bind_double(s, 11, actor.dynamicStats.health.current);
        sqlite3_bind_double(s, 12, actor.dynamicStats.health.mod);
        sqlite3_bind_double(s, 13, actor.dynamicStats.magicka.base);
        sqlite3_bind_double(s, 14, actor.dynamicStats.magicka.current);
        sqlite3_bind_double(s, 15, actor.dynamicStats.magicka.mod);
        sqlite3_bind_double(s, 16, actor.dynamicStats.fatigue.base);
        sqlite3_bind_double(s, 17, actor.dynamicStats.fatigue.current);
        sqlite3_bind_double(s, 18, actor.dynamicStats.fatigue.mod);
        sqlite3_bind_int(s, 19, actor.isDead ? 1 : 0);
        sqlite3_bind_int(s, 20, actor.deathState);
        sqlite3_bind_int(s, 21, actor.isInstantDeath ? 1 : 0);
        sqlite3_bind_text(s, 22, actor.deathAnimGroup.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 23, now);
        sqlite3_bind_int64(s, 24, now);
        checkSqlite(sqlite3_step(s), mDb, "upsertDeadVanillaActor");
        sqlite3_finalize(s);
    }

    void PlayerDatabase::deleteDeadVanillaActor(std::string_view refId, uint32_t refNum)
    {
        if (refId.empty())
            return;

        sqlite3_stmt* s = prepare("DELETE FROM world_dead_vanilla_actors WHERE ref_id=?1 AND ref_num=?2");
        sqlite3_bind_text(s, 1, refId.data(), static_cast<int>(refId.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, refNum);
        checkSqlite(sqlite3_step(s), mDb, "deleteDeadVanillaActor");
        sqlite3_finalize(s);
    }

    std::size_t PlayerDatabase::deleteDeadVanillaActorsForCell(std::string_view cellId)
    {
        if (cellId.empty())
            return 0;

        sqlite3_stmt* s = prepare("DELETE FROM world_dead_vanilla_actors WHERE cell_id=?1");
        sqlite3_bind_text(s, 1, cellId.data(), static_cast<int>(cellId.size()), SQLITE_TRANSIENT);
        checkSqlite(sqlite3_step(s), mDb, "deleteDeadVanillaActorsForCell");
        const std::size_t removed = static_cast<std::size_t>(sqlite3_changes(mDb));
        sqlite3_finalize(s);
        return removed;
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
            const std::string ownerC = std::to_string(record.refNum);

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

            sqlite3_stmt* clearItems
                = prepare("DELETE FROM world_container_items WHERE cell_id=?1 AND ref_id=?2 AND ref_num=?3");
            sqlite3_bind_text(clearItems, 1, record.cellId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(clearItems, 2, record.refId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(clearItems, 3, record.refNum);
            checkSqlite(sqlite3_step(clearItems), mDb, "upsertContainerRecord(clearItems)");
            sqlite3_finalize(clearItems);

            sqlite3_stmt* itemStmt = prepare(
                "INSERT INTO world_container_items(cell_id, ref_id, ref_num, item_index, item_ref_id, item_count, "
                "charge)"
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

            sqlite3_stmt* clearLinks = prepare(
                "DELETE FROM world_dynamic_record_links"
                " WHERE link_kind=?1 AND owner_a=?2 AND owner_b=?3 AND owner_c=?4");
            clearDynamicRecordLinksForOwner(mDb, clearLinks, "container_parent", record.cellId, record.refId, ownerC);
            clearDynamicRecordLinksForOwner(mDb, clearLinks, "container_item", record.cellId, record.refId, ownerC);
            sqlite3_finalize(clearLinks);

            sqlite3_stmt* insertLink = prepare(
                "INSERT OR REPLACE INTO world_dynamic_record_links(record_id, link_kind, owner_a, owner_b, owner_c, "
                "owner_index)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
            insertDynamicRecordLink(
                mDb, insertLink, record.refId, "container_parent", record.cellId, record.refId, ownerC, 0);
            for (std::size_t i = 0; i < record.items.size(); ++i)
                insertDynamicRecordLink(mDb, insertLink, record.items[i].refId, "container_item", record.cellId,
                    record.refId, ownerC, static_cast<int64_t>(i));
            sqlite3_finalize(insertLink);

            exec("COMMIT");
        }
        catch (...)
        {
            try
            {
                exec("ROLLBACK");
            }
            catch (...)
            {
            }
            throw;
        }
    }

    void PlayerDatabase::deleteContainerRecord(std::string_view cellId, std::string_view refId, uint32_t refNum)
    {
        exec("BEGIN");
        try
        {
            const std::string ownerC = std::to_string(refNum);

            sqlite3_stmt* s = prepare("DELETE FROM world_containers WHERE cell_id=?1 AND ref_id=?2 AND ref_num=?3");
            sqlite3_bind_text(s, 1, cellId.data(), static_cast<int>(cellId.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, refId.data(), static_cast<int>(refId.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 3, refNum);
            checkSqlite(sqlite3_step(s), mDb, "deleteContainerRecord");
            sqlite3_finalize(s);

            sqlite3_stmt* clearLinks = prepare(
                "DELETE FROM world_dynamic_record_links"
                " WHERE link_kind=?1 AND owner_a=?2 AND owner_b=?3 AND owner_c=?4");
            clearDynamicRecordLinksForOwner(mDb, clearLinks, "container_parent", cellId, refId, ownerC);
            clearDynamicRecordLinksForOwner(mDb, clearLinks, "container_item", cellId, refId, ownerC);
            sqlite3_finalize(clearLinks);

            exec("COMMIT");
        }
        catch (...)
        {
            try
            {
                exec("ROLLBACK");
            }
            catch (...)
            {
            }
            throw;
        }
    }

    std::size_t PlayerDatabase::deleteContainerRecordsForCell(std::string_view cellId)
    {
        if (cellId.empty())
            return 0;

        exec("BEGIN");
        try
        {
            sqlite3_stmt* s = prepare("DELETE FROM world_containers WHERE cell_id=?1");
            sqlite3_bind_text(s, 1, cellId.data(), static_cast<int>(cellId.size()), SQLITE_TRANSIENT);
            checkSqlite(sqlite3_step(s), mDb, "deleteContainerRecordsForCell");
            const std::size_t removed = static_cast<std::size_t>(sqlite3_changes(mDb));
            sqlite3_finalize(s);

            sqlite3_stmt* clearLinks = prepare(
                "DELETE FROM world_dynamic_record_links"
                " WHERE owner_a=?1 AND (link_kind=?2 OR link_kind=?3)");
            sqlite3_bind_text(clearLinks, 1, cellId.data(), static_cast<int>(cellId.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(clearLinks, 2, "container_parent", -1, SQLITE_STATIC);
            sqlite3_bind_text(clearLinks, 3, "container_item", -1, SQLITE_STATIC);
            checkSqlite(sqlite3_step(clearLinks), mDb, "deleteContainerRecordsForCell(link)");
            sqlite3_finalize(clearLinks);

            exec("COMMIT");
            return removed;
        }
        catch (...)
        {
            try
            {
                exec("ROLLBACK");
            }
            catch (...)
            {
            }
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
        exec("BEGIN");
        try
        {
            const std::string ownerC = std::to_string(entry.refNum);

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

            sqlite3_stmt* clearLinks = prepare(
                "DELETE FROM world_dynamic_record_links"
                " WHERE link_kind=?1 AND owner_a=?2 AND owner_b=?3 AND owner_c=?4");
            clearDynamicRecordLinksForOwner(mDb, clearLinks, "door_state", entry.cellId, entry.refId, ownerC);
            sqlite3_finalize(clearLinks);

            sqlite3_stmt* insertLink = prepare(
                "INSERT OR REPLACE INTO world_dynamic_record_links(record_id, link_kind, owner_a, owner_b, owner_c, "
                "owner_index)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
            insertDynamicRecordLink(mDb, insertLink, entry.refId, "door_state", entry.cellId, entry.refId, ownerC, 0);
            sqlite3_finalize(insertLink);

            exec("COMMIT");
        }
        catch (...)
        {
            try
            {
                exec("ROLLBACK");
            }
            catch (...)
            {
            }
            throw;
        }
    }

    void PlayerDatabase::deleteDoorState(std::string_view cellId, std::string_view refId, uint32_t refNum)
    {
        exec("BEGIN");
        try
        {
            const std::string ownerC = std::to_string(refNum);

            sqlite3_stmt* s = prepare("DELETE FROM world_doors WHERE cell_id=?1 AND ref_id=?2 AND ref_num=?3");
            sqlite3_bind_text(s, 1, cellId.data(), static_cast<int>(cellId.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, refId.data(), static_cast<int>(refId.size()), SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 3, refNum);
            checkSqlite(sqlite3_step(s), mDb, "deleteDoorState");
            sqlite3_finalize(s);

            sqlite3_stmt* clearLinks = prepare(
                "DELETE FROM world_dynamic_record_links"
                " WHERE link_kind=?1 AND owner_a=?2 AND owner_b=?3 AND owner_c=?4");
            clearDynamicRecordLinksForOwner(mDb, clearLinks, "door_state", cellId, refId, ownerC);
            sqlite3_finalize(clearLinks);

            exec("COMMIT");
        }
        catch (...)
        {
            try
            {
                exec("ROLLBACK");
            }
            catch (...)
            {
            }
            throw;
        }
    }

    std::size_t PlayerDatabase::deleteDoorStatesForCell(std::string_view cellId)
    {
        if (cellId.empty())
            return 0;

        exec("BEGIN");
        try
        {
            sqlite3_stmt* s = prepare("DELETE FROM world_doors WHERE cell_id=?1");
            sqlite3_bind_text(s, 1, cellId.data(), static_cast<int>(cellId.size()), SQLITE_TRANSIENT);
            checkSqlite(sqlite3_step(s), mDb, "deleteDoorStatesForCell");
            const std::size_t removed = static_cast<std::size_t>(sqlite3_changes(mDb));
            sqlite3_finalize(s);

            sqlite3_stmt* clearLinks = prepare(
                "DELETE FROM world_dynamic_record_links WHERE link_kind=?1 AND owner_a=?2");
            sqlite3_bind_text(clearLinks, 1, "door_state", -1, SQLITE_STATIC);
            sqlite3_bind_text(clearLinks, 2, cellId.data(), static_cast<int>(cellId.size()), SQLITE_TRANSIENT);
            checkSqlite(sqlite3_step(clearLinks), mDb, "deleteDoorStatesForCell(link)");
            sqlite3_finalize(clearLinks);

            exec("COMMIT");
            return removed;
        }
        catch (...)
        {
            try
            {
                exec("ROLLBACK");
            }
            catch (...)
            {
            }
            throw;
        }
    }

    uint64_t PlayerDatabase::loadNextMpNum(uint64_t minimumNext)
    {
        minimumNext = std::max<uint64_t>(minimumNext, 1);

        sqlite3_stmt* s = prepare("SELECT value FROM world_metadata WHERE key='next_mp_num' LIMIT 1");
        uint64_t storedNext = 0;
        if (sqlite3_step(s) == SQLITE_ROW)
        {
            const sqlite3_int64 value = sqlite3_column_int64(s, 0);
            if (value > 0)
                storedNext = static_cast<uint64_t>(value);
        }
        sqlite3_finalize(s);

        const uint64_t nextMpNum = std::max(storedNext, minimumNext);
        if (nextMpNum != storedNext)
            saveNextMpNum(nextMpNum);

        return nextMpNum;
    }

    void PlayerDatabase::saveNextMpNum(uint64_t nextMpNum)
    {
        nextMpNum = std::max<uint64_t>(nextMpNum, 1);
        if (nextMpNum > static_cast<uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
            throw std::runtime_error("[PlayerDB] next_mp_num exceeds SQLite INTEGER range");

        sqlite3_stmt* s = prepare(
            "INSERT INTO world_metadata(key, value) VALUES('next_mp_num', ?1)"
            " ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        sqlite3_bind_int64(s, 1, static_cast<sqlite3_int64>(nextMpNum));
        checkSqlite(sqlite3_step(s), mDb, "saveNextMpNum");
        sqlite3_finalize(s);
    }

    std::vector<PersistedDynamicRecord> PlayerDatabase::loadDynamicRecords()
    {
        sqlite3_stmt* s = prepare(
            "SELECT record_type, record_id, record_scope, record_data, created_at, updated_at"
            " FROM world_dynamic_records ORDER BY created_at, record_type, record_id");

        std::vector<PersistedDynamicRecord> records;
        while (sqlite3_step(s) == SQLITE_ROW)
        {
            PersistedDynamicRecord record;
            auto textCol = [&](int i) -> std::string {
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
                return t ? t : "";
            };

            record.recordType = textCol(0);
            record.recordId = textCol(1);
            record.recordScope = textCol(2);

            const void* blob = sqlite3_column_blob(s, 3);
            const int blobSize = sqlite3_column_bytes(s, 3);
            if (blob && blobSize > 0)
                record.data.assign(static_cast<const char*>(blob), static_cast<std::size_t>(blobSize));

            record.createdAt = sqlite3_column_int64(s, 4);
            record.updatedAt = sqlite3_column_int64(s, 5);
            records.push_back(std::move(record));
        }

        sqlite3_finalize(s);
        return records;
    }

    void PlayerDatabase::upsertDynamicRecord(const PersistedDynamicRecord& record)
    {
        sqlite3_stmt* s = prepare(
            "INSERT INTO world_dynamic_records(record_type, record_id, record_scope, record_data, created_at, "
            "updated_at)"
            " VALUES(?1, ?2, ?3, ?4, COALESCE(?5, strftime('%s', 'now')), ?6)"
            " ON CONFLICT(record_type, record_id) DO UPDATE SET"
            " record_scope=excluded.record_scope,"
            " record_data=excluded.record_data,"
            " updated_at=excluded.updated_at");

        const int64_t createdAt = record.createdAt != 0 ? record.createdAt : static_cast<int64_t>(std::time(nullptr));
        const int64_t updatedAt = record.updatedAt != 0 ? record.updatedAt : static_cast<int64_t>(std::time(nullptr));

        sqlite3_bind_text(s, 1, record.recordType.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, record.recordId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, record.recordScope.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(s, 4, record.data.data(), static_cast<int>(record.data.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 5, createdAt);
        sqlite3_bind_int64(s, 6, updatedAt);
        checkSqlite(sqlite3_step(s), mDb, "upsertDynamicRecord");
        sqlite3_finalize(s);
    }

    void PlayerDatabase::deleteDynamicRecord(std::string_view recordType, std::string_view recordId)
    {
        sqlite3_stmt* s = prepare("DELETE FROM world_dynamic_records WHERE record_type=?1 AND record_id=?2");
        sqlite3_bind_text(s, 1, recordType.data(), static_cast<int>(recordType.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, recordId.data(), static_cast<int>(recordId.size()), SQLITE_TRANSIENT);
        checkSqlite(sqlite3_step(s), mDb, "deleteDynamicRecord");
        sqlite3_finalize(s);
    }

    std::vector<DynamicRecordCatalogEntry> PlayerDatabase::loadDynamicRecordCatalog()
    {
        sqlite3_stmt* s = prepare(
            "SELECT c.record_type, c.record_id, c.record_scope, c.is_persistent, c.created_at, c.updated_at,"
            " COALESCE(("
            "   SELECT COUNT(*) FROM world_dynamic_record_links l WHERE l.record_id = c.record_id"
            " ), 0)"
            " FROM world_dynamic_record_catalog c ORDER BY c.created_at, c.record_type, c.record_id");

        std::vector<DynamicRecordCatalogEntry> records;
        while (sqlite3_step(s) == SQLITE_ROW)
        {
            DynamicRecordCatalogEntry record;
            auto textCol = [&](int i) -> std::string {
                const char* t = reinterpret_cast<const char*>(sqlite3_column_text(s, i));
                return t ? t : "";
            };

            record.recordType = textCol(0);
            record.recordId = textCol(1);
            record.recordScope = textCol(2);
            record.persistent = sqlite3_column_int(s, 3) != 0;
            record.createdAt = sqlite3_column_int64(s, 4);
            record.updatedAt = sqlite3_column_int64(s, 5);
            record.linkCount = sqlite3_column_int64(s, 6);
            records.push_back(std::move(record));
        }

        sqlite3_finalize(s);
        return records;
    }

    void PlayerDatabase::upsertDynamicRecordCatalog(const DynamicRecordCatalogEntry& record)
    {
        sqlite3_stmt* s = prepare(
            "INSERT INTO world_dynamic_record_catalog(record_type, record_id, record_scope, is_persistent, created_at, "
            "updated_at)"
            " VALUES(?1, ?2, ?3, ?4, COALESCE(?5, strftime('%s', 'now')), ?6)"
            " ON CONFLICT(record_type, record_id) DO UPDATE SET"
            " record_scope=excluded.record_scope,"
            " is_persistent=excluded.is_persistent,"
            " updated_at=excluded.updated_at");

        const int64_t createdAt = record.createdAt != 0 ? record.createdAt : static_cast<int64_t>(std::time(nullptr));
        const int64_t updatedAt = record.updatedAt != 0 ? record.updatedAt : static_cast<int64_t>(std::time(nullptr));

        sqlite3_bind_text(s, 1, record.recordType.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, record.recordId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, record.recordScope.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 4, record.persistent ? 1 : 0);
        sqlite3_bind_int64(s, 5, createdAt);
        sqlite3_bind_int64(s, 6, updatedAt);
        checkSqlite(sqlite3_step(s), mDb, "upsertDynamicRecordCatalog");
        sqlite3_finalize(s);
    }

    void PlayerDatabase::deleteDynamicRecordCatalog(std::string_view recordType, std::string_view recordId)
    {
        sqlite3_stmt* s = prepare("DELETE FROM world_dynamic_record_catalog WHERE record_type=?1 AND record_id=?2");
        sqlite3_bind_text(s, 1, recordType.data(), static_cast<int>(recordType.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, recordId.data(), static_cast<int>(recordId.size()), SQLITE_TRANSIENT);
        checkSqlite(sqlite3_step(s), mDb, "deleteDynamicRecordCatalog");
        sqlite3_finalize(s);
    }

    void PlayerDatabase::deleteDynamicRecordLinks(std::string_view recordId)
    {
        sqlite3_stmt* s = prepare("DELETE FROM world_dynamic_record_links WHERE record_id=?1");
        sqlite3_bind_text(s, 1, recordId.data(), static_cast<int>(recordId.size()), SQLITE_TRANSIENT);
        checkSqlite(sqlite3_step(s), mDb, "deleteDynamicRecordLinks");
        sqlite3_finalize(s);
    }

    void PlayerDatabase::upsertSpawnedActorDynamicRecordLink(
        std::string_view recordId, std::string_view cellId, uint32_t mpNum)
    {
        if (recordId.empty() || cellId.empty() || mpNum == 0)
            return;

        const std::string ownerA = std::to_string(mpNum);

        sqlite3_stmt* insertLink = prepare(
            "INSERT OR REPLACE INTO world_dynamic_record_links(record_id, link_kind, owner_a, owner_b, owner_c, "
            "owner_index)"
            " VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
        insertDynamicRecordLink(mDb, insertLink, recordId, "spawned_actor", ownerA, cellId, "", 0);
        sqlite3_finalize(insertLink);
    }

    void PlayerDatabase::deleteSpawnedActorDynamicRecordLink(uint32_t mpNum, std::string_view cellId)
    {
        if (mpNum == 0)
            return;

        const std::string ownerA = std::to_string(mpNum);

        sqlite3_stmt* s = prepare(
            "DELETE FROM world_dynamic_record_links"
            " WHERE link_kind=?1 AND owner_a=?2 AND (?3='' OR owner_b=?3)");
        sqlite3_bind_text(s, 1, "spawned_actor", -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, ownerA.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, cellId.data(), static_cast<int>(cellId.size()), SQLITE_TRANSIENT);
        checkSqlite(sqlite3_step(s), mDb, "deleteSpawnedActorDynamicRecordLink");
        sqlite3_finalize(s);
    }

    std::size_t PlayerDatabase::deleteSpawnedActorDynamicRecordLinksForCell(std::string_view cellId)
    {
        if (cellId.empty())
            return 0;

        sqlite3_stmt* s = prepare(
            "DELETE FROM world_dynamic_record_links WHERE link_kind=?1 AND owner_b=?2");
        sqlite3_bind_text(s, 1, "spawned_actor", -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, cellId.data(), static_cast<int>(cellId.size()), SQLITE_TRANSIENT);
        checkSqlite(sqlite3_step(s), mDb, "deleteSpawnedActorDynamicRecordLinksForCell");
        const std::size_t removed = static_cast<std::size_t>(sqlite3_changes(mDb));
        sqlite3_finalize(s);
        return removed;
    }

    void PlayerDatabase::clearSpawnedActorDynamicRecordLinks()
    {
        sqlite3_stmt* s = prepare("DELETE FROM world_dynamic_record_links WHERE link_kind=?1");
        sqlite3_bind_text(s, 1, "spawned_actor", -1, SQLITE_STATIC);
        checkSqlite(sqlite3_step(s), mDb, "clearSpawnedActorDynamicRecordLinks");
        sqlite3_finalize(s);
    }

    void PlayerDatabase::replaceDynamicRecordDependencies(std::string_view ownerRecordType,
        std::string_view ownerRecordId, const std::vector<std::string>& dependencyRecordIds)
    {
        sqlite3_stmt* clearLinks = prepare(
            "DELETE FROM world_dynamic_record_links"
            " WHERE link_kind=?1 AND owner_a=?2 AND owner_b=?3 AND owner_c=?4");
        clearDynamicRecordLinksForOwner(mDb, clearLinks, "record_dependency", ownerRecordType, ownerRecordId, "");
        sqlite3_finalize(clearLinks);

        if (dependencyRecordIds.empty())
            return;

        sqlite3_stmt* insertLink = prepare(
            "INSERT OR REPLACE INTO world_dynamic_record_links(record_id, link_kind, owner_a, owner_b, owner_c, "
            "owner_index)"
            " VALUES(?1, ?2, ?3, ?4, ?5, ?6)");

        int64_t ownerIndex = 0;
        for (const auto& dependencyRecordId : dependencyRecordIds)
        {
            insertDynamicRecordLink(mDb, insertLink, dependencyRecordId, "record_dependency", ownerRecordType,
                ownerRecordId, "", ownerIndex++);
        }

        sqlite3_finalize(insertLink);
    }

    std::vector<int64_t> PlayerDatabase::listCharactersWithSavedItems()
    {
        sqlite3_stmt* s
            = prepare("SELECT id FROM characters WHERE inventory_saved != 0 OR equipment_saved != 0 ORDER BY id");

        std::vector<int64_t> ids;
        while (sqlite3_step(s) == SQLITE_ROW)
            ids.push_back(sqlite3_column_int64(s, 0));

        sqlite3_finalize(s);
        return ids;
    }

    std::optional<std::string> PlayerDatabase::loadCharacterLuaStorageValue(
        int64_t characterId, std::string_view storageNamespace, std::string_view key)
    {
        if (characterId <= 0 || storageNamespace.empty() || key.empty())
            return std::nullopt;

        sqlite3_stmt* s = prepare(
            "SELECT value FROM character_lua_storage"
            " WHERE character_id=?1 AND storage_namespace=?2 AND storage_key=?3");
        sqlite3_bind_int64(s, 1, characterId);
        sqlite3_bind_text(s, 2, storageNamespace.data(), static_cast<int>(storageNamespace.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);

        std::optional<std::string> result;
        const int rc = sqlite3_step(s);
        if (rc == SQLITE_ROW)
        {
            const void* blob = sqlite3_column_blob(s, 0);
            const int bytes = sqlite3_column_bytes(s, 0);
            result = blob && bytes > 0 ? std::string(static_cast<const char*>(blob), static_cast<std::size_t>(bytes))
                                       : std::string();
        }
        else
            checkSqlite(rc, mDb, "loadCharacterLuaStorageValue");

        sqlite3_finalize(s);
        return result;
    }

    void PlayerDatabase::saveCharacterLuaStorageValue(
        int64_t characterId, std::string_view storageNamespace, std::string_view key, std::string_view value)
    {
        if (characterId <= 0 || storageNamespace.empty() || key.empty())
            return;

        sqlite3_stmt* s = prepare(
            "INSERT INTO character_lua_storage(character_id, storage_namespace, storage_key, value, updated_at)"
            " VALUES(?1, ?2, ?3, ?4, ?5)"
            " ON CONFLICT(character_id, storage_namespace, storage_key) DO UPDATE SET"
            " value=excluded.value, updated_at=excluded.updated_at");
        sqlite3_bind_int64(s, 1, characterId);
        sqlite3_bind_text(s, 2, storageNamespace.data(), static_cast<int>(storageNamespace.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob(s, 4, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 5, static_cast<int64_t>(std::time(nullptr)));
        checkSqlite(sqlite3_step(s), mDb, "saveCharacterLuaStorageValue");
        sqlite3_finalize(s);
    }

    bool PlayerDatabase::deleteCharacterLuaStorageValue(
        int64_t characterId, std::string_view storageNamespace, std::string_view key)
    {
        if (characterId <= 0 || storageNamespace.empty() || key.empty())
            return false;

        sqlite3_stmt* s = prepare(
            "DELETE FROM character_lua_storage"
            " WHERE character_id=?1 AND storage_namespace=?2 AND storage_key=?3");
        sqlite3_bind_int64(s, 1, characterId);
        sqlite3_bind_text(s, 2, storageNamespace.data(), static_cast<int>(storageNamespace.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
        checkSqlite(sqlite3_step(s), mDb, "deleteCharacterLuaStorageValue");
        const bool removed = sqlite3_changes(mDb) > 0;
        sqlite3_finalize(s);
        return removed;
    }

    std::vector<DatabaseTableInfo> PlayerDatabase::listBrowsableTables()
    {
        std::vector<DatabaseTableInfo> results;
        results.reserve(sizeof(kBrowsableTableDefs) / sizeof(kBrowsableTableDefs[0]));

        for (const BrowsableTableDef& entry : kBrowsableTableDefs)
        {
            const std::string sql = "SELECT COUNT(*) FROM " + std::string(entry.name);
            sqlite3_stmt* s = prepare(sql.c_str());

            DatabaseTableInfo info;
            info.name = entry.name;
            if (sqlite3_step(s) == SQLITE_ROW)
                info.rowCount = sqlite3_column_int64(s, 0);

            sqlite3_finalize(s);
            results.push_back(std::move(info));
        }

        return results;
    }

    std::optional<DatabaseBrowsePage> PlayerDatabase::browseTable(
        std::string_view tableName, int64_t offset, int64_t limit)
    {
        const BrowsableTableDef* definition = findBrowsableTableDef(tableName);
        if (!definition)
            return std::nullopt;

        offset = std::max<int64_t>(0, offset);
        limit = std::clamp<int64_t>(limit <= 0 ? 100 : limit, 1, 500);

        DatabaseBrowsePage page;
        page.tableName = definition->name;
        page.offset = offset;
        page.limit = limit;

        {
            const std::string countSql = "SELECT COUNT(*) FROM " + std::string(definition->name);
            sqlite3_stmt* countStmt = prepare(countSql.c_str());
            if (sqlite3_step(countStmt) == SQLITE_ROW)
                page.totalRows = sqlite3_column_int64(countStmt, 0);
            sqlite3_finalize(countStmt);
        }

        const std::string querySql = "SELECT * FROM " + std::string(definition->name) + " ORDER BY "
            + definition->orderBy + " LIMIT ?1 OFFSET ?2";
        sqlite3_stmt* s = prepare(querySql.c_str());
        sqlite3_bind_int64(s, 1, limit);
        sqlite3_bind_int64(s, 2, offset);

        const int columnCount = sqlite3_column_count(s);
        page.columns.reserve(static_cast<std::size_t>(columnCount));
        for (int i = 0; i < columnCount; ++i)
            page.columns.emplace_back(sqlite3_column_name(s, i));

        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW)
        {
            std::vector<std::optional<std::string>> row;
            row.reserve(static_cast<std::size_t>(columnCount));

            for (int i = 0; i < columnCount; ++i)
                row.push_back(sqliteColumnToString(s, i));

            page.rows.push_back(std::move(row));
        }

        sqlite3_finalize(s);
        checkSqlite(rc, mDb, "browseTable");
        return page;
    }

    int64_t PlayerDatabase::addKeypair(int64_t accountId, std::string_view publicKey, std::string_view label)
    {
        sqlite3_stmt* s = prepare(
            "INSERT INTO account_keypairs(account_id, public_key, label, created_at)"
            " VALUES(?1, ?2, ?3, ?4)");
        sqlite3_bind_int64(s, 1, accountId);
        sqlite3_bind_text(s, 2, publicKey.data(), static_cast<int>(publicKey.size()), SQLITE_STATIC);
        sqlite3_bind_text(s, 3, label.data(), static_cast<int>(label.size()), SQLITE_STATIC);
        sqlite3_bind_int64(s, 4, static_cast<int64_t>(std::time(nullptr)));
        checkSqlite(sqlite3_step(s), mDb, "insert keypair");
        sqlite3_finalize(s);
        const int64_t id = sqlite3_last_insert_rowid(mDb);
        Log(Debug::Info) << "[PlayerDB] keypair registered for account=" << accountId << " label='" << label << "'";
        return id;
    }

    std::vector<PlayerDatabase::KeypairEntry> PlayerDatabase::listKeypairs(int64_t accountId)
    {
        std::vector<KeypairEntry> results;
        sqlite3_stmt* s = prepare("SELECT id, public_key, label FROM account_keypairs WHERE account_id=?1");
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
            e.label = col(2);
            results.push_back(std::move(e));
        }
        sqlite3_finalize(s);
        return results;
    }

    int64_t PlayerDatabase::lookupAccountByKeypair(std::string_view publicKey)
    {
        sqlite3_stmt* s = prepare("SELECT account_id FROM account_keypairs WHERE public_key=?1 LIMIT 1");
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
            if (t)
                name = t;
        }
        sqlite3_finalize(s);
        return name;
    }

    void PlayerDatabase::removeKeypair(std::string_view publicKey)
    {
        sqlite3_stmt* s = prepare("DELETE FROM account_keypairs WHERE public_key=?1");
        sqlite3_bind_text(s, 1, publicKey.data(), static_cast<int>(publicKey.size()), SQLITE_STATIC);
        checkSqlite(sqlite3_step(s), mDb, "removeKeypair");
        sqlite3_finalize(s);
    }

    std::vector<PlayerDatabase::CharacterSummary> PlayerDatabase::listCharacters(int64_t accountId)
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
            cs.name = col(0);
            cs.race = col(1);
            cs.className = col(2);
            int64_t ts = sqlite3_column_int64(s, 3);
            cs.lastSeen = ts ? std::to_string(ts) : "";
            cs.isNew = sqlite3_column_int(s, 4) != 0;
            results.push_back(std::move(cs));
        }
        sqlite3_finalize(s);
        return results;
    }

} // namespace mwmp
