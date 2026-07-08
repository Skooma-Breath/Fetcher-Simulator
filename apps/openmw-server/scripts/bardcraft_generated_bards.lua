local config = require("config")
local mp = require("mp")

local M = {}

local GENERATED_BARD_PREFIX = "r_bc_dyn_bard_"
local STORAGE_SECTION = "BardcraftGeneratedBards"
local STORAGE_KEY = "registry"
local ADMIN_STORAGE_SECTION = "CoreRuntimeAdmins"
local SCHEMA_VERSION = 1
local VALID_MODES = {
    dynamic = true,
    hybrid = true,
    baked = true,
}

local DEFAULTS = {
    class = "r_bc_bard",
    script = "_bcHireableBard",
    sheathedInstrument = "misc_de_lute_01",
    startingLevel = 30,
    defaultSpeed = 75,
    defaultFollowDistance = 192,
}

local registryCache

local function trim(value)
    return tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", "")
end

local function normalizeLookup(value)
    return string.lower(trim(value))
end

local function stripWrappingQuotes(value)
    value = trim(value)
    if #value >= 2 then
        local first = value:sub(1, 1)
        local last = value:sub(-1)
        if (first == '"' and last == '"') or (first == "'" and last == "'") then
            return trim(value:sub(2, -2))
        end
    end
    return value
end

local function runtimeMode()
    local bardcraftConfig = type(config.Bardcraft) == "table" and config.Bardcraft or {}
    local mode = normalizeLookup(bardcraftConfig.dynamicBardRecordsMode)
    if not VALID_MODES[mode] then
        return "dynamic"
    end
    return mode
end

local function storage()
    assert(mp.storage and type(mp.storage.globalSection) == "function",
        "bardcraft_generated_bards.lua requires mp.storage")
    return mp.storage.globalSection(STORAGE_SECTION)
end

local function normalizeRecord(entry)
    if type(entry) ~= "table" then
        return nil
    end

    local recordId = normalizeLookup(entry.recordId)
    -- Older command parsing preserved wrapping quotes from inputs such as
    -- /bc npc makebard "caius cosades". Repair those persisted records before
    -- re-upserting them so a malformed baseId cannot poison client bootstrap.
    local sourceRefId = normalizeLookup(stripWrappingQuotes(entry.sourceRefId))
    if recordId == "" or sourceRefId == "" or recordId:sub(1, #GENERATED_BARD_PREFIX) ~= GENERATED_BARD_PREFIX then
        return nil
    end

    return {
        recordId = recordId,
        sourceRefId = sourceRefId,
        name = trim(entry.name) ~= "" and trim(entry.name) or sourceRefId,
        class = trim(entry.class) ~= "" and trim(entry.class) or DEFAULTS.class,
        script = trim(entry.script) ~= "" and trim(entry.script) or DEFAULTS.script,
        race = trim(entry.race) ~= "" and trim(entry.race) or nil,
        head = trim(entry.head) ~= "" and trim(entry.head) or nil,
        hair = trim(entry.hair) ~= "" and trim(entry.hair) or nil,
        sheathedInstrument = trim(entry.sheathedInstrument) ~= ""
            and trim(entry.sheathedInstrument) or DEFAULTS.sheathedInstrument,
        startingLevel = tonumber(entry.startingLevel) or DEFAULTS.startingLevel,
        defaultSpeed = tonumber(entry.defaultSpeed) or DEFAULTS.defaultSpeed,
        defaultFollowDistance = tonumber(entry.defaultFollowDistance) or DEFAULTS.defaultFollowDistance,
        baked = entry.baked == true,
        enabled = entry.enabled ~= false,
    }
end

local function loadRegistry()
    if registryCache ~= nil then
        return registryCache
    end

    local stored = storage():getCopy(STORAGE_KEY)
    local registry = {
        schemaVersion = SCHEMA_VERSION,
        records = {},
    }
    if type(stored) == "table" and type(stored.records) == "table" then
        for _, rawEntry in ipairs(stored.records) do
            local entry = normalizeRecord(rawEntry)
            if entry then
                table.insert(registry.records, entry)
            end
        end
    end
    table.sort(registry.records, function(left, right)
        return left.recordId < right.recordId
    end)
    registryCache = registry
    return registryCache
end

local function saveRegistry()
    local registry = loadRegistry()
    registry.schemaVersion = SCHEMA_VERSION
    table.sort(registry.records, function(left, right)
        return left.recordId < right.recordId
    end)
    storage():set(STORAGE_KEY, registry)
end

local function findRecord(recordId)
    local wanted = normalizeLookup(recordId)
    for _, entry in ipairs(loadRegistry().records) do
        if entry.recordId == wanted then
            return entry
        end
    end
    return nil
end

local function findRecordBySource(sourceRefId)
    local wanted = normalizeLookup(sourceRefId)
    for _, entry in ipairs(loadRegistry().records) do
        if entry.sourceRefId == wanted then
            return entry
        end
    end
    return nil
end

local function stableRecordId(sourceRefId)
    local slug = normalizeLookup(sourceRefId)
        :gsub("[^%w]+", "_")
        :gsub("^_+", "")
        :gsub("_+$", "")
        :gsub("_+", "_")
    if slug == "" then
        return nil
    end
    return GENERATED_BARD_PREFIX .. slug
end

local function shouldSendDynamic(entry)
    if not entry or entry.enabled == false then
        return false
    end
    local mode = runtimeMode()
    return mode == "dynamic" or (mode == "hybrid" and entry.baked ~= true)
end

local function dynamicRecordData(entry)
    local data = {
        baseId = entry.sourceRefId,
        name = entry.name,
        class = DEFAULTS.class,
        mwscript = DEFAULTS.script,
    }
    if entry.race then data.race = entry.race end
    if entry.head then data.head = entry.head end
    if entry.hair then data.hair = entry.hair end
    return data
end

local function upsertDynamic(entry)
    if not shouldSendDynamic(entry) then
        return true, "not-required"
    end
    local ok = mp.upsertDynamicRecord("npc", entry.recordId, dynamicRecordData(entry), {
        -- Stable registry IDs must survive while no actor instance references
        -- them. The server auto-GCs unlinked scope="generated" records.
        scope = "permanent",
        persistent = true,
    })
    if not ok then
        return false, "mp.upsertDynamicRecord returned false"
    end
    return true, "queued"
end

local function syncDynamic(entry)
    if shouldSendDynamic(entry) then
        return upsertDynamic(entry)
    end
    if type(mp.removeDynamicRecord) == "function" then
        -- Removing an absent override is harmless. This also deletes a
        -- previously persisted dynamic override when switching to baked mode.
        mp.removeDynamicRecord("npc", entry.recordId)
    end
    return true, "suppressed"
end

local function jsonEscape(value)
    return '"' .. tostring(value)
        :gsub("\\", "\\\\")
        :gsub('"', '\\"')
        :gsub("\b", "\\b")
        :gsub("\f", "\\f")
        :gsub("\n", "\\n")
        :gsub("\r", "\\r")
        :gsub("\t", "\\t") .. '"'
end

local function jsonValue(value)
    if value == nil then return "null" end
    if type(value) == "boolean" or type(value) == "number" then return tostring(value) end
    return jsonEscape(value)
end

local EXPORT_FIELDS = {
    "recordId",
    "sourceRefId",
    "name",
    "class",
    "script",
    "race",
    "head",
    "hair",
    "sheathedInstrument",
    "startingLevel",
    "defaultSpeed",
    "defaultFollowDistance",
    "baked",
    "enabled",
}

local function encodeRegistryJson()
    local lines = {
        "{",
        "  \"schemaVersion\": " .. SCHEMA_VERSION .. ",",
        "  \"records\": [",
    }
    local records = loadRegistry().records
    for index, entry in ipairs(records) do
        lines[#lines + 1] = "    {"
        for fieldIndex, field in ipairs(EXPORT_FIELDS) do
            local suffix = fieldIndex < #EXPORT_FIELDS and "," or ""
            lines[#lines + 1] = "      " .. jsonEscape(field) .. ": " .. jsonValue(entry[field]) .. suffix
        end
        lines[#lines + 1] = "    }" .. (index < #records and "," or "")
    end
    lines[#lines + 1] = "  ]"
    lines[#lines + 1] = "}"
    return table.concat(lines, "\n") .. "\n"
end

function M.initialize()
    local queued = 0
    local skipped = 0
    local failed = 0
    for _, entry in ipairs(loadRegistry().records) do
        if shouldSendDynamic(entry) then
            local ok = syncDynamic(entry)
            if ok then queued = queued + 1 else failed = failed + 1 end
        else
            syncDynamic(entry)
            skipped = skipped + 1
        end
    end
    mp.log(string.format(
        "[bardcraft] generated bard registry mode=%s records=%d queued=%d skipped=%d failed=%d",
        runtimeMode(), #loadRegistry().records, queued, skipped, failed))
    return queued, skipped, failed
end

function M.make(sourceRefId, displayName)
    sourceRefId = normalizeLookup(stripWrappingQuotes(sourceRefId))
    if sourceRefId == "" then
        return nil, "Source NPC record ID is required."
    end

    local existing = findRecordBySource(sourceRefId)
    if existing then
        local ok, reason = syncDynamic(existing)
        if not ok then
            return nil, "Generated bard exists, but dynamic NPC synchronization failed: " .. tostring(reason)
        end
        return existing, nil, true
    end

    local recordId = stableRecordId(sourceRefId)
    if not recordId then
        return nil, "Source NPC record ID does not contain usable ID characters."
    end
    local collision = findRecord(recordId)
    if collision and collision.sourceRefId ~= sourceRefId then
        return nil, string.format("Generated ID collision: %s already belongs to %s.", recordId, collision.sourceRefId)
    end

    local entry = normalizeRecord({
        recordId = recordId,
        sourceRefId = sourceRefId,
        name = trim(displayName) ~= "" and trim(displayName) or sourceRefId,
        class = DEFAULTS.class,
        script = DEFAULTS.script,
        sheathedInstrument = DEFAULTS.sheathedInstrument,
        startingLevel = DEFAULTS.startingLevel,
        defaultSpeed = DEFAULTS.defaultSpeed,
        defaultFollowDistance = DEFAULTS.defaultFollowDistance,
        baked = false,
        enabled = true,
    })
    table.insert(loadRegistry().records, entry)
    saveRegistry()

    local ok, reason = syncDynamic(entry)
    if not ok then
        return nil, "Registry saved, but dynamic NPC upsert failed: " .. tostring(reason)
    end
    return entry, nil, false
end

function M.setBaked(recordId, baked)
    local entry = findRecord(recordId)
    if not entry then
        return nil, "Generated bard not found: " .. tostring(recordId)
    end
    entry.baked = baked == true
    saveRegistry()
    local ok, reason = syncDynamic(entry)
    if not ok then
        return nil, "Baked flag updated, but dynamic NPC synchronization failed: " .. tostring(reason)
    end
    return entry
end

function M.export()
    if type(mp.writeGeneratedBardsExport) ~= "function" then
        return false, "Server binary does not provide mp.writeGeneratedBardsExport."
    end
    local ok = mp.writeGeneratedBardsExport(encodeRegistryJson())
    if not ok then
        return false, "Failed to write generated_bards.json."
    end
    return true, "generated_bards.json"
end

function M.list()
    local result = {}
    for _, entry in ipairs(loadRegistry().records) do
        result[#result + 1] = entry
    end
    return result
end

function M.get(recordId)
    return findRecord(recordId)
end

function M.forget(recordId)
    local wanted = normalizeLookup(recordId)
    local records = loadRegistry().records
    for index, entry in ipairs(records) do
        if entry.recordId == wanted then
            table.remove(records, index)
            saveRegistry()
            return entry
        end
    end
    return nil
end

function M.isEnabledRecord(recordId)
    local entry = findRecord(recordId)
    return entry ~= nil and entry.enabled ~= false
end

function M.isRuntimeAdmin(guid)
    guid = tonumber(guid)
    return guid ~= nil
        and mp.storage.globalSection(ADMIN_STORAGE_SECTION):getCopy(tostring(guid)) == true
end

function M.mode()
    return runtimeMode()
end

function M.prefix()
    return GENERATED_BARD_PREFIX
end

return M
