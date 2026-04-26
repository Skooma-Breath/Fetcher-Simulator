-- config.lua
-- OpenMW Multiplayer — Server Operator Configuration
--
-- Loaded as a sandboxed Lua module via `require("config")`.
-- Return a plain table of operator-editable settings.

local Config = {}

------------------------------------------------------------------------
-- General
------------------------------------------------------------------------

Config.MOTD = "Welcome!  Type /help for a list of commands."
Config.ADMIN_PASSWORD = "changeme"

------------------------------------------------------------------------
-- World
------------------------------------------------------------------------

Config.START_HOUR = 8.0
Config.TIME_SCALE = 30.0
-- Periodic server Lua still runs at 60 Hz for ordinary script events.
-- Immediate intent evaluation wakes the Lua thread on demand for hot paths
-- like Activate, so these settings mainly cover the non-blocking tick work.
Config.LUA_TICK_RATE = 60
Config.LUA_TICK_DIAGNOSTICS_INTERVAL = 5
Config.LUA_SLOW_TICK_MS = 8
-- Immediate intent evaluation wakes the Lua thread on demand and waits
-- briefly for a decision instead of waiting for the next periodic tick.
Config.IMMEDIATE_INTENT_TIMEOUT_MS = 50
-- Keep this off until the server can verify static-world refs from a content index.
Config.ALLOW_UNVERIFIED_ACTIVATE = false
Config.GENERATED_RECORD_ID_PREFIX = "$custom"
Config.ADMIN_HTTP_ENABLED = true
Config.ADMIN_HTTP_HOST = "127.0.0.1"
Config.ADMIN_HTTP_PORT = 8081
Config.ADMIN_HTTP_TIMEOUT_MS = 250

------------------------------------------------------------------------
-- Characters
------------------------------------------------------------------------

Config.MAX_CHARS_PER_ACCOUNT = 5

------------------------------------------------------------------------
-- Spawn
------------------------------------------------------------------------

Config.SPAWN_CELL = "mournhold"
Config.SPAWNED_ACTOR_DEFAULT_PERSISTENT = true

------------------------------------------------------------------------
-- Destructible spawners
------------------------------------------------------------------------

Config.SPAWNER_BASE_CREATURE = "mudcrab"
Config.SPAWNER_MODEL = "meshes\\i\\active_port_Indo.NIF"
Config.SPAWNER_HEALTH = 50
Config.SPAWNER_DEFAULT_COUNT = 1
Config.SPAWNER_DEFAULT_RESPAWN_ON_CELL_RESET = false
Config.SPAWNER_SPAWNED_ACTOR_PERSISTENT = true
Config.SPAWNER_SPAWN_INTERVAL_SECONDS = 10
Config.SPAWNER_SPAWN_CONFIRM_TIMEOUT_SECONDS = 8

------------------------------------------------------------------------
-- Chat & commands
------------------------------------------------------------------------

Config.COMMAND_PREFIX = "/"
Config.ANNOUNCE_JOIN_LEAVE = true
Config.LOG_CHAT = false

------------------------------------------------------------------------
-- Moderation
------------------------------------------------------------------------

Config.MAX_ADMIN_LOGIN_ATTEMPTS = 5

return Config
