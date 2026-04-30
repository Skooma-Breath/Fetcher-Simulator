local mp = require("mp")
local Config = require("config")
local recordStore = require("recordstore")

local M = {}

local STORAGE_SECTION = "DestructibleSpawners"
local DEFAULT_SPAWN_INTERVAL = 10
local DEFAULT_SPAWN_CONFIRM_TIMEOUT = 8

local stateCache

local function trim(text)
    if not text then
        return nil
    end
    return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function lower(text)
    return string.lower(trim(text) or "")
end

local function storage()
    assert(mp.storage and type(mp.storage.globalSection) == "function", "destructible_spawners.lua requires mp.storage")
    return mp.storage.globalSection(STORAGE_SECTION)
end

local function loadState()
    if stateCache ~= nil then
        return stateCache
    end

    local state = storage():getCopy("spawners")
    if type(state) ~= "table" then
        state = {}
    end
    stateCache = state
    return stateCache
end

local function saveState(state)
    stateCache = state or {}
    storage():set("spawners", stateCache)
end

local function normalizeName(name)
    name = lower(name)
    if name == "" then
        return nil
    end
    return name
end

local function recordIdForName(name)
    local normalized = normalizeName(name) or "spawner"
    local safe = normalized:gsub("[^%w_%-]", "_")
    return "spawner_" .. safe
end

local function nameForRecordId(recordId)
    recordId = trim(recordId)
    if not recordId or recordId == "" then
        return nil
    end
    if recordId:sub(1, #"spawner_") ~= "spawner_" then
        return nil
    end
    return normalizeName(recordId:sub(#"spawner_" + 1):gsub("_", " "))
end

local function spawnerRecordExists(recordId)
    recordId = trim(recordId)
    if not recordId or recordId == "" then
        return false
    end
    for _, entry in ipairs(recordStore.list("creature") or {}) do
        if entry.recordId == recordId then
            return true
        end
    end
    return false
end

local function normalizeMeshPath(path)
    path = trim(path) or ""
    if path == "" then
        path = "i\\active_port_Indo.NIF"
    end

    local normalized = path:gsub("/", "\\")
    if lower(normalized):sub(1, #"meshes\\") == "meshes\\" then
        return normalized
    end

    return "meshes\\" .. normalized
end

local function copyPosition(position)
    position = position or {}
    return {
        x = tonumber(position.x) or 0,
        y = tonumber(position.y) or 0,
        z = tonumber(position.z) or 0,
        rx = tonumber(position.rx) or 0,
        ry = tonumber(position.ry) or 0,
        rz = tonumber(position.rz) or 0,
    }
end

local function tableCount(value)
    local count = 0
    if type(value) ~= "table" then
        return count
    end

    for _, _ in pairs(value) do
        count = count + 1
    end
    return count
end

local function positionsDistanceSquared(left, right)
    left = left or {}
    right = right or {}
    local dx = (tonumber(left.x) or 0) - (tonumber(right.x) or 0)
    local dy = (tonumber(left.y) or 0) - (tonumber(right.y) or 0)
    local dz = (tonumber(left.z) or 0) - (tonumber(right.z) or 0)
    return dx * dx + dy * dy + dz * dz
end

local function getSpawnInterval()
    return math.max(0.1, tonumber(Config.SPAWNER_SPAWN_INTERVAL_SECONDS) or DEFAULT_SPAWN_INTERVAL)
end

local function getSpawnConfirmTimeout()
    return math.max(1, tonumber(Config.SPAWNER_SPAWN_CONFIRM_TIMEOUT_SECONDS) or DEFAULT_SPAWN_CONFIRM_TIMEOUT)
end

local function normalizeSpawnedMpNums(spawner)
    local normalized = {}
    if type(spawner.spawnedMpNums) == "table" then
        for mpNum, entry in pairs(spawner.spawnedMpNums) do
            local numericMpNum = tonumber(mpNum)
            if numericMpNum and numericMpNum ~= 0 then
                if type(entry) ~= "table" then
                    entry = {}
                end
                entry.refId = trim(entry.refId) or spawner.spawnRefId
                entry.cellId = trim(entry.cellId) or spawner.cellId
                normalized[tostring(numericMpNum)] = entry
            end
        end
    end
    spawner.spawnedMpNums = normalized
end

local function normalizePendingSpawns(spawner)
    local normalized = {}
    if type(spawner.pendingSpawns) == "table" then
        for _, entry in pairs(spawner.pendingSpawns) do
            if type(entry) == "table" then
                local refId = trim(entry.refId) or spawner.spawnRefId
                local cellId = trim(entry.cellId) or spawner.cellId
                if refId and refId ~= "" and cellId and cellId ~= "" then
                    normalized[#normalized + 1] = {
                        refId = refId,
                        cellId = cellId,
                        position = copyPosition(entry.position),
                        queued = entry.queued == true,
                        age = math.max(0, tonumber(entry.age) or 0),
                    }
                end
            end
        end
    end
    spawner.pendingSpawns = normalized
end

local function refreshCounts(spawner)
    spawner.liveCount = tableCount(spawner.spawnedMpNums)
    spawner.pendingCount = #spawner.pendingSpawns
    spawner.remaining = math.max(0,
        (tonumber(spawner.spawnCount) or 0) - spawner.liveCount - spawner.pendingCount)
end

local function normalizeSpawnerState(spawner)
    spawner.name = normalizeName(spawner.name) or spawner.name
    spawner.recordId = trim(spawner.recordId) or (spawner.name and recordIdForName(spawner.name) or "")
    spawner.spawnRefId = trim(spawner.spawnRefId) or ""
    spawner.cellId = trim(spawner.cellId) or ""
    spawner.position = copyPosition(spawner.position)
    spawner.spawnCount = math.max(1,
        math.floor(tonumber(spawner.spawnCount) or tonumber(Config.SPAWNER_DEFAULT_COUNT) or 1))
    spawner.spawnInterval = math.max(0.1, tonumber(spawner.spawnInterval) or getSpawnInterval())
    spawner.spawnTimer = math.max(0, tonumber(spawner.spawnTimer) or spawner.spawnInterval)
    if spawner.spawnedPersistent == nil then
        spawner.spawnedPersistent = Config.SPAWNER_SPAWNED_ACTOR_PERSISTENT ~= false
    else
        spawner.spawnedPersistent = spawner.spawnedPersistent == true
    end
    spawner.persistent = spawner.persistent ~= false
    spawner.respawnOnCellReset = spawner.respawnOnCellReset == true
    spawner.destroyed = spawner.destroyed == true
    spawner.active = spawner.active == true
    spawner.actorMpNum = tonumber(spawner.actorMpNum) or 0

    normalizeSpawnedMpNums(spawner)
    normalizePendingSpawns(spawner)
    refreshCounts(spawner)
    return spawner
end

local function ensureSpawnerRecord(name, persistent)
    local health = math.max(1, math.floor(tonumber(Config.SPAWNER_HEALTH) or 50))
    return recordStore.ensure("creature", {
        baseId = Config.SPAWNER_BASE_CREATURE or "mudcrab",
        name = "Spawner: " .. name,
        model = normalizeMeshPath(Config.SPAWNER_MODEL),
        level = 1,
        health = health,
        magicka = 0,
        fatigue = health,
        soulValue = 0,
        baseGold = 0,
        combatSkill = 0,
        magicSkill = 0,
        stealthSkill = 0,
        attack = { 0, 0, 0, 0, 0, 0 },
        canWalk = false,
        canSwim = false,
        canFly = false,
        canUseWeapons = false,
        isRespawning = false,
    }, {
        scope = "permanent",
        persistent = persistent ~= false,
        recordId = recordIdForName(name),
        allowReuse = false,
    })
end

local function spawnSpawnerActor(spawner)
    if not spawner or not spawner.recordId or spawner.recordId == "" or not spawner.cellId or spawner.cellId == "" then
        return false
    end

    normalizeSpawnerState(spawner)
    if spawner.destroyed then
        mp.log(string.format("[spawner] %s spawnSpawnerActor skipped because spawner is destroyed",
            tostring(spawner.name)
        ))
        return false
    end

    local stored = ensureSpawnerRecord(spawner.name, spawner.persistent)
    if not stored then
        return false
    end
    spawner.recordId = stored.recordId

    return mp.spawnActor(
        spawner.recordId,
        0,
        tonumber(spawner.actorMpNum) or 0,
        spawner.cellId,
        copyPosition(spawner.position),
        { persistent = spawner.persistent ~= false }
    )
end

local function payloadPosition(spawner, index, total)
    local angle = ((index - 1) / math.max(1, total)) * math.pi * 2
    local radius = total > 1 and 96 or 64
    local spawnPos = copyPosition(spawner.position)
    spawnPos.x = spawnPos.x + math.cos(angle) * radius
    spawnPos.y = spawnPos.y + math.sin(angle) * radius
    return spawnPos
end

local function spawnPayloadActor(spawner, position)
    return mp.spawnActor(
        spawner.spawnRefId,
        0,
        0,
        spawner.cellId,
        copyPosition(position),
        { persistent = spawner.spawnedPersistent == true }
    )
end

local function sendUsage(player, prefix)
    player:sendMessage(prefix ..
        "spawner create <name> <spawnRefId> [count] [distance] [direction] [persistent|session] [reset|noreset]")
    player:sendMessage(prefix .. "spawner move <name> [distance] [direction]")
    player:sendMessage(prefix .. "spawner count <name> <count>")
    player:sendMessage(prefix .. "spawner list | info <name> | reset <name> [count] | purge <name> | remove <name>")
end

local function parsePersistence(args, startIndex, defaultPersistent, defaultReset)
    local persistent = defaultPersistent
    local reset = defaultReset
    for index = startIndex, #args do
        local value = lower(args[index])
        if value == "persistent" or value == "persist" then
            persistent = true
        elseif value == "session" or value == "temporary" or value == "temp" then
            persistent = false
        elseif value == "reset" or value == "respawn" then
            reset = true
        elseif value == "noreset" or value == "norespawn" then
            reset = false
        end
    end
    return persistent, reset
end

local function queueSpawnIfNeeded(spawner)
    normalizeSpawnerState(spawner)
    if spawner.destroyed or spawner.remaining <= 0 then
        return false
    end

    local index = spawner.liveCount + spawner.pendingCount + 1
    local spawnPos = payloadPosition(spawner, index, spawner.spawnCount)
    spawner.pendingSpawns[#spawner.pendingSpawns + 1] = {
        refId = spawner.spawnRefId,
        cellId = spawner.cellId,
        position = spawnPos,
        queued = false,
        age = 0,
    }
    spawner.spawnTimer = spawner.spawnInterval
    refreshCounts(spawner)
    return true
end

local function flushPendingSpawns(spawner)
    local spawned = 0
    for _, pending in ipairs(spawner.pendingSpawns) do
        if not pending.queued then
            if spawnPayloadActor(spawner, pending.position) then
                pending.queued = true
                pending.age = 0
                spawned = spawned + 1
            end
            break
        end
    end
    refreshCounts(spawner)
    return spawned
end

local function expirePendingSpawns(spawner, dt)
    local changed = false
    local timeout = getSpawnConfirmTimeout()
    for index = #spawner.pendingSpawns, 1, -1 do
        local pending = spawner.pendingSpawns[index]
        if pending.queued then
            pending.age = math.max(0, (tonumber(pending.age) or 0) + dt)
            if pending.age >= timeout then
                table.remove(spawner.pendingSpawns, index)
                spawner.spawnTimer = 0
                changed = true
                mp.log(string.format("[spawner] %s timed out waiting for %s spawn confirmation",
                    tostring(spawner.name),
                    tostring(pending.refId)
                ))
            end
        end
    end

    if changed then
        refreshCounts(spawner)
    end
    return changed
end

local function findPendingSpawn(spawner, data)
    local bestIndex
    local bestDistance
    for index, pending in ipairs(spawner.pendingSpawns) do
        if pending.queued
            and lower(pending.refId) == lower(data.refId)
            and pending.cellId == data.cellId then
            local distance = positionsDistanceSquared(pending.position, data.position)
            if not bestDistance or distance < bestDistance then
                bestDistance = distance
                bestIndex = index
            end
        end
    end
    return bestIndex
end

local function purgeSpawnedActors(spawner)
    normalizeSpawnerState(spawner)

    local removed = 0
    for mpNum, _ in pairs(spawner.spawnedMpNums) do
        local numericMpNum = tonumber(mpNum)
        if numericMpNum and numericMpNum ~= 0 then
            mp.removeGameObject(numericMpNum, spawner.cellId or "")
            removed = removed + 1
        end
    end

    spawner.spawnedMpNums = {}
    spawner.pendingSpawns = {}
    spawner.spawnTimer = spawner.spawnInterval
    refreshCounts(spawner)
    return removed
end

local function createSpawner(player, options, env)
    local name = normalizeName(options and options.name)
    local spawnRefId = trim(options and options.spawnRefId)
    if not name or not spawnRefId or spawnRefId == "" then
        return false, "Spawner create requires a name and spawn refId."
    end

    local count = math.max(1,
        math.floor(tonumber(options.spawnCount or options.count) or tonumber(Config.SPAWNER_DEFAULT_COUNT) or 1))
    local distance = tonumber(options.distance) or 128
    local direction = tonumber(options.direction) or 0
    local persistent = options.persistent
    if persistent == nil then
        persistent = true
    end
    local respawnOnCellReset = options.respawnOnCellReset
    if respawnOnCellReset == nil then
        respawnOnCellReset = Config.SPAWNER_DEFAULT_RESPAWN_ON_CELL_RESET == true
    end

    local state = loadState()
    if state[name] then
        return false, "Spawner already exists: " .. name
    end

    local cellId = env.normalizeCellId(player.cell)
    if not cellId or cellId == "" then
        return false, "Unable to determine your current cell."
    end

    local stored, err = ensureSpawnerRecord(name, persistent)
    if not stored then
        return false, "Failed to create spawner record: " .. tostring(err)
    end

    local spawner = normalizeSpawnerState({
        name = name,
        recordId = stored.recordId,
        spawnRefId = spawnRefId,
        spawnCount = count,
        cellId = cellId,
        position = env.placeAtPosition(player, distance, direction),
        actorMpNum = 0,
        active = false,
        persistent = persistent ~= false,
        spawnedPersistent = persistent ~= false and Config.SPAWNER_SPAWNED_ACTOR_PERSISTENT ~= false,
        respawnOnCellReset = respawnOnCellReset == true,
        spawnInterval = getSpawnInterval(),
        spawnTimer = getSpawnInterval(),
        spawnedMpNums = {},
        pendingSpawns = {},
        destroyed = false,
    })

    state[name] = spawner
    saveState(state)

    if not spawnSpawnerActor(spawner) then
        state[name] = nil
        saveState(state)
        recordStore.remove("creature", stored.recordId, { force = true })
        return false, "Failed to queue spawner actor spawn."
    end

    saveState(state)
    return true, string.format("Created spawner %s -> %s x%d in %s.", name, spawnRefId, count, cellId)
end

local function setSpawnerCount(name, count)
    name = normalizeName(name)
    count = tonumber(count)
    if not name or not count then
        return false, "Usage: /spawner count <name> <count>"
    end

    count = math.max(1, math.floor(count))
    local state = loadState()
    local spawner = state[name]
    if not spawner then
        return false, "Spawner not found: " .. name
    end

    normalizeSpawnerState(spawner)
    spawner.spawnCount = count
    refreshCounts(spawner)
    if spawner.pendingCount == 0 and not spawner.destroyed and spawner.remaining > 0 then
        spawner.spawnTimer = math.min(spawner.spawnTimer, spawner.spawnInterval)
    end
    saveState(state)
    return true, string.format("Set spawner %s count to %d. live=%d pending=%d remaining=%d",
        name,
        count,
        spawner.liveCount,
        spawner.pendingCount,
        spawner.remaining
    )
end

local function purgeSpawner(name)
    name = normalizeName(name)
    if not name then
        return false, "Spawner name is required."
    end

    local state = loadState()
    local spawner = state[name]
    if not spawner then
        return false, "Spawner not found: " .. name
    end

    local removed = purgeSpawnedActors(spawner)
    saveState(state)
    return true, string.format("Purged %d spawned actor(s) for %s.", removed, name)
end

local function removeSpawner(name)
    name = normalizeName(name)
    if not name then
        return false, "Spawner name is required."
    end

    local state = loadState()
    local spawner = state[name]
    if not spawner then
        local orphanRecordId = recordIdForName(name)
        if spawnerRecordExists(orphanRecordId) then
            recordStore.remove("creature", orphanRecordId, { force = true })
            return true, "Removed orphaned spawner record " .. orphanRecordId .. "."
        end
        return false, "Spawner not found: " .. name
    end

    normalizeSpawnerState(spawner)
    local removedPayloads = purgeSpawnedActors(spawner)
    if spawner.actorMpNum and spawner.actorMpNum ~= 0 then
        mp.removeGameObject(spawner.actorMpNum, spawner.cellId or "")
    end
    recordStore.remove("creature", spawner.recordId, { force = true })
    state[name] = nil
    saveState(state)
    return true, string.format("Removed spawner %s and %d payload actor(s).", name, removedPayloads)
end

local function resetSpawner(name, count)
    name = normalizeName(name)
    if not name then
        return false, "Spawner name is required."
    end

    local state = loadState()
    local spawner = state[name]
    if not spawner then
        return false, "Spawner not found: " .. name
    end

    normalizeSpawnerState(spawner)
    local removedPayloads = purgeSpawnedActors(spawner)
    local newCount = tonumber(count)
    if newCount then
        spawner.spawnCount = math.max(1, math.floor(newCount))
    end
    spawner.destroyed = false
    spawner.active = false
    spawner.spawnTimer = spawner.spawnInterval
    if spawner.actorMpNum and spawner.actorMpNum ~= 0 then
        mp.removeGameObject(spawner.actorMpNum, spawner.cellId or "")
    end
    spawner.actorMpNum = 0
    refreshCounts(spawner)
    saveState(state)

    if not spawnSpawnerActor(spawner) then
        return false, "Reset spawner " .. name .. ", but failed to queue the replacement actor."
    end

    saveState(state)
    return true, string.format("Reset spawner %s and purged %d payload actor(s).", name, removedPayloads)
end

local function handleCreate(player, args, env)
    local name = normalizeName(args[2])
    local spawnRefId = trim(args[3])
    if not name or not spawnRefId or spawnRefId == "" then
        sendUsage(player, env.commandPrefix)
        return false
    end

    local countValue = tonumber(args[4])
    local count = countValue and math.max(1, math.floor(countValue)) or (tonumber(Config.SPAWNER_DEFAULT_COUNT) or 1)
    local optionIndex = countValue and 5 or 4
    local distance = 128
    if tonumber(args[optionIndex]) then
        distance = tonumber(args[optionIndex])
        optionIndex = optionIndex + 1
    end
    local direction = 0
    if tonumber(args[optionIndex]) then
        direction = tonumber(args[optionIndex])
        optionIndex = optionIndex + 1
    end
    local persistent, respawnOnCellReset = parsePersistence(
        args,
        optionIndex,
        true,
        Config.SPAWNER_DEFAULT_RESPAWN_ON_CELL_RESET == true
    )

    local ok, message = createSpawner(player, {
        name = name,
        spawnRefId = spawnRefId,
        count = count,
        distance = distance,
        direction = direction,
        persistent = persistent,
        respawnOnCellReset = respawnOnCellReset,
    }, env)
    player:sendMessage(message)
    return false
end

local function handleMove(player, args, env)
    local name = normalizeName(args[2])
    if not name then
        sendUsage(player, env.commandPrefix)
        return false
    end

    local state = loadState()
    local spawner = state[name]
    if not spawner then
        player:sendMessage("Spawner not found: " .. name)
        return false
    end

    normalizeSpawnerState(spawner)

    local distance = tonumber(args[3]) or 128
    local direction = tonumber(args[4]) or 0
    local cellId = env.normalizeCellId(player.cell)
    if not cellId or cellId == "" then
        player:sendMessage("Unable to determine your current cell.")
        return false
    end

    local removedPayloads = purgeSpawnedActors(spawner)

    if spawner.actorMpNum and spawner.actorMpNum ~= 0 then
        mp.removeGameObject(spawner.actorMpNum, spawner.cellId or cellId)
    end

    spawner.cellId = cellId
    spawner.position = env.placeAtPosition(player, distance, direction)
    spawner.active = false
    spawner.actorMpNum = 0
    spawner.destroyed = false
    spawner.spawnTimer = spawner.spawnInterval
    refreshCounts(spawner)
    saveState(state)

    if not spawnSpawnerActor(spawner) then
        player:sendMessage("Moved spawner " .. name .. ", but failed to queue the replacement actor.")
        return false
    end

    saveState(state)
    player:sendMessage(string.format("Moved spawner %s and purged %d payload actor(s).", name, removedPayloads))
    return false
end

local function handlePurge(player, args)
    local name = normalizeName(args[2])
    if not name then
        return false
    end

    local _, message = purgeSpawner(name)
    player:sendMessage(message)
    return false
end

local function handleRemove(player, args)
    local name = normalizeName(args[2])
    if not name then
        return false
    end

    local _, message = removeSpawner(name)
    player:sendMessage(message)
    return false
end

local function handleReset(player, args)
    local name = normalizeName(args[2])
    if not name then
        return false
    end

    local _, message = resetSpawner(name, args[3])
    player:sendMessage(message)
    return false
end

local function handleCount(player, args)
    local ok, message = setSpawnerCount(args[2], args[3])
    player:sendMessage(message)
    return false
end

local function handleList(player)
    local state = loadState()
    local names = {}
    for name, _ in pairs(state) do
        names[#names + 1] = name
    end
    table.sort(names)
    if #names == 0 then
        player:sendMessage("No destructible spawners.")
    else
        player:sendMessage("Spawners: " .. table.concat(names, ", "))
    end
    return false
end

local function handleInfo(player, args)
    local name = normalizeName(args[2])
    if not name then
        return false
    end

    local spawner = loadState()[name]
    if not spawner then
        player:sendMessage("Spawner not found: " .. name)
        return false
    end

    normalizeSpawnerState(spawner)

    player:sendMessage(string.format(
        "%s: actor=%s record=%s spawn=%s live=%d pending=%d remaining=%d/%d timer=%.1fs cell=%s persistent=%s payloadPersistent=%s reset=%s destroyed=%s",
        name,
        tostring(spawner.actorMpNum or 0),
        tostring(spawner.recordId),
        tostring(spawner.spawnRefId),
        tonumber(spawner.liveCount) or 0,
        tonumber(spawner.pendingCount) or 0,
        tonumber(spawner.remaining) or 0,
        tonumber(spawner.spawnCount) or 0,
        tonumber(spawner.spawnTimer) or 0,
        tostring(spawner.cellId),
        tostring(spawner.persistent ~= false),
        tostring(spawner.spawnedPersistent == true),
        tostring(spawner.respawnOnCellReset == true),
        tostring(spawner.destroyed == true)
    ))
    return false
end

function M.handleChat(player, data, env)
    local prefix = env.commandPrefix or "/"
    local msg = data.message or ""
    local command = prefix .. "spawner"
    local commandPrefix = command .. " "
    if msg ~= command and msg:sub(1, #commandPrefix) ~= commandPrefix then
        return nil
    end

    if not env.requireAdmin(player) then
        return false
    end

    local args, parseError = env.parseCommandArgs(msg == command and "" or msg:sub(#commandPrefix + 1))
    if not args then
        player:sendMessage(parseError or "Invalid arguments.")
        sendUsage(player, prefix)
        return false
    end

    local subcommand = lower(args[1] or "")
    if subcommand == "create" then
        return handleCreate(player, args, env)
    elseif subcommand == "move" then
        return handleMove(player, args, env)
    elseif subcommand == "purge" then
        return handlePurge(player, args)
    elseif subcommand == "remove" or subcommand == "delete" then
        return handleRemove(player, args)
    elseif subcommand == "reset" then
        return handleReset(player, args)
    elseif subcommand == "count" or subcommand == "setcount" then
        return handleCount(player, args)
    elseif subcommand == "list" or subcommand == "" then
        return handleList(player)
    elseif subcommand == "info" then
        return handleInfo(player, args)
    end

    sendUsage(player, prefix)
    return false
end

function M.listForAdminUi()
    local entries = {}
    local seenRecordIds = {}
    for name, spawner in pairs(loadState()) do
        if type(spawner) == "table" then
            normalizeSpawnerState(spawner)
            seenRecordIds[spawner.recordId] = true
            entries[#entries + 1] = {
                name = name,
                recordId = spawner.recordId,
                spawnRefId = spawner.spawnRefId,
                spawnCount = spawner.spawnCount,
                liveCount = spawner.liveCount,
                pendingCount = spawner.pendingCount,
                remaining = spawner.remaining,
                spawnInterval = spawner.spawnInterval,
                spawnTimer = spawner.spawnTimer,
                actorMpNum = spawner.actorMpNum,
                cellId = spawner.cellId,
                persistent = spawner.persistent ~= false,
                spawnedPersistent = spawner.spawnedPersistent == true,
                respawnOnCellReset = spawner.respawnOnCellReset == true,
                destroyed = spawner.destroyed == true,
                active = spawner.active == true,
            }
        end
    end

    for _, record in ipairs(recordStore.list("creature") or {}) do
        local recordId = tostring(record.recordId or "")
        if not seenRecordIds[recordId] then
            local name = nameForRecordId(recordId)
            if name then
                entries[#entries + 1] = {
                    name = name,
                    recordId = recordId,
                    spawnRefId = "",
                    spawnCount = 0,
                    liveCount = 0,
                    pendingCount = 0,
                    remaining = 0,
                    spawnInterval = getSpawnInterval(),
                    spawnTimer = 0,
                    actorMpNum = 0,
                    cellId = "",
                    persistent = record.persistent == true,
                    spawnedPersistent = false,
                    respawnOnCellReset = false,
                    destroyed = true,
                    active = false,
                    orphanedRecord = true,
                }
            end
        end
    end

    table.sort(entries, function(left, right)
        return left.name < right.name
    end)
    return entries
end

function M.defaultsForAdminUi()
    return {
        spawnRefId = "rat",
        spawnCount = tonumber(Config.SPAWNER_DEFAULT_COUNT) or 1,
        spawnInterval = getSpawnInterval(),
        health = tonumber(Config.SPAWNER_HEALTH) or 50,
        model = normalizeMeshPath(Config.SPAWNER_MODEL),
        persistent = true,
        respawnOnCellReset = Config.SPAWNER_DEFAULT_RESPAWN_ON_CELL_RESET == true,
    }
end

function M.createFromAdminUi(player, data, env)
    return createSpawner(player, {
        name = data.name,
        spawnRefId = data.spawnRefId,
        count = data.spawnCount or data.count,
        distance = data.distance,
        direction = data.direction,
        persistent = data.persistent,
        respawnOnCellReset = data.respawnOnCellReset,
    }, env)
end

function M.setCountFromAdminUi(data)
    return setSpawnerCount(data.name, data.spawnCount or data.count)
end

function M.purgeFromAdminUi(data)
    return purgeSpawner(data.name)
end

function M.removeFromAdminUi(data)
    return removeSpawner(data.name)
end

function M.resetFromAdminUi(data)
    return resetSpawner(data.name, data.spawnCount or data.count)
end

function M.onActorSpawned(data)
    local state = loadState()
    local changed = false
    for name, spawner in pairs(state) do
        normalizeSpawnerState(spawner)
        if spawner.recordId == data.refId and spawner.cellId == data.cellId then
            spawner.actorMpNum = tonumber(data.mpNum) or 0
            spawner.active = true
            changed = true
            mp.log(string.format("[spawner] %s actor mpNum=%s", name, tostring(data.mpNum)))
            break
        end

        local pendingIndex = findPendingSpawn(spawner, data)
        if pendingIndex then
            table.remove(spawner.pendingSpawns, pendingIndex)
            spawner.spawnedMpNums[tostring(data.mpNum)] = {
                refId = data.refId,
                cellId = data.cellId,
            }
            refreshCounts(spawner)
            changed = true
            mp.log(string.format("[spawner] %s payload actor mpNum=%s live=%d pending=%d remaining=%d",
                name,
                tostring(data.mpNum),
                spawner.liveCount,
                spawner.pendingCount,
                spawner.remaining
            ))
            break
        end
    end

    if changed then
        saveState(state)
    end
end

function M.onActorDeath(data)
    local state = loadState()
    local matchedName
    local spawner
    local isSpawnerActor = false
    local mpNum = tonumber(data.mpNum) or 0

    for name, candidate in pairs(state) do
        normalizeSpawnerState(candidate)
        -- Primary match: the server-side mpNum we explicitly track.
        -- Fallback (actorMpNum == 0): match by recordId + cellId only when the
        -- spawner actor hasn't been confirmed yet.  Requiring actorMpNum == 0
        -- prevents a ghost actor from a previous server run (same recordId,
        -- same cellId, different mpNum) from falsely destroying an active spawner.
        if (candidate.actorMpNum ~= 0 and candidate.actorMpNum == mpNum)
            or (candidate.actorMpNum == 0 and candidate.recordId == data.refId and candidate.cellId == data.cellId) then
            matchedName = name
            spawner = candidate
            isSpawnerActor = true
            break
        elseif candidate.spawnedMpNums[tostring(mpNum)] then
            matchedName = name
            spawner = candidate
            break
        end
    end

    if not spawner then
        return
    end

    if isSpawnerActor then
        spawner.destroyed = true
        spawner.active = false
        spawner.actorMpNum = 0
        spawner.pendingSpawns = {}
        spawner.spawnTimer = spawner.spawnInterval
        refreshCounts(spawner)
        mp.log(string.format("[spawner] %s destroyed; future timed spawns halted live=%d remaining=%d actorMpNum reset to 0",
            matchedName,
            spawner.liveCount,
            spawner.remaining
        ))

        if mpNum ~= 0 then
            mp.removeGameObject(mpNum, spawner.cellId or data.cellId or "")
        end
    else
        spawner.spawnedMpNums[tostring(mpNum)] = nil
        if not spawner.destroyed then
            spawner.spawnTimer = spawner.spawnInterval
        end
        refreshCounts(spawner)
        if spawner.destroyed then
            mp.log(string.format("[spawner] %s payload died mpNum=%s; no replacement because spawner is destroyed live=%d pending=%d remaining=%d",
                matchedName,
                tostring(data.mpNum),
                spawner.liveCount,
                spawner.pendingCount,
                spawner.remaining
            ))
        else
            mp.log(string.format("[spawner] %s payload died mpNum=%s; replacement in %.1fs live=%d pending=%d remaining=%d",
                matchedName,
                tostring(data.mpNum),
                spawner.spawnInterval,
                spawner.liveCount,
                spawner.pendingCount,
                spawner.remaining
            ))
        end
    end

    saveState(state)
end

function M.onServerInit()
    local state = loadState()
    local changed = false
    for name, spawner in pairs(state) do
        if type(spawner) ~= "table" then
            state[name] = nil
            changed = true
        else
            normalizeSpawnerState(spawner)
            if spawner.persistent == false then
                recordStore.remove("creature", spawner.recordId, { force = true })
                state[name] = nil
                changed = true
            else
                spawner.pendingSpawns = {}
                local staleActorMpNum = tonumber(spawner.actorMpNum) or 0
                if staleActorMpNum ~= 0 then
                    mp.removeGameObject(staleActorMpNum, spawner.cellId or "")
                    mp.log(string.format("[spawner] %s init removing stale spawner actor mpNum=%s",
                        tostring(name),
                        tostring(staleActorMpNum)
                    ))
                    spawner.actorMpNum = 0
                    spawner.active = false
                    changed = true
                end

                if tableCount(spawner.spawnedMpNums) > 0 then
                    local removedPayloads = purgeSpawnedActors(spawner)
                    mp.log(string.format("[spawner] %s init purged %d stale payload actor(s) from previous server run",
                        tostring(name),
                        removedPayloads
                    ))
                    changed = true
                end

                refreshCounts(spawner)

                local stored = ensureSpawnerRecord(name, true)
                if stored and spawner.recordId ~= stored.recordId then
                    spawner.recordId = stored.recordId
                    changed = true
                end

                if spawner.destroyed then
                    if spawner.actorMpNum ~= 0 then
                        mp.log(string.format("[spawner] %s init clearing stale actorMpNum=%s on destroyed spawner",
                            tostring(name),
                            tostring(spawner.actorMpNum)
                        ))
                        spawner.actorMpNum = 0
                        changed = true
                    end
                elseif spawner.actorMpNum == 0 then
                    mp.log(string.format("[spawner] %s init respawning missing spawner actor", tostring(name)))
                    spawnSpawnerActor(spawner)
                    changed = true
                end
            end
        end
    end

    if changed then
        saveState(state)
    end
end

function M.onServerTick(data)
    local dt = tonumber(data and data.dt) or 0
    if dt <= 0 then
        return
    end

    local state = loadState()
    local changed = false

    for name, spawner in pairs(state) do
        if type(spawner) == "table" then
            normalizeSpawnerState(spawner)

            if spawner.destroyed then
                if spawner.actorMpNum ~= 0 then
                    mp.log(string.format("[spawner] %s tick clearing stale actorMpNum=%s on destroyed spawner",
                        tostring(name),
                        tostring(spawner.actorMpNum)
                    ))
                    spawner.actorMpNum = 0
                    changed = true
                end
            else
                if expirePendingSpawns(spawner, dt) then
                    changed = true
                end

                if spawner.pendingCount == 0 and spawner.remaining > 0 then
                    spawner.spawnTimer = math.max(0, spawner.spawnTimer - dt)
                    if spawner.spawnTimer <= 0 and queueSpawnIfNeeded(spawner) then
                        changed = true
                        mp.log(string.format("[spawner] %s queued timed spawn live=%d pending=%d remaining=%d interval=%.2f",
                            name,
                            spawner.liveCount,
                            spawner.pendingCount,
                            spawner.remaining,
                            spawner.spawnInterval
                        ))
                    end
                end

                if spawner.pendingCount > 0 then
                    local spawned = flushPendingSpawns(spawner)
                    if spawned > 0 then
                        changed = true
                        mp.log(string.format("[spawner] %s submitted %d pending spawn request(s) live=%d pending=%d remaining=%d",
                            name,
                            spawned,
                            spawner.liveCount,
                            spawner.pendingCount,
                            spawner.remaining
                        ))
                    end
                end
            end
        end
    end

    if changed then
        saveState(state)
    end
end

return M
