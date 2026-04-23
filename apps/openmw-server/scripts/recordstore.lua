local mp = require("mp")

local M = {}

local STORAGE_SECTION = "RecordStore"

local listCatalog

local TYPE_ALIASES = {
    misc = "miscellaneous",
}

local ENCHANTABLE_RECORD_TYPES = {
    armor = true,
    book = true,
    clothing = true,
    weapon = true,
}

local SPELL_DEPENDENT_RECORD_TYPES = {
    book = true,
    spell = true,
}

local SCOPE_ALIASES = {
    gen = "generated",
    generated = "generated",
    perm = "permanent",
    permanent = "permanent",
}

local PERSISTENCE_ALIASES = {
    persistent = true,
    persist = true,
    session = false,
    temp = false,
    temporary = false,
    all = "all",
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

local function getStateSection()
    assert(mp.storage and type(mp.storage.globalSection) == "function", "recordstore.lua requires mp.storage")
    return mp.storage.globalSection(STORAGE_SECTION)
end

local function loadState()
    local state = getStateSection():getCopy("records")
    if type(state) == "table" then
        return state
    end
    return {}
end

local function saveState(state)
    getStateSection():set("records", state)
end

local function normalizeRecordType(rawType)
    local normalized = TYPE_ALIASES[lower(rawType)] or lower(rawType)
    if normalized == "" then
        return nil
    end
    return normalized
end

local function normalizeScope(rawScope)
    if rawScope == nil then
        return nil
    end
    return SCOPE_ALIASES[lower(rawScope)]
end

local function normalizePersistence(rawValue)
    if rawValue == nil then
        return nil
    end
    return PERSISTENCE_ALIASES[lower(rawValue)]
end

local function appendDependency(dependencyIds, seen, recordId)
    recordId = trim(recordId)
    if not recordId or recordId == "" or seen[recordId] then
        return
    end

    seen[recordId] = true
    dependencyIds[#dependencyIds + 1] = recordId
end

local function collectDependencyIds(recordType, data, options)
    local dependencyIds = {}
    local seen = {}

    if type(options.dependencies) == "string" then
        appendDependency(dependencyIds, seen, options.dependencies)
    elseif type(options.dependencies) == "table" then
        for _, value in pairs(options.dependencies) do
            if type(value) == "string" then
                appendDependency(dependencyIds, seen, value)
            end
        end
    end

    if type(data) == "table" and ENCHANTABLE_RECORD_TYPES[recordType] then
        if type(data.enchant) == "string" then
            appendDependency(dependencyIds, seen, data.enchant)
        end
        if type(data.enchantmentId) == "string" then
            appendDependency(dependencyIds, seen, data.enchantmentId)
        end
    end

    if type(data) == "table" and SPELL_DEPENDENT_RECORD_TYPES[recordType] then
        if type(data.spell) == "string" then
            appendDependency(dependencyIds, seen, data.spell)
        end
        if type(data.spellId) == "string" then
            appendDependency(dependencyIds, seen, data.spellId)
        end
        if type(data.trap) == "string" then
            appendDependency(dependencyIds, seen, data.trap)
        end
        if type(data.trapSpell) == "string" then
            appendDependency(dependencyIds, seen, data.trapSpell)
        end
        if type(data.spells) == "table" then
            for _, value in pairs(data.spells) do
                if type(value) == "string" then
                    appendDependency(dependencyIds, seen, value)
                elseif type(value) == "table" then
                    if type(value.id) == "string" then
                        appendDependency(dependencyIds, seen, value.id)
                    end
                    if type(value.recordId) == "string" then
                        appendDependency(dependencyIds, seen, value.recordId)
                    end
                    if type(value.spellId) == "string" then
                        appendDependency(dependencyIds, seen, value.spellId)
                    end
                end
            end
        end
    end

    return dependencyIds
end

local function validateDependencyLifetimes(recordType, scope, persistent, dependencyIds)
    if #dependencyIds == 0 then
        return true
    end

    for _, dependencyId in ipairs(dependencyIds) do
        local found = false
        for _, entry in ipairs(listCatalog()) do
            if entry.recordId == dependencyId then
                found = true

                if scope == "permanent" and entry.scope == "generated" then
                    return false, string.format(
                        "Permanent %s record cannot depend on generated record %s.",
                        recordType,
                        dependencyId
                    )
                end

                if persistent and not entry.persistent then
                    return false, string.format(
                        "Persistent %s record cannot depend on session-only record %s.",
                        recordType,
                        dependencyId
                    )
                end

                break
            end
        end

        if not found then
            return false, string.format(
                "Dependency record %s was not found in the authoritative catalog.",
                dependencyId
            )
        end
    end

    return true
end

local function isArray(value)
    if type(value) ~= "table" then
        return false
    end

    local count = 0
    for key, _ in pairs(value) do
        if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then
            return false
        end
        count = count + 1
    end

    for index = 1, count do
        if value[index] == nil then
            return false
        end
    end

    return true
end

local function stableSerialize(value)
    local valueType = type(value)

    if valueType == "nil" then
        return "null"
    end
    if valueType == "boolean" then
        return value and "true" or "false"
    end
    if valueType == "number" then
        return string.format("%.17g", value)
    end
    if valueType == "string" then
        return string.format("%q", value)
    end
    if valueType ~= "table" then
        error("Unsupported value type for recordstore fingerprint: " .. valueType)
    end

    if isArray(value) then
        local parts = {}
        for index = 1, #value do
            parts[#parts + 1] = stableSerialize(value[index])
        end
        return "[" .. table.concat(parts, ",") .. "]"
    end

    local keys = {}
    for key, _ in pairs(value) do
        keys[#keys + 1] = key
    end

    table.sort(keys, function(left, right)
        local leftType = type(left)
        local rightType = type(right)
        if leftType ~= rightType then
            return leftType < rightType
        end
        return tostring(left) < tostring(right)
    end)

    local parts = {}
    for _, key in ipairs(keys) do
        parts[#parts + 1] = stableSerialize(key) .. ":" .. stableSerialize(value[key])
    end

    return "{" .. table.concat(parts, ",") .. "}"
end

local function fingerprintRecord(recordType, data)
    return stableSerialize({
        recordType = recordType,
        data = data,
    })
end

local function hashFingerprint(text)
    local hash = 5381
    for index = 1, #text do
        hash = (hash * 33 + string.byte(text, index)) % 4294967296
    end
    return string.format("%08x", hash)
end

local function deterministicPermanentId(recordType, fingerprint)
    return string.format("recordstore_%s_%s", recordType, hashFingerprint(fingerprint))
end

listCatalog = function()
    local source = mp.listDynamicRecords()
    if source == nil then
        return {}
    end

    local entries = {}
    for key, entry in pairs(source) do
        if type(key) == "number" and entry ~= nil then
            entries[#entries + 1] = entry
        end
    end

    table.sort(entries, function(left, right)
        local leftType = tostring(left.recordType or "")
        local rightType = tostring(right.recordType or "")
        if leftType ~= rightType then
            return leftType < rightType
        end
        return tostring(left.recordId or "") < tostring(right.recordId or "")
    end)

    return entries
end

local function buildCatalogIndex(entries)
    local index = {}
    for _, entry in ipairs(entries) do
        index[entry.recordType .. "\31" .. entry.recordId] = entry
    end
    return index
end

local function saveMetadata(state, recordType, recordId, metadata)
    state[recordType] = state[recordType] or {}
    state[recordType][recordId] = metadata
end

local function removeMetadata(state, recordType, recordId)
    local perType = state[recordType]
    if type(perType) ~= "table" then
        return
    end

    perType[recordId] = nil
    if next(perType) == nil then
        state[recordType] = nil
    end
end

local function syncStateInternal()
    local state = loadState()
    local catalogIndex = buildCatalogIndex(listCatalog())
    local removed = 0

    for recordType, records in pairs(state) do
        if type(records) ~= "table" then
            state[recordType] = nil
            removed = removed + 1
        else
            for recordId, _ in pairs(records) do
                if catalogIndex[recordType .. "\31" .. recordId] == nil then
                    removeMetadata(state, recordType, recordId)
                    removed = removed + 1
                end
            end
        end
    end

    if removed > 0 then
        saveState(state)
    end

    return state, catalogIndex, removed
end

local function findMatchingRecordId(state, catalogIndex, recordType, fingerprint, scope, persistent)
    local records = state[recordType]
    if type(records) ~= "table" then
        return nil
    end

    for recordId, metadata in pairs(records) do
        if type(metadata) == "table"
            and metadata.fingerprint == fingerprint
            and metadata.scope == scope
            and metadata.persistent == persistent
            and catalogIndex[recordType .. "\31" .. recordId] ~= nil then
            return recordId
        end
    end

    return nil
end

function M.syncState()
    local _, _, removed = syncStateInternal()
    return removed
end

function M.list(recordType)
    local normalizedType = normalizeRecordType(recordType)
    local entries = listCatalog()
    if not normalizedType then
        return entries
    end

    local filtered = {}
    for _, entry in ipairs(entries) do
        if entry.recordType == normalizedType then
            filtered[#filtered + 1] = entry
        end
    end
    return filtered
end

function M.getInfo(recordType, recordId)
    local normalizedType = normalizeRecordType(recordType)
    if not normalizedType or not recordId or recordId == "" then
        return nil
    end

    return mp.getDynamicRecordInfo(normalizedType, recordId)
end

function M.ensure(recordType, data, options)
    local normalizedType = normalizeRecordType(recordType)
    if not normalizedType or type(data) ~= "table" then
        return nil, "Invalid record type or payload."
    end

    options = options or {}

    local scope = normalizeScope(options.scope)
    local persistent = options.persistent
    if persistent == nil then
        persistent = true
    end

    local recordId = trim(options.recordId)
    if not scope then
        local generatedPrefix = (mp.getGeneratedRecordIdPrefix and mp.getGeneratedRecordIdPrefix() or "$custom") .. "_"
        scope = recordId and recordId ~= "" and recordId:sub(1, #generatedPrefix) == generatedPrefix
            and "generated" or "permanent"
    end

    if scope ~= "generated" and scope ~= "permanent" then
        return nil, "Unsupported record scope."
    end

    local fingerprint = fingerprintRecord(normalizedType, data)
    local allowReuse = options.allowReuse ~= false

    local state, catalogIndex = syncStateInternal()
    if allowReuse then
        local existingId = findMatchingRecordId(state, catalogIndex, normalizedType, fingerprint, scope, persistent)
        if existingId then
            local info = catalogIndex[normalizedType .. "\31" .. existingId]
            return {
                recordType = normalizedType,
                recordId = existingId,
                scope = scope,
                persistent = persistent,
                fingerprint = fingerprint,
                reused = true,
                info = info,
            }
        end
    end

    if not recordId or recordId == "" then
        if scope == "generated" then
            recordId = mp.generateDynamicRecordId(normalizedType)
        else
            recordId = deterministicPermanentId(normalizedType, fingerprint)
        end
    end

    if not recordId or recordId == "" then
        return nil, "Failed to allocate a record ID."
    end

    local ok = mp.upsertDynamicRecord(normalizedType, recordId, data, {
        scope = scope,
        persistent = persistent,
    })
    if not ok then
        return nil, "mp.upsertDynamicRecord returned false."
    end

    local dependencyIds = collectDependencyIds(normalizedType, data, options)
    local dependenciesValid, dependencyError = validateDependencyLifetimes(
        normalizedType,
        scope,
        persistent,
        dependencyIds
    )
    if not dependenciesValid then
        return nil, dependencyError
    end

    if type(mp.setDynamicRecordDependencies) == "function" then
        local dependencyOk = mp.setDynamicRecordDependencies(normalizedType, recordId, dependencyIds)
        if not dependencyOk then
            return nil, "mp.setDynamicRecordDependencies returned false."
        end
    end

    saveMetadata(state, normalizedType, recordId, {
        fingerprint = fingerprint,
        scope = scope,
        persistent = persistent,
    })
    saveState(state)

    return {
        recordType = normalizedType,
        recordId = recordId,
        scope = scope,
        persistent = persistent,
        fingerprint = fingerprint,
        reused = false,
        info = mp.getDynamicRecordInfo(normalizedType, recordId),
    }
end

function M.remove(recordType, recordId, options)
    local normalizedType = normalizeRecordType(recordType)
    recordId = trim(recordId)
    if not normalizedType or not recordId or recordId == "" then
        return false, "Invalid record type or id."
    end

    options = options or {}
    local info = mp.getDynamicRecordInfo(normalizedType, recordId)
    if not info then
        local state = loadState()
        removeMetadata(state, normalizedType, recordId)
        saveState(state)
        return false, "Record not found."
    end

    if info.linkCount > 0 and not options.force then
        return false, string.format("Record still has %d link(s).", info.linkCount)
    end

    if not mp.removeDynamicRecord(normalizedType, recordId) then
        return false, "mp.removeDynamicRecord returned false."
    end

    local state = loadState()
    removeMetadata(state, normalizedType, recordId)
    saveState(state)
    return true, info
end

function M.gcGeneratedUnlinked(options)
    options = options or {}

    local persistenceFilter = options.persistent
    if type(persistenceFilter) == "string" then
        persistenceFilter = normalizePersistence(persistenceFilter)
    end
    if persistenceFilter == "all" then
        persistenceFilter = nil
    end

    local normalizedType = normalizeRecordType(options.recordType)
    local removed = {}
    local state = loadState()

    if type(mp.gcDynamicRecords) == "function" then
        local authoritative = mp.gcDynamicRecords(normalizedType, persistenceFilter)
        if type(authoritative) == "table" then
            for _, entry in pairs(authoritative) do
                removeMetadata(state, entry.recordType, entry.recordId)
                removed[#removed + 1] = entry
            end
            if #removed > 0 then
                saveState(state)
                return removed
            end
        end
    end

    local seen = {}

    local function maybeRemove(recordType, recordId)
        local key = tostring(recordType or "") .. "\31" .. tostring(recordId or "")
        if seen[key] then
            return
        end
        seen[key] = true

        local info = M.getInfo(recordType, recordId)
        if not info then
            return
        end

        local scope = info.scope
        local linkCount = tonumber(info.linkCount) or 0
        local persistent = info.persistent

        if scope ~= "generated"
            or linkCount > 0
            or (persistenceFilter ~= nil and persistent ~= persistenceFilter)
            or (normalizedType ~= nil and info.recordType ~= normalizedType) then
            return
        end

        if mp.removeDynamicRecord(info.recordType, info.recordId) then
            removeMetadata(state, info.recordType, info.recordId)
            removed[#removed + 1] = info
        end
    end

    for recordType, records in pairs(state) do
        if type(records) == "table" then
            for recordId, _ in pairs(records) do
                maybeRemove(recordType, recordId)
            end
        end
    end

    for _, entry in ipairs(listCatalog()) do
        maybeRemove(entry.recordType, entry.recordId)
    end

    if #removed > 0 then
        saveState(state)
    end

    return removed
end

local function sendUsage(player, prefix)
    player:sendMessage(prefix .. "recordstore list [type|all]")
    player:sendMessage(prefix .. "recordstore info <type> <recordId>")
    player:sendMessage(prefix .. "recordstore info [type|all]")
    player:sendMessage(prefix .. "recordstore sync")
    player:sendMessage(prefix .. "recordstore gc [session|persistent|all] [type|all]")
end

local function sendList(player, entries, emptyMessage)
    if #entries == 0 then
        player:sendMessage(emptyMessage or "No dynamic records loaded.")
        return
    end

    for _, entry in ipairs(entries) do
        player:sendMessage(string.format(
            "%s %s %s/%s links=%d loaded=%s",
            entry.recordType,
            entry.recordId,
            entry.scope,
            entry.persistent and "persistent" or "session",
            entry.linkCount or 0,
            tostring(entry.loaded)
        ))
    end
end

local function sendInfoEntries(player, entries, emptyMessage)
    if #entries == 0 then
        player:sendMessage(emptyMessage or "No matching dynamic records loaded.")
        return
    end

    for _, entry in ipairs(entries) do
        player:sendMessage(string.format(
            "%s %s scope=%s persistent=%s links=%d loaded=%s createdAt=%s updatedAt=%s",
            entry.recordType,
            entry.recordId,
            entry.scope,
            tostring(entry.persistent),
            entry.linkCount or 0,
            tostring(entry.loaded),
            tostring(entry.createdAt or 0),
            tostring(entry.updatedAt or 0)
        ))
    end
end

function M.handleChat(player, data, env)
    local msg = trim(data.message or "")
    if not msg or msg == "" then
        return nil
    end

    local prefix = env.commandPrefix or "/"
    local command = prefix .. "recordstore"
    local rest

    if msg == command then
        rest = ""
    elseif msg:sub(1, #command + 1) == command .. " " then
        rest = msg:sub(#command + 1)
    else
        return nil
    end

    if not env.requireAdmin(player) then
        return false
    end

    local args, parseError = env.parseCommandArgs(rest)
    if not args then
        player:sendMessage(parseError or "Invalid arguments.")
        sendUsage(player, prefix)
        return false
    end

    if #args == 0 then
        sendUsage(player, prefix)
        return false
    end

    local subcommand = lower(args[1])
    if subcommand == "help" then
        sendUsage(player, prefix)
        return false
    end

    if subcommand == "list" then
        local target = lower(args[2] or "all")
        local entries = target == "all" and M.list() or M.list(target)
        if target ~= "all" and not normalizeRecordType(target) then
            player:sendMessage("Unknown record type.")
            return false
        end
        sendList(player, entries, target == "all"
            and "No dynamic records loaded."
            or ("No dynamic records loaded for type '" .. target .. "'."))
        return false
    end

    if subcommand == "info" then
        local recordType = args[2]
        local recordId = args[3]
        if not recordType then
            sendUsage(player, prefix)
            return false
        end

        local target = lower(recordType)
        if not recordId then
            if target == "all" then
                sendInfoEntries(player, M.list(), "No dynamic records loaded.")
                return false
            end

            local normalizedType = normalizeRecordType(recordType)
            if not normalizedType then
                player:sendMessage("Unknown record type.")
                return false
            end

            sendInfoEntries(player, M.list(normalizedType),
                "No dynamic records loaded for type '" .. normalizedType .. "'.")
            return false
        end

        local info = M.getInfo(recordType, recordId)
        if not info then
            player:sendMessage("Dynamic record not found.")
            return false
        end

        player:sendMessage(string.format(
            "%s %s scope=%s persistent=%s links=%d loaded=%s createdAt=%s updatedAt=%s",
            info.recordType,
            info.recordId,
            info.scope,
            tostring(info.persistent),
            info.linkCount or 0,
            tostring(info.loaded),
            tostring(info.createdAt or 0),
            tostring(info.updatedAt or 0)
        ))
        return false
    end

    if subcommand == "sync" then
        local removed = M.syncState()
        player:sendMessage(string.format("Recordstore metadata sync removed %d stale entr%s.", removed,
            removed == 1 and "y" or "ies"))
        return false
    end

    if subcommand == "gc" then
        local persistenceFilter = nil
        local persistenceFilterLabel = "all"
        local recordType = nil

        if args[2] then
            local parsedPersistence = normalizePersistence(args[2])
            if parsedPersistence ~= nil then
                if parsedPersistence == "all" then
                    persistenceFilter = nil
                    persistenceFilterLabel = "all"
                else
                    persistenceFilter = parsedPersistence and true or false
                    persistenceFilterLabel = persistenceFilter and "persistent" or "session"
                end
                recordType = args[3]
            else
                recordType = args[2]
            end
        end

        if recordType and lower(recordType) ~= "all" and not normalizeRecordType(recordType) then
            player:sendMessage("Unknown record type.")
            return false
        end

        local removed = M.gcGeneratedUnlinked({
            persistent = persistenceFilter,
            recordType = recordType and lower(recordType) ~= "all" and recordType or nil,
        })

        if #removed == 0 then
            player:sendMessage("No unlinked generated records were removed.")
            return false
        end

        for _, entry in ipairs(removed) do
            player:sendMessage(string.format(
                "Removed %s %s (%s/%s)",
                entry.recordType,
                entry.recordId,
                entry.scope,
                entry.persistent and "persistent" or "session"
            ))
        end
        return false
    end

    sendUsage(player, prefix)
    return false
end

function M.onServerInit()
    local removed = M.syncState()
    if removed > 0 then
        mp.log(string.format("[recordstore] pruned %d stale metadata entr%s on server init",
            removed, removed == 1 and "y" or "ies"))
    end
end

return M
