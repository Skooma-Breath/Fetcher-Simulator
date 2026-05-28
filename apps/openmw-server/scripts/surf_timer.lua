local mp = require("mp")

local M = {}

local activeRuns = {}

local function playerName(player)
    return player and player.name or "Player"
end

local function elapsedSeconds(startTime, endTime)
    return endTime - startTime
end

local function formatTime(seconds)
    local minutes = math.floor(seconds / 60)
    local remainder = seconds - (minutes * 60)
    if minutes > 0 then
        return string.format("%d:%06.3f", minutes, remainder)
    end
    return string.format("%.3f", remainder)
end

local DEBUG_TIMER = false

local function logTimer(message)
    if DEBUG_TIMER then
        print("[SurfTimer] " .. tostring(message))
    end
end

local function getPlayerFromPayload(data)
    if not data or not data.pid then
        return nil
    end
    return mp.getPlayer(data.pid)
end

local function mapFromPayload(data)
    local map = tostring(data and data.map or "")
    if map == "" then
        return nil
    end
    return map
end

local function sameMap(player, data)
    if not player or not data then
        return false
    end

    local map = mapFromPayload(data)
    if not map then
        return false
    end

    return player.cell == map
end

function M.handleTrigger(data)
    logTimer(string.format("raw event trigger=%s map=%s pid=%s pos=%.2f %.2f %.2f", tostring(data and data.trigger), tostring(data and data.map), tostring(data and data.pid), tonumber(data and data.x) or 0, tonumber(data and data.y) or 0, tonumber(data and data.z) or 0))
    local player = getPlayerFromPayload(data)
    local map = mapFromPayload(data)

    if not sameMap(player, data) then
        logTimer(string.format("ignored by sameMap playerCell=%s dataMap=%s", tostring(player and player.cell), tostring(data and data.map)))
        return
    end

    local trigger = string.lower(tostring(data.trigger or ""))
    local now = mp.getUptime()

    if trigger == "start" then
        activeRuns[player.guid] = {
            startTime = now,
            map = map,
        }
        logTimer(string.format("%s start map=%s pos=%.2f %.2f %.2f", playerName(player), tostring(map), tonumber(data.x) or 0, tonumber(data.y) or 0, tonumber(data.z) or 0))
        -- Silent start: only finish prints to chat.
        return
    end

    if trigger == "finish" then
        local run = activeRuns[player.guid]
        if not run then
            player:sendMessage("Surf finish ignored: no active timer.")
            return
        end

        if run.map ~= map then
            logTimer(string.format("finish ignored: active map=%s finish map=%s player=%s", tostring(run.map), tostring(map), playerName(player)))
            player:sendMessage("Surf finish ignored: active timer is for another map.")
            return
        end

        local elapsed = elapsedSeconds(run.startTime, now)
        activeRuns[player.guid] = nil
        local formatted = formatTime(elapsed)
        player:sendMessage("Surf finished in " .. formatted .. "s.")
        mp.broadcast(string.format("%s finished %s in %ss", playerName(player), map, formatted))
        return
    end

    logTimer("ignored unknown trigger=" .. tostring(trigger))
end

function M.onPlayerDisconnect(data)
    if data and data.guid then
        activeRuns[data.guid] = nil
    end
end

return M
