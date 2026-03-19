-- server-scripts/core.lua
-- Reference server script for TES3MP (OpenMW-native rebuild).
-- Edit freely — this file is the starting point for server customisation.
--
-- Callbacks fired by the server:
--   OnServerInit()
--   OnPlayerConnect(player)
--   OnPlayerDisconnect(player, reason)
--   OnPlayerCellChange(player, newCell, oldCell)
--   OnPlayerSendMessage(player, msg)  → return false to suppress relay
--   OnDoorState(cellId, refId, isOpen)
--   OnWorldWeather(region, current, next, transitionFactor)
--   OnServerTick(dt)
--
-- Player fields:  player.name  player.guid  player.cell  player.position {x,y,z}
-- Player methods: player:sendMessage(text)  player:kick(reason)
--                 player:setData(key, value)  player:getData(key)
-- Server funcs:   mp.broadcast(text)  mp.getPlayers()  mp.getPlayerCount()
--                 mp.getPlayer(guid)  mp.getUptime()  mp.log(text)
--                 mp.getWorldTime()  mp.setWorldTime(hour)

------------------------------------------------------------------------
-- Config
------------------------------------------------------------------------
local ADMIN_PASSWORD = "changeme"   -- change before going live
local MAX_PLAYERS    = 32           -- informational; hard cap is in C++
local MOTD           = "Welcome to the server!  Type !help for commands."

------------------------------------------------------------------------
-- State
------------------------------------------------------------------------
local admins = {}   -- guid → true; cleared on disconnect

------------------------------------------------------------------------
-- Helpers
------------------------------------------------------------------------
local function findPlayerByName(name)
    for _, p in ipairs(mp.getPlayers()) do
        if p.name == name then return p end
    end
    return nil
end

local function isAdmin(player)
    return admins[player.guid] == true
end

local function playerList()
    local names = {}
    for _, p in ipairs(mp.getPlayers()) do
        table.insert(names, p.name)
    end
    return table.concat(names, ", ")
end

------------------------------------------------------------------------
-- OnServerInit
------------------------------------------------------------------------
function OnServerInit()
    mp.log("[core] Server ready — " .. mp.getPlayerCount() .. " player(s) connected.")
end

------------------------------------------------------------------------
-- OnPlayerConnect
------------------------------------------------------------------------
function OnPlayerConnect(player)
    mp.broadcast(">> " .. player.name .. " has joined.  ("
                 .. mp.getPlayerCount() .. "/" .. MAX_PLAYERS .. ")")
    player:sendMessage(MOTD)
    mp.log("[core] Connect: " .. player.name .. " (guid " .. player.guid .. ")")
end

------------------------------------------------------------------------
-- OnPlayerDisconnect
------------------------------------------------------------------------
function OnPlayerDisconnect(player, reason)
    admins[player.guid] = nil   -- revoke admin on disconnect
    mp.broadcast("<< " .. player.name .. " has left.  (" .. reason .. ")")
    mp.log("[core] Disconnect: " .. player.name .. " — " .. reason)
end

------------------------------------------------------------------------
-- OnPlayerCellChange
------------------------------------------------------------------------
function OnPlayerCellChange(player, newCell, oldCell)
    mp.log("[core] " .. player.name .. ": " .. oldCell .. " → " .. newCell)
end

------------------------------------------------------------------------
-- OnPlayerSendMessage
-- Return false to suppress relay to other players.
------------------------------------------------------------------------
function OnPlayerSendMessage(player, msg)

    -- ── !help ──────────────────────────────────────────────────────────
    if msg == "!help" then
        player:sendMessage(
            "Commands:  !who  !time  !uptime"
            .. (isAdmin(player) and "  !kick <name>  !settime <hour>" or "")
            .. "  !login <password>"
        )
        return false

    -- ── !who ───────────────────────────────────────────────────────────
    elseif msg == "!who" then
        player:sendMessage("Online (" .. mp.getPlayerCount() .. "): " .. playerList())
        return false

    -- ── !time ──────────────────────────────────────────────────────────
    elseif msg == "!time" then
        local h = mp.getWorldTime()
        local hour   = math.floor(h)
        local minute = math.floor((h - hour) * 60)
        player:sendMessage(string.format("Server time: %02d:%02d", hour, minute))
        return false

    -- ── !uptime ────────────────────────────────────────────────────────
    elseif msg == "!uptime" then
        local secs  = math.floor(mp.getUptime())
        local mins  = math.floor(secs / 60)
        local hours = math.floor(mins / 60)
        player:sendMessage(string.format("Uptime: %dh %02dm %02ds",
                           hours, mins % 60, secs % 60))
        return false

    -- ── !login <password> ──────────────────────────────────────────────
    elseif msg:sub(1, 7) == "!login " then
        local pw = msg:sub(8)
        if pw == ADMIN_PASSWORD then
            admins[player.guid] = true
            player:sendMessage("You are now an admin.")
            mp.log("[core] Admin login: " .. player.name)
        else
            player:sendMessage("Wrong password.")
            mp.log("[core] Failed admin login attempt: " .. player.name)
        end
        return false

    -- ── !kick <name>  (admin only) ─────────────────────────────────────
    elseif msg:sub(1, 6) == "!kick " then
        if not isAdmin(player) then
            player:sendMessage("Permission denied.")
            return false
        end
        local targetName = msg:sub(7)
        local target = findPlayerByName(targetName)
        if target then
            mp.broadcast(targetName .. " was kicked by " .. player.name .. ".")
            target:kick("Kicked by " .. player.name)
            mp.log("[core] Kick: " .. targetName .. " by " .. player.name)
        else
            player:sendMessage("Player not found: " .. targetName)
        end
        return false

    -- ── !settime <hour>  (admin only) ──────────────────────────────────
    elseif msg:sub(1, 9) == "!settime " then
        if not isAdmin(player) then
            player:sendMessage("Permission denied.")
            return false
        end
        local hour = tonumber(msg:sub(10))
        if hour and hour >= 0 and hour < 24 then
            mp.setWorldTime(hour)
            mp.broadcast("Server time set to " .. string.format("%02d:00", math.floor(hour))
                         .. " by " .. player.name .. ".")
            mp.log("[core] settime " .. hour .. " by " .. player.name)
        else
            player:sendMessage("Usage: !settime <0-23>")
        end
        return false

    -- ── unknown ! command ──────────────────────────────────────────────
    elseif msg:sub(1, 1) == "!" then
        player:sendMessage("Unknown command.  Type !help for a list.")
        return false
    end

    -- Not a command — relay normally.
end

------------------------------------------------------------------------
-- OnDoorState  (informational log only)
------------------------------------------------------------------------
function OnDoorState(cellId, refId, isOpen)
    mp.log(string.format("[core] Door %s in '%s' is now %s",
           refId, cellId, isOpen and "open" or "closed"))
end

------------------------------------------------------------------------
-- OnWorldWeather  (informational log only)
------------------------------------------------------------------------
function OnWorldWeather(region, current, next, transitionFactor)
    if next >= 0 then
        mp.log(string.format("[core] Weather '%s': %d → %d (%.2f)",
               region, current, next, transitionFactor))
    else
        mp.log(string.format("[core] Weather '%s': %d", region, current))
    end
end

------------------------------------------------------------------------
-- OnServerTick  — runs every server frame (~20 Hz).
-- Keep this lean; expensive work should use timers.
------------------------------------------------------------------------
local tickAccum = 0
function OnServerTick(dt)
    -- Example: log player count every 5 minutes of real time.
    tickAccum = tickAccum + dt
    if tickAccum >= 300 then
        tickAccum = 0
        mp.log("[core] Heartbeat — players: " .. mp.getPlayerCount()
               .. "  time: " .. string.format("%.1f", mp.getWorldTime()) .. "h")
    end
end
