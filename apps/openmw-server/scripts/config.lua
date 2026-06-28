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
-- LuaJIT has shown native stack corruption under repeated concurrent script
-- events on the AArch64 dedicated server. The interpreter has ample headroom
-- for the measured server workload and avoids process-level JIT failures.
Config.LUA_JIT_ENABLED = false
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

Config.SPAWN_CELL = "surf_kitsune_mw"
Config.DEFAULT_SPAWN = {
    cell = "surf_kitsune_mw",
    position = {
        x = -0.8005232810974121,
        y = -2016.4405517578125,
        z = 192.99996948242188,
        rx = 0.27265647053718567,
        ry = 0.0,
        rz = 0.01220219861716032,
    },
}
Config.DEFAULT_PLAYER_MARKS = {
    {
        name = "kitsune",
        cell = "surf_kitsune_mw",
        position = {
            x = -0.8005232810974121,
            y = -2016.4405517578125,
            z = 192.99996948242188,
            rx = 0.27265647053718567,
            ry = 0.0,
            rz = 0.01220219861716032,
        },
    },
    {
        name = "utopia",
        cell = "surf_utopia_mw",
        position = {
            x = -27521.025390625,
            y = -839.512939453125,
            z = 25001.966796875,
            rx = 0.4054699242115021,
            ry = 0.0,
            rz = 2.0280449390411377,
        },
    },
    {
        name = "mesa",
        cell = "surf_mesa_mw",
        position = {
            x = 84.77737426757812,
            y = -8537.3974609375,
            z = 27873.416015625,
            rx = 0.22188639640808105,
            ry = 0.0,
            rz = 0.11640609055757523,
        },
    },
}
Config.SPAWNED_ACTOR_DEFAULT_PERSISTENT = true

------------------------------------------------------------------------
-- Destructible spawners
------------------------------------------------------------------------

Config.SPAWNER_BASE_CREATURE = "mudcrab"
Config.SPAWNER_MODEL = "meshes\\i\\active_port_Indo.NIF"
Config.SPAWNER_HEALTH = 50
Config.SPAWNER_DEFAULT_COUNT = 1
Config.SPAWNER_DEFAULT_RESPAWN_ON_CELL_RESET = false
Config.SPAWNER_SPAWNED_ACTOR_PERSISTENT = false
Config.SPAWNER_SPAWN_INTERVAL_SECONDS = 10
Config.SPAWNER_SPAWN_CONFIRM_TIMEOUT_SECONDS = 8

------------------------------------------------------------------------
-- Chat & commands
------------------------------------------------------------------------

Config.COMMAND_PREFIX = "/"
Config.ANNOUNCE_JOIN_LEAVE = true
Config.ANNOUNCE_PLAYER_DEATHS = true
Config.LOG_CHAT = false
-- 0 disables speech rate limiting. Set above 0 to rate-limit repeated /s commands.
Config.SPEECH_COOLDOWN_SECONDS = 0
Config.SPEECH_COOLDOWN_NOTICE_SECONDS = 1.5

------------------------------------------------------------------------
-- Bardcraft
------------------------------------------------------------------------

-- Safe public-server defaults: clients must already have matching songs locally.
Config.Bardcraft = {
    requireLocalSongHash = true,
    communitySongSharingMode = false,
    allowServerHostedMidiDownloads = false,
    allowPlayerSongUpload = false,
    allowImportedMidiLiveRelayFallback = false,
    -- Optional external Discord/Drive/community-pack URL shown for missing songs.
    communitySongPackUrl = "",
}

-- Only enable community song sharing if the server has permission to distribute
-- the MIDI files, or the operator accepts responsibility for user-supplied/community sharing.

------------------------------------------------------------------------
-- Moderation
------------------------------------------------------------------------

Config.MAX_ADMIN_LOGIN_ATTEMPTS = 5

return Config
