-- config.lua
-- OpenMW Multiplayer — Server Operator Configuration
--
-- Loaded as a sandboxed Lua module via `require("config")`.
-- Return a plain table of operator-editable settings.

local Config = {}

------------------------------------------------------------------------
-- General
------------------------------------------------------------------------

Config.MOTD = "Welcome!  Type !help for a list of commands."
Config.ADMIN_PASSWORD = "changeme"

------------------------------------------------------------------------
-- World
------------------------------------------------------------------------

Config.START_HOUR = 8.0
Config.TIME_SCALE = 30.0
Config.LUA_TICK_RATE = 20
Config.LUA_TICK_DIAGNOSTICS_INTERVAL = 5
Config.LUA_SLOW_TICK_MS = 8

------------------------------------------------------------------------
-- Characters
------------------------------------------------------------------------

Config.MAX_CHARS_PER_ACCOUNT = 5

------------------------------------------------------------------------
-- Spawn
------------------------------------------------------------------------

Config.SPAWN_CELL = "mournhold"

------------------------------------------------------------------------
-- Chat & commands
------------------------------------------------------------------------

Config.COMMAND_PREFIX = "!"
Config.ANNOUNCE_JOIN_LEAVE = true
Config.LOG_CHAT = false

------------------------------------------------------------------------
-- Moderation
------------------------------------------------------------------------

Config.MAX_ADMIN_LOGIN_ATTEMPTS = 5

return Config
