local mp = require("mp")
local commandRegistry = require("command_registry")
local recordStore = require("recordstore")

local M = {}

local TYPE_ALIASES = {
    misc = "miscellaneous",
}

local RECORD_TYPE_DEFS = {
    { id = "activator", defaultBaseId = "active_de_p_bed_28", defaultName = "RecordDynamic Test Activator", canPlace = true },
    { id = "armor", defaultBaseId = "orcish_cuirass", defaultName = "RecordDynamic Test Armor", canPlace = true },
    { id = "book", defaultBaseId = "bk_guide_to_vvardenfell", defaultName = "RecordDynamic Test Book", canPlace = true },
    { id = "clothing", defaultBaseId = "exquisite_robe_01", defaultName = "RecordDynamic Test Clothing", canPlace = true },
    { id = "container", defaultBaseId = "chest_small_01", defaultName = "RecordDynamic Test Container", canPlace = true },
    { id = "creature", defaultBaseId = "mudcrab", defaultName = "RecordDynamic Test Creature", canPlace = false, hint = "Use /spawnat <recordId> to spawn actor test refs." },
    { id = "door", defaultBaseId = "ex_common_door_01", defaultName = "RecordDynamic Test Door", canPlace = true },
    { id = "enchantment", defaultName = "RecordDynamic Test Enchantment", canPlace = false, hint = "Uses blank defaults unless you provide a baseId." },
    { id = "light", defaultBaseId = "light_com_torch_01", defaultName = "RecordDynamic Test Light", canPlace = true },
    { id = "miscellaneous", defaultBaseId = "misc_de_pot_blue_01", defaultName = "RecordDynamic Test Misc", canPlace = true },
    { id = "npc", defaultBaseId = "fargoth", defaultName = "RecordDynamic Test NPC", canPlace = false, hint = "Use /spawnat <recordId> to spawn actor test refs." },
    { id = "potion", defaultBaseId = "p_restore_health_s", defaultName = "RecordDynamic Test Potion", canPlace = true },
    { id = "spell", defaultName = "RecordDynamic Test Spell", canPlace = false, hint = "Use through spellbook, traps, or future spell UI." },
    { id = "static", defaultBaseId = "terrain_rock_wg_01", canPlace = true },
    { id = "weapon", defaultBaseId = "iron shortsword", defaultName = "RecordDynamic Test Weapon", canPlace = true },
}

local function prependKeepOption(options)
    local result = {
        { label = "keep", value = "" },
    }
    for _, entry in ipairs(options or {}) do
        result[#result + 1] = entry
    end
    return result
end

local function triStateOptions()
    return {
        { label = "keep", value = "" },
        { label = "yes", value = true },
        { label = "no", value = false },
    }
end

local RECORD_FIELD_SCHEMAS = {
    weapon = {
        { id = "model", label = "Model", kind = "text" },
        { id = "icon", label = "Icon", kind = "text" },
        { id = "mwscript", label = "Script", kind = "text" },
        { id = "enchant", label = "Enchant ID", kind = "text" },
        { id = "weight", label = "Weight", kind = "number" },
        { id = "value", label = "Value", kind = "number", integer = true },
        { id = "type", label = "Weapon Type", kind = "choice", options = prependKeepOption({
            { label = "Short Blade 1H", value = 0 },
            { label = "Long Blade 1H", value = 1 },
            { label = "Long Blade 2H", value = 2 },
            { label = "Blunt 1H", value = 3 },
            { label = "Blunt 2C", value = 4 },
            { label = "Blunt 2W", value = 5 },
            { label = "Spear 2W", value = 6 },
            { label = "Axe 1H", value = 7 },
            { label = "Axe 2H", value = 8 },
            { label = "Bow", value = 9 },
            { label = "Crossbow", value = 10 },
            { label = "Thrown", value = 11 },
            { label = "Arrow", value = 12 },
            { label = "Bolt", value = 13 },
        }) },
        { id = "health", label = "Health", kind = "number", integer = true },
        { id = "speed", label = "Speed", kind = "number" },
        { id = "reach", label = "Reach", kind = "number" },
        { id = "enchantCapacity", label = "Enchant Capacity", kind = "number" },
        { id = "chopMinDamage", label = "Chop Min", kind = "number", integer = true },
        { id = "chopMaxDamage", label = "Chop Max", kind = "number", integer = true },
        { id = "slashMinDamage", label = "Slash Min", kind = "number", integer = true },
        { id = "slashMaxDamage", label = "Slash Max", kind = "number", integer = true },
        { id = "thrustMinDamage", label = "Thrust Min", kind = "number", integer = true },
        { id = "thrustMaxDamage", label = "Thrust Max", kind = "number", integer = true },
        { id = "isMagical", label = "Magical", kind = "choice", options = triStateOptions() },
        { id = "isSilver", label = "Silver", kind = "choice", options = triStateOptions() },
    },
    armor = {
        { id = "model", label = "Model", kind = "text" },
        { id = "icon", label = "Icon", kind = "text" },
        { id = "mwscript", label = "Script", kind = "text" },
        { id = "enchant", label = "Enchant ID", kind = "text" },
        { id = "weight", label = "Weight", kind = "number" },
        { id = "value", label = "Value", kind = "number", integer = true },
        { id = "type", label = "Armor Type", kind = "choice", options = prependKeepOption({
            { label = "Helmet", value = 0 },
            { label = "Cuirass", value = 1 },
            { label = "LPauldron", value = 2 },
            { label = "RPauldron", value = 3 },
            { label = "Greaves", value = 4 },
            { label = "Boots", value = 5 },
            { label = "LGauntlet", value = 6 },
            { label = "RGauntlet", value = 7 },
            { label = "Shield", value = 8 },
            { label = "LBracer", value = 9 },
            { label = "RBracer", value = 10 },
        }) },
        { id = "health", label = "Health", kind = "number", integer = true },
        { id = "baseArmor", label = "Base Armor", kind = "number", integer = true },
        { id = "enchantCapacity", label = "Enchant Capacity", kind = "number" },
    },
    clothing = {
        { id = "model", label = "Model", kind = "text" },
        { id = "icon", label = "Icon", kind = "text" },
        { id = "mwscript", label = "Script", kind = "text" },
        { id = "enchant", label = "Enchant ID", kind = "text" },
        { id = "weight", label = "Weight", kind = "number" },
        { id = "value", label = "Value", kind = "number", integer = true },
        { id = "type", label = "Clothing Type", kind = "choice", options = prependKeepOption({
            { label = "Amulet", value = 0 },
            { label = "Belt", value = 1 },
            { label = "LGlove", value = 2 },
            { label = "Pants", value = 3 },
            { label = "RGlove", value = 4 },
            { label = "Ring", value = 5 },
            { label = "Robe", value = 6 },
            { label = "Shirt", value = 7 },
            { label = "Shoes", value = 8 },
            { label = "Skirt", value = 9 },
        }) },
        { id = "enchantCapacity", label = "Enchant Capacity", kind = "number" },
    },
    light = {
        { id = "model", label = "Model", kind = "text" },
        { id = "icon", label = "Icon", kind = "text" },
        { id = "mwscript", label = "Script", kind = "text" },
        { id = "weight", label = "Weight", kind = "number" },
        { id = "value", label = "Value", kind = "number", integer = true },
        { id = "duration", label = "Duration", kind = "number", integer = true },
        { id = "radius", label = "Radius", kind = "number", integer = true },
        { id = "color", label = "Color", kind = "color", hint = "#RRGGBB or R,G,B" },
        { id = "isCarriable", label = "Carriable", kind = "choice", options = triStateOptions() },
        { id = "isDynamic", label = "Dynamic", kind = "choice", options = triStateOptions() },
        { id = "isFire", label = "Fire", kind = "choice", options = triStateOptions() },
        { id = "isFlicker", label = "Flicker", kind = "choice", options = triStateOptions() },
        { id = "isFlickerSlow", label = "Flicker Slow", kind = "choice", options = triStateOptions() },
        { id = "isNegative", label = "Negative", kind = "choice", options = triStateOptions() },
        { id = "isOffByDefault", label = "Off By Default", kind = "choice", options = triStateOptions() },
        { id = "isPulse", label = "Pulse", kind = "choice", options = triStateOptions() },
        { id = "isPulseSlow", label = "Pulse Slow", kind = "choice", options = triStateOptions() },
    },
    potion = {
        { id = "model", label = "Model", kind = "text" },
        { id = "icon", label = "Icon", kind = "text" },
        { id = "mwscript", label = "Script", kind = "text" },
        { id = "weight", label = "Weight", kind = "number" },
        { id = "value", label = "Value", kind = "number", integer = true },
        { id = "isAutocalc", label = "Autocalc", kind = "choice", options = triStateOptions() },
        { id = "effectsText", label = "Effects", kind = "effects", hint = "effectId|range|duration|min|max|area|skill|attribute ; ..." },
    },
    spell = {
        { id = "type", label = "Spell Type", kind = "choice", options = prependKeepOption({
            { label = "Spell", value = 0 },
            { label = "Ability", value = 1 },
            { label = "Blight", value = 2 },
            { label = "Disease", value = 3 },
            { label = "Curse", value = 4 },
            { label = "Power", value = 5 },
        }) },
        { id = "cost", label = "Cost", kind = "number", integer = true },
        { id = "isAutocalc", label = "Autocalc", kind = "choice", options = triStateOptions() },
        { id = "alwaysSucceedFlag", label = "Always Succeed", kind = "choice", options = triStateOptions() },
        { id = "starterSpellFlag", label = "Starter Spell", kind = "choice", options = triStateOptions() },
        { id = "effectsText", label = "Effects", kind = "effects", hint = "effectId|range|duration|min|max|area|skill|attribute ; ..." },
    },
    enchantment = {
        { id = "type", label = "Enchant Type", kind = "choice", options = prependKeepOption({
            { label = "Cast Once", value = 0 },
            { label = "Cast On Strike", value = 1 },
            { label = "Cast On Use", value = 2 },
            { label = "Constant Effect", value = 3 },
        }) },
        { id = "cost", label = "Cost", kind = "number", integer = true },
        { id = "charge", label = "Charge", kind = "number", integer = true },
        { id = "isAutocalc", label = "Autocalc", kind = "choice", options = triStateOptions() },
        { id = "effectsText", label = "Effects", kind = "effects", hint = "effectId|range|duration|min|max|area|skill|attribute ; ..." },
    },
}

local function trim(text)
    if not text then
        return nil
    end
    return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function lower(text)
    return string.lower(trim(text) or "")
end

local function normalizeRecordType(recordType)
    local normalized = TYPE_ALIASES[lower(recordType)] or lower(recordType)
    if normalized == "" then
        return nil
    end
    return normalized
end

local function getRecordTypeDef(recordType)
    local normalized = normalizeRecordType(recordType)
    if not normalized then
        return nil
    end

    for _, entry in ipairs(RECORD_TYPE_DEFS) do
        if entry.id == normalized then
            return entry
        end
    end

    return nil
end

local function sortRecords(records)
    table.sort(records, function(left, right)
        if left.recordType ~= right.recordType then
            return left.recordType < right.recordType
        end

        local leftPersistent = left.persistent and 1 or 0
        local rightPersistent = right.persistent and 1 or 0
        if leftPersistent ~= rightPersistent then
            return leftPersistent > rightPersistent
        end

        return left.recordId < right.recordId
    end)
end

local function copyRecordEntry(entry)
    return {
        recordType = tostring(entry.recordType or ""),
        recordId = tostring(entry.recordId or ""),
        scope = tostring(entry.scope or ""),
        persistent = entry.persistent == true,
        createdAt = tonumber(entry.createdAt) or 0,
        updatedAt = tonumber(entry.updatedAt) or 0,
        linkCount = tonumber(entry.linkCount) or 0,
        loaded = entry.loaded == true,
    }
end

local function copyRecordEntries(entries)
    local records = {}
    for _, entry in ipairs(entries or {}) do
        records[#records + 1] = copyRecordEntry(entry)
    end
    return records
end

local function buildPlayers(env)
    local players = {}
    for _, entry in ipairs(mp.getPlayers()) do
        players[#players + 1] = {
            guid = entry.guid,
            name = entry.name,
            cell = entry.cell,
            isAdmin = env.isAdmin(entry),
        }
    end

    table.sort(players, function(left, right)
        return string.lower(left.name) < string.lower(right.name)
    end)

    return players
end

local function buildSummary(records)
    local byType = {}
    local generated = 0
    local persistent = 0

    for _, entry in ipairs(records) do
        byType[entry.recordType] = (byType[entry.recordType] or 0) + 1
        if entry.scope == "generated" then
            generated = generated + 1
        end
        if entry.persistent then
            persistent = persistent + 1
        end
    end

    local counts = {}
    for recordType, count in pairs(byType) do
        counts[#counts + 1] = {
            recordType = recordType,
            count = count,
        }
    end
    table.sort(counts, function(left, right)
        return left.recordType < right.recordType
    end)

    return {
        total = #records,
        generated = generated,
        persistent = persistent,
        countsByType = counts,
    }
end

local function copyDatabaseTableEntries(entries)
    local tables = {}
    for _, entry in ipairs(entries or {}) do
        tables[#tables + 1] = {
            name = tostring(entry.name or ""),
            rowCount = tonumber(entry.rowCount) or 0,
        }
    end
    return tables
end

local function copyDatabasePage(page)
    if not page then
        return nil
    end

    local columns = {}
    for _, column in ipairs(page.columns or {}) do
        columns[#columns + 1] = tostring(column or "")
    end

    local rows = {}
    for _, row in ipairs(page.rows or {}) do
        local copy = {}
        for _, column in ipairs(columns) do
            local value = row[column]
            if value ~= nil then
                copy[column] = tostring(value)
            end
        end
        rows[#rows + 1] = copy
    end

    return {
        tableName = tostring(page.tableName or ""),
        totalRows = tonumber(page.totalRows) or 0,
        offset = tonumber(page.offset) or 0,
        limit = tonumber(page.limit) or 0,
        columns = columns,
        rows = rows,
    }
end

local function jsonEscape(text)
    local value = tostring(text or "")
    value = value:gsub("\\", "\\\\")
    value = value:gsub("\"", "\\\"")
    value = value:gsub("\b", "\\b")
    value = value:gsub("\f", "\\f")
    value = value:gsub("\n", "\\n")
    value = value:gsub("\r", "\\r")
    value = value:gsub("\t", "\\t")
    return value:gsub("[%z\1-\31]", function(char)
        return string.format("\\u%04x", string.byte(char))
    end)
end

local function isArray(value)
    if type(value) ~= "table" then
        return false, 0
    end

    local maxIndex = 0
    local count = 0
    for key, _ in pairs(value) do
        if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then
            return false, 0
        end
        if key > maxIndex then
            maxIndex = key
        end
        count = count + 1
    end

    return maxIndex == count, maxIndex
end

local function encodeJson(value)
    local valueType = type(value)
    if value == nil then
        return "null"
    end
    if valueType == "boolean" then
        return value and "true" or "false"
    end
    if valueType == "number" then
        return tostring(value)
    end
    if valueType == "string" then
        return "\"" .. jsonEscape(value) .. "\""
    end
    if valueType ~= "table" then
        return "\"" .. jsonEscape(tostring(value)) .. "\""
    end

    local arrayLike, length = isArray(value)
    if arrayLike then
        local parts = {}
        for index = 1, length do
            parts[#parts + 1] = encodeJson(value[index])
        end
        return "[" .. table.concat(parts, ",") .. "]"
    end

    local keys = {}
    for key, _ in pairs(value) do
        keys[#keys + 1] = key
    end
    table.sort(keys, function(left, right)
        return tostring(left) < tostring(right)
    end)

    local parts = {}
    for _, key in ipairs(keys) do
        parts[#parts + 1] = "\"" .. jsonEscape(tostring(key)) .. "\":" .. encodeJson(value[key])
    end
    return "{" .. table.concat(parts, ",") .. "}"
end

local function jsonResponse(status, payload)
    return {
        status = tonumber(status) or 200,
        contentType = "application/json; charset=utf-8",
        body = encodeJson(payload or {}),
    }
end

local function parseColorValue(text)
    local value = trim(text)
    if not value or value == "" then
        return nil
    end

    local r, g, b = value:match("^(%d+)%s*,%s*(%d+)%s*,%s*(%d+)$")
    if r and g and b then
        r = tonumber(r)
        g = tonumber(g)
        b = tonumber(b)
        if r <= 255 and g <= 255 and b <= 255 then
            return (r * 65536) + (g * 256) + b
        end
    end

    local hex = value:match("^#?(%x%x%x%x%x%x)$") or value:match("^0[xX](%x%x%x%x%x%x)$")
    if hex then
        return tonumber(hex, 16)
    end

    return nil, "Color must be #RRGGBB, 0xRRGGBB, or R,G,B."
end

local function normalizeModelResourcePath(text)
    local value = trim(text)
    if not value or value == "" then
        return nil
    end

    local normalized = tostring(value):gsub("/", "\\"):gsub("^\\+", "")
    if not string.lower(normalized):match("^meshes\\") then
        normalized = "meshes\\" .. normalized
    end

    return normalized
end

local function parseRangeValue(text)
    local normalized = lower(text)
    if normalized == "" or normalized == "self" then
        return 0
    end
    if normalized == "touch" then
        return 1
    end
    if normalized == "target" then
        return 2
    end

    local numeric = tonumber(text)
    if numeric and numeric >= 0 and numeric <= 2 then
        return math.floor(numeric)
    end

    return nil, "Effect range must be Self, Touch, Target, 0, 1, or 2."
end

local function splitPipe(text)
    local parts = {}
    for part in ((text or "") .. "|"):gmatch("(.-)|") do
        parts[#parts + 1] = trim(part) or ""
    end
    return parts
end

local function parseEffectsValue(text)
    local value = trim(text)
    if not value or value == "" then
        return nil
    end

    local effects = {}
    for rawEntry in value:gmatch("([^;\r\n]+)") do
        local entry = trim(rawEntry)
        if entry and entry ~= "" then
            local parts = splitPipe(entry)
            local effectId = trim(parts[1])
            if not effectId or effectId == "" then
                return nil, "Each effect entry needs an effect id."
            end

            local range, rangeErr = parseRangeValue(parts[2])
            if not range then
                return nil, rangeErr
            end

            local duration = tonumber(parts[3] ~= "" and parts[3] or "1")
            local magnitudeMin = tonumber(parts[4] ~= "" and parts[4] or "1")
            local magnitudeMax = tonumber(parts[5] ~= "" and parts[5] or tostring(magnitudeMin))
            local area = tonumber(parts[6] ~= "" and parts[6] or "0")
            if not duration or not magnitudeMin or not magnitudeMax or not area then
                return nil, "Effect numbers must use duration|min|max|area."
            end

            local effect = {
                id = effectId,
                range = math.floor(range),
                duration = math.floor(duration),
                magnitudeMin = math.floor(magnitudeMin),
                magnitudeMax = math.floor(magnitudeMax),
                area = math.floor(area),
            }

            local skill = trim(parts[7])
            if skill and skill ~= "" then
                effect.affectedSkill = skill
            end

            local attribute = trim(parts[8])
            if attribute and attribute ~= "" then
                effect.affectedAttribute = attribute
            end

            effects[#effects + 1] = effect
        end
    end

    return effects
end

local function parseChoiceValue(field, raw)
    if raw == nil or raw == "" then
        return nil
    end

    for _, option in ipairs(field.options or {}) do
        if option.value == raw then
            return option.value
        end
        if type(option.value) == "number" and tonumber(raw) == option.value then
            return option.value
        end
        if type(option.value) == "boolean" and tostring(raw) == tostring(option.value) then
            return option.value
        end
        if type(option.value) == "string" and tostring(raw) == option.value then
            return option.value
        end
    end

    return nil, string.format("%s has an invalid value.", field.label or field.id)
end

local function parseSchemaFieldValue(field, raw)
    if field.kind == "text" then
        if field.id == "model" then
            return normalizeModelResourcePath(raw)
        end
        return trim(raw)
    end

    if field.kind == "number" then
        local text = trim(raw)
        if not text or text == "" then
            return nil
        end
        local value = tonumber(text)
        if not value then
            return nil, string.format("%s must be numeric.", field.label or field.id)
        end
        if field.integer then
            value = math.floor(value)
        end
        return value
    end

    if field.kind == "choice" then
        return parseChoiceValue(field, raw)
    end

    if field.kind == "color" then
        return parseColorValue(raw)
    end

    if field.kind == "effects" then
        return parseEffectsValue(raw)
    end

    return nil
end

local function applySchemaFields(payload, recordType, rawFields)
    local schema = RECORD_FIELD_SCHEMAS[recordType]
    if not schema or type(rawFields) ~= "table" then
        return payload
    end

    for _, field in ipairs(schema) do
        local value, err = parseSchemaFieldValue(field, rawFields[field.id])
        if err then
            return nil, err
        end
        if value ~= nil and value ~= "" then
            local targetKey = field.payloadKey or field.id
            payload[targetKey] = value
        end
    end

    return payload
end

local function buildDatabaseInfo()
    local tables = {}
    if mp.listDatabaseTables then
        tables = copyDatabaseTableEntries(mp.listDatabaseTables() or {})
    end

    return {
        defaultLimit = 100,
        tables = tables,
    }
end

local function buildHealthPayload()
    local database = buildDatabaseInfo()
    return {
        ok = true,
        playerCount = mp.getPlayerCount(),
        uptime = mp.getUptime(),
        worldHour = mp.getWorldTime(),
        tableCount = #(database.tables or {}),
    }
end

local function buildSnapshot(player, env)
    local records = copyRecordEntries(recordStore.list())
    sortRecords(records)
    local canManageRecords = true

    return {
        commandPrefix = env.commandPrefix or "/",
        generatedRecordIdPrefix = mp.getGeneratedRecordIdPrefix and mp.getGeneratedRecordIdPrefix() or "$custom",
        commandSections = commandRegistry.getSections(env.commandPrefix or "/", true),
        canManageRecords = canManageRecords,
        recordTypes = RECORD_TYPE_DEFS,
        recordFieldSchemas = RECORD_FIELD_SCHEMAS,
        records = records,
        database = buildDatabaseInfo(),
        players = buildPlayers(env),
        summary = buildSummary(records),
        server = {
            playerCount = mp.getPlayerCount(),
            worldTime = mp.getWorldTime(),
            uptime = mp.getUptime(),
            requestingPlayer = player.name,
        },
    }
end

local function sendSnapshot(player, eventName, env)
    mp.send(player.guid, eventName, buildSnapshot(player, env))
end

local function sendToast(player, message, level)
    mp.send(player.guid, "AdminUi_Toast", {
        message = message,
        level = level or "info",
    })
end

local function buildRecordPayload(definition, recordType, data)
    data = data or {}
    local baseId = trim(data.baseId) or definition.defaultBaseId
    local name = trim(data.name)

    if recordType == "enchantment" then
        local payload
        if baseId and baseId ~= "" then
            payload = {
                baseId = baseId,
            }
        else
            payload = {
                type = 0,
                cost = 1,
                charge = 1,
                isAutocalc = false,
                effects = {},
            }
        end

        return applySchemaFields(payload, recordType, data.fields)
    end

    if recordType == "spell" then
        local payload
        if baseId and baseId ~= "" then
            payload = {
                baseId = baseId,
                name = name ~= "" and name or definition.defaultName,
            }
        else
            payload = {
                name = name ~= "" and name or definition.defaultName,
                type = 0,
                cost = 1,
                alwaysSucceedFlag = true,
                isAutocalc = false,
                effects = {},
            }
        end

        return applySchemaFields(payload, recordType, data.fields)
    end

    if not baseId or baseId == "" then
        return nil, string.format("%s requires a baseId.", recordType)
    end

    local payload = {
        baseId = baseId,
    }
    if name and name ~= "" then
        payload.name = name
    elseif definition.defaultName then
        payload.name = definition.defaultName
    end
    return applySchemaFields(payload, recordType, data.fields)
end

local function resolvePersistent(value)
    if value == true or value == false then
        return value
    end

    local normalized = lower(value)
    if normalized == "persistent" or normalized == "persist" or normalized == "true" then
        return true
    end
    if normalized == "session" or normalized == "temporary" or normalized == "temp" or normalized == "false" then
        return false
    end

    return false
end

local function resolveScope(value)
    local normalized = lower(value)
    if normalized == "permanent" or normalized == "perm" then
        return "permanent"
    end
    return "generated"
end

local function handleCreate(player, data, env)
    local recordType = normalizeRecordType(data.recordType)
    local definition = getRecordTypeDef(recordType)
    if not definition then
        sendToast(player, "Unknown record type.", "error")
        sendSnapshot(player, "AdminUi_Snapshot", env)
        return
    end

    local payload, payloadErr = buildRecordPayload(definition, recordType, data)
    if not payload then
        sendToast(player, payloadErr or "Failed to build record payload.", "error")
        sendSnapshot(player, "AdminUi_Snapshot", env)
        return
    end

    local result, err = recordStore.ensure(recordType, payload, {
        scope = resolveScope(data.scope),
        persistent = resolvePersistent(data.persistent),
    })
    if not result then
        sendToast(player, err or "Failed to create dynamic record.", "error")
        sendSnapshot(player, "AdminUi_Snapshot", env)
        return
    end

    local verb = result.reused and "Reused" or "Created"
    local detail = definition.canPlace and (" Use " .. (env.commandPrefix or "/") .. "placeat " .. result.recordId .. ".") or ""
    sendToast(player, string.format(
        "%s %s %s (%s/%s).%s",
        verb,
        result.recordType,
        result.recordId,
        result.scope,
        result.persistent and "persistent" or "session",
        detail
    ), "success")
    sendSnapshot(player, "AdminUi_Snapshot", env)
end

local function handleDelete(player, data, env)
    local recordType = normalizeRecordType(data.recordType)
    local recordId = trim(data.recordId)
    if not recordType or not recordId or recordId == "" then
        sendToast(player, "Delete requires recordType and recordId.", "error")
        sendSnapshot(player, "AdminUi_Snapshot", env)
        return
    end

    local ok, result = recordStore.remove(recordType, recordId, {
        force = data.force == true,
    })
    if not ok then
        sendToast(player, result or "Failed to remove dynamic record.", "error")
        sendSnapshot(player, "AdminUi_Snapshot", env)
        return
    end

    sendToast(player, string.format("Removed %s %s.", recordType, recordId), "success")
    sendSnapshot(player, "AdminUi_Snapshot", env)
end

local function handleGc(player, data, env)
    local removed = recordStore.gcGeneratedUnlinked({
        persistent = data.persistence,
        recordType = data.recordType,
    })

    if #removed == 0 then
        sendToast(player, "No unlinked generated records were removed.", "info")
        sendSnapshot(player, "AdminUi_Snapshot", env)
        return
    end

    sendToast(player, string.format("Removed %d unlinked generated record(s).", #removed), "success")
    sendSnapshot(player, "AdminUi_Snapshot", env)
end

local function handleSync(player, env)
    local removed = recordStore.syncState()
    sendToast(player, string.format("Recordstore metadata sync removed %d stale entr%s.",
        removed, removed == 1 and "y" or "ies"), "info")
    sendSnapshot(player, "AdminUi_Snapshot", env)
end

local function handleDatabaseBrowse(player, data)
    if not mp.browseDatabaseTable then
        sendToast(player, "Database browsing is unavailable on this server build.", "error")
        return
    end

    local tableName = trim(data.tableName)
    if not tableName or tableName == "" then
        sendToast(player, "Database browse requires a tableName.", "error")
        return
    end

    local offset = math.max(0, math.floor(tonumber(data.offset) or 0))
    local limit = math.max(1, math.floor(tonumber(data.limit) or 100))
    local page = mp.browseDatabaseTable(tableName, offset, limit)
    if not page then
        sendToast(player, "Unknown database table: " .. tostring(tableName), "error")
        return
    end

    mp.send(player.guid, "AdminUi_DatabasePage", copyDatabasePage(page))
end

function M.handleChat(player, data, env)
    local msg = trim(data.message or "")
    local prefix = env.commandPrefix or "/"
    local command = prefix .. "helpmenu"
    local legacyCommand = prefix .. "adminui"
    if msg ~= command and msg ~= legacyCommand then
        return nil
    end

    sendSnapshot(player, "AdminUi_Open", env)
    player:sendMessage("Opened the help menu.")
    return false
end

function M.handleRequest(data, env)
    local player = mp.getPlayer(data.pid or data.guid)
    if not player then
        return
    end

    local action = lower(data.action)
    if action == "" or action == "snapshot" then
        sendSnapshot(player, "AdminUi_Snapshot", env)
        return
    end

    if action == "create" then
        handleCreate(player, data, env)
        return
    end

    if action == "delete" then
        handleDelete(player, data, env)
        return
    end

    if action == "gc" then
        handleGc(player, data, env)
        return
    end

    if action == "sync" then
        handleSync(player, env)
        return
    end

    if action == "database" or action == "database_browse" then
        handleDatabaseBrowse(player, data)
        return
    end

    sendToast(player, "Unknown help menu action: " .. tostring(data.action), "error")
end

function M.handleHttpRequest(data, env)
    env = env or {}
    local pseudoPlayer = {
        guid = 0,
        name = "Web Browser",
    }

    local action = lower(data.action)
    if action == "" or action == "snapshot" then
        return jsonResponse(200, {
            ok = true,
            snapshot = buildSnapshot(pseudoPlayer, env),
        })
    end

    if action == "health" then
        return jsonResponse(200, buildHealthPayload())
    end

    if action == "database_tables" then
        local info = buildDatabaseInfo()
        info.ok = true
        return jsonResponse(200, info)
    end

    if action == "database" or action == "database_browse" then
        if not mp.browseDatabaseTable then
            return jsonResponse(503, {
                ok = false,
                error = "Database browsing is unavailable on this server build.",
            })
        end

        local tableName = trim(data.tableName or data.table)
        if not tableName or tableName == "" then
            return jsonResponse(400, {
                ok = false,
                error = "Database browse requires a tableName.",
            })
        end

        local offset = math.max(0, math.floor(tonumber(data.offset) or 0))
        local limit = math.max(1, math.floor(tonumber(data.limit) or 100))
        local page = mp.browseDatabaseTable(tableName, offset, limit)
        if not page then
            return jsonResponse(404, {
                ok = false,
                error = "Unknown database table: " .. tostring(tableName),
            })
        end

        return jsonResponse(200, {
            ok = true,
            page = copyDatabasePage(page),
        })
    end

    return jsonResponse(400, {
        ok = false,
        error = "Unknown admin HTTP action: " .. tostring(data.action),
    })
end

return M
