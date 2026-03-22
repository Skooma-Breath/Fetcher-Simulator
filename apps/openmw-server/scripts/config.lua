-- config.lua
-- OpenMW Multiplayer — Server Operator Configuration
--
-- This file is loaded by core.lua and contains all the settings
-- a server operator is likely to want to change.  You should not
-- need to touch core.lua for normal configuration.
--
-- Reload:  restart the server to apply changes.

Config = {}

------------------------------------------------------------------------
-- General
------------------------------------------------------------------------

-- Message of the day shown to players when they connect.
Config.MOTD = "Welcome!  Type !help for a list of commands."

-- Password that grants in-session admin rights via the !login command.
-- Change this before going live.  It is NOT a player account password.
Config.ADMIN_PASSWORD = "changeme"

------------------------------------------------------------------------
-- World
------------------------------------------------------------------------

-- Starting time of day (0-24).  The server clock starts here each run.
-- This does not affect the in-game calendar date.
Config.START_HOUR = 8.0

-- Time scale: how many in-game seconds pass per real second.
-- 30 is Morrowind's default (1 real minute = 30 game minutes).
-- Set to 1 for real-time, 0 to freeze time.
Config.TIME_SCALE = 30.0

------------------------------------------------------------------------
-- Characters
------------------------------------------------------------------------

-- Maximum number of character slots per account.
-- Set to 0 for unlimited.
Config.MAX_CHARS_PER_ACCOUNT = 5

------------------------------------------------------------------------
-- Spawn
------------------------------------------------------------------------

-- Cell name (or "x,y" grid coords) where new characters first appear.
-- Interior examples:  "ToddTest"  "Seyda Neen, Arrille's Tradehouse"
-- Exterior examples:  "-2,-9"  (the grid cell Seyda Neen is in)
-- This is also the fallback if the server cannot find the player's
-- saved cell on reconnect.  The C++ server reads this value directly
-- from the Lua global Config table after loading scripts.
Config.SPAWN_CELL = "mournhold"

------------------------------------------------------------------------
-- Chat & commands
------------------------------------------------------------------------

-- Prefix character for server commands.
Config.COMMAND_PREFIX = "!"

-- Whether to broadcast join/leave messages to all players.
Config.ANNOUNCE_JOIN_LEAVE = true

-- Whether to log every chat message to the server log.
Config.LOG_CHAT = false

------------------------------------------------------------------------
-- Moderation
------------------------------------------------------------------------

-- Maximum number of failed !login attempts before an IP is rate-limited.
-- (Future: will feed into mp.banIP once that binding exists.)
Config.MAX_ADMIN_LOGIN_ATTEMPTS = 5

