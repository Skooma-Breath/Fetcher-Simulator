local mp = require("mp")
local recordStore = require("recordstore")

local M = {}

local STORAGE_SECTION = "RecordDynamicTest"
local DEFAULT_SCOPE = "generated"
local DEFAULT_PERSISTENT = false

local function trim(text)
    if not text then
        return nil
    end
    return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function lower(text)
    return string.lower(trim(text) or "")
end

local SUPPORTED_TYPES = {
    "activator",
    "armor",
    "book",
    "clothing",
    "container",
    "creature",
    "door",
    "enchantment",
    "light",
    "miscellaneous",
    "npc",
    "potion",
    "spell",
    "static",
    "weapon",
}

local ENCHANTABLE_TEST_TYPES = {
    armor = true,
    book = true,
    clothing = true,
    weapon = true,
}

local MAGIC_EFFECT_PRESETS = {
    firebite = {
        id = 14,
        rangeType = 0,
        area = 0,
        duration = 5,
        magnitudeMin = 5,
        magnitudeMax = 10,
    },
    hearth = {
        id = 68,
        rangeType = 0,
        area = 0,
        duration = 1,
        magnitudeMin = 1,
        magnitudeMax = 1,
    },
    spark = {
        id = 12,
        rangeType = 2,
        area = 5,
        duration = 3,
        magnitudeMin = 3,
        magnitudeMax = 6,
    },
}

local TYPE_ALIASES = {
    misc = "miscellaneous",
}

local SCOPE_ALIASES = {
    gen = "generated",
    generated = "generated",
    perm = "permanent",
    permanent = "permanent",
}

local PERSISTENCE_ALIASES = {
    persist = true,
    persistent = true,
    session = false,
    temp = false,
    temporary = false,
}

local TEST_DEFINITIONS = {
    activator = {
        defaultBaseId = "active_de_p_bed_28",
        name = "RecordDynamic Test Activator",
        canPlace = true,
    },
    armor = {
        defaultBaseId = "orcish_cuirass",
        name = "RecordDynamic Test Armor",
        canPlace = true,
    },
    book = {
        defaultBaseId = "bk_guide_to_vvardenfell",
        name = "RecordDynamic Test Book",
        canPlace = true,
    },
    clothing = {
        defaultBaseId = "exquisite_robe_01",
        name = "RecordDynamic Test Clothing",
        canPlace = true,
    },
    container = {
        defaultBaseId = "chest_small_01",
        name = "RecordDynamic Test Container",
        canPlace = true,
    },
    creature = {
        defaultBaseId = "mudcrab",
        name = "RecordDynamic Test Creature",
        canPlace = false,
    },
    door = {
        defaultBaseId = "ex_common_door_01",
        name = "RecordDynamic Test Door",
        canPlace = true,
    },
    enchantment = {
        canPlace = false,
        hint = "use via enchanted item dependencies or future equipment tests",
        buildData = function(baseIdOverride)
            local baseId = trim(baseIdOverride)
            if baseId and baseId ~= "" then
                return {
                    baseId = baseId,
                }, baseId
            end

            return {
                type = 0,
                cost = 1,
                charge = 1,
                isAutocalc = false,
                effects = {},
            }, "blank defaults"
        end,
    },
    light = {
        defaultBaseId = "light_com_torch_01",
        name = "RecordDynamic Test Light",
        canPlace = true,
    },
    miscellaneous = {
        defaultBaseId = "misc_de_pot_blue_01",
        name = "RecordDynamic Test Misc",
        canPlace = true,
    },
    npc = {
        defaultBaseId = "fargoth",
        name = "RecordDynamic Test NPC",
        canPlace = false,
    },
    potion = {
        defaultBaseId = "p_restore_health_s",
        name = "RecordDynamic Test Potion",
        canPlace = true,
    },
    spell = {
        canPlace = false,
        hint = "use via spellbook, traps, or future dependency tests",
        buildData = function(baseIdOverride)
            local baseId = trim(baseIdOverride)
            if baseId and baseId ~= "" then
                return {
                    baseId = baseId,
                    name = "RecordDynamic Test Spell",
                }, baseId
            end

            return {
                name = "RecordDynamic Test Spell",
                type = 0,
                cost = 1,
                alwaysSucceedFlag = true,
                isAutocalc = false,
                effects = {},
            }, "blank defaults"
        end,
    },
    static = {
        defaultBaseId = "terrain_rock_wg_01",
        canPlace = true,
    },
    weapon = {
        defaultBaseId = "iron shortsword",
        name = "RecordDynamic Test Weapon",
        canPlace = true,
    },
}

local function getStateSection()
    assert(mp.storage and type(mp.storage.globalSection) == "function", "recorddynamic_test.lua requires mp.storage")
    return mp.storage.globalSection(STORAGE_SECTION)
end

local function loadGeneratedIds()
    return getStateSection():getCopy("generatedIds") or {}
end

local function saveGeneratedIds(generatedIds)
    getStateSection():set("generatedIds", generatedIds)
end

local function slotName(persistent)
    return persistent and "persistent" or "session"
end

local function stablePermanentId(recordType)
    return "recordtest_" .. recordType
end

local function normalizeRecordType(rawType)
    local normalized = TYPE_ALIASES[lower(rawType)] or lower(rawType)
    if TEST_DEFINITIONS[normalized] then
        return normalized
    end
    return nil
end

local function resolveTargetTypes(rawType)
    local normalized = lower(rawType)
    if normalized == "all" then
        return SUPPORTED_TYPES
    end

    local recordType = normalizeRecordType(rawType)
    if not recordType then
        return nil
    end

    return { recordType }
end

local function formatScope(scope, persistent)
    return scope .. "/" .. slotName(persistent)
end

local function formatPlaceHint(prefix, recordType, recordId)
    local definition = TEST_DEFINITIONS[recordType]
    if definition and definition.hint then
        return definition.hint
    end

    if definition and definition.canPlace then
        return "use " .. prefix .. "placeat " .. recordId
    end

    return "/spawnat still needs actor authority"
end

local function buildMagicEffects(presetName)
    local preset = MAGIC_EFFECT_PRESETS[lower(presetName)]
    if not preset then
        return nil, "Unknown magic preset. Use one of: firebite, hearth, spark."
    end

    return {
        {
            id = preset.id,
            rangeType = preset.rangeType,
            area = preset.area,
            duration = preset.duration,
            magnitudeMin = preset.magnitudeMin,
            magnitudeMax = preset.magnitudeMax,
        }
    }
end

local function sendUsage(player, prefix)
    player:sendMessage(prefix .. "recordtest types")
    player:sendMessage(prefix .. "recordtest info <type|all>")
    player:sendMessage(prefix ..
    "recordtest create <type|all> [generated|permanent] [persistent|session] [baseId|\"base id\"]")
    player:sendMessage(prefix ..
    "recordtest enchant <weapon|armor|book|clothing> [generated|permanent] [persistent|session] [itemBaseId|\"item base\"] [enchantBaseId|\"enchant base\"]")
    player:sendMessage(prefix ..
    "recordtest spell <generated|permanent> <persistent|session> [preset] [baseId|\"base id\"]")
    player:sendMessage(prefix ..
    "recordtest trap <book|spell> <generated|permanent> <persistent|session> [preset] [baseId|\"base id\"]")
    player:sendMessage(prefix .. "recordtest remove <type|all>")
    player:sendMessage("Defaults to generated/session so test records do not stay in the DB.")
end

local function sendTypes(player)
    player:sendMessage("RecordDynamic test types:")
    for _, recordType in ipairs(SUPPORTED_TYPES) do
        local definition = TEST_DEFINITIONS[recordType]
        player:sendMessage(string.format("  %s -> baseId=%s", recordType, definition.defaultBaseId or "blank defaults"))
    end
end

local function clearSessionGeneratedIds()
    local generatedIds = loadGeneratedIds()
    local changed = false

    for recordType, slots in pairs(generatedIds) do
        if type(slots) == "table" and slots.session ~= nil then
            slots.session = nil
            changed = true
        end
        if type(slots) ~= "table" or (slots.persistent == nil and slots.session == nil) then
            generatedIds[recordType] = nil
            changed = true
        end
    end

    if changed then
        saveGeneratedIds(generatedIds)
    end
end

local function getTrackedGeneratedId(recordType, persistent)
    local generatedIds = loadGeneratedIds()
    local slots = generatedIds[recordType]
    if type(slots) ~= "table" then
        return nil
    end
    return slots[slotName(persistent)]
end

local function rememberGeneratedId(recordType, persistent, recordId)
    local generatedIds = loadGeneratedIds()
    generatedIds[recordType] = generatedIds[recordType] or {}
    generatedIds[recordType][slotName(persistent)] = recordId
    saveGeneratedIds(generatedIds)
end

local function forgetGeneratedId(recordType, persistent)
    local generatedIds = loadGeneratedIds()
    local slots = generatedIds[recordType]
    if type(slots) ~= "table" then
        return
    end

    slots[slotName(persistent)] = nil
    if slots.persistent == nil and slots.session == nil then
        generatedIds[recordType] = nil
    end
    saveGeneratedIds(generatedIds)
end

local function buildRecordData(recordType, baseIdOverride)
    local definition = TEST_DEFINITIONS[recordType]
    if definition.buildData then
        return definition.buildData(baseIdOverride)
    end

    local baseId = trim(baseIdOverride) or definition.defaultBaseId
    if not baseId or baseId == "" then
        return nil, "No baseId available for " .. recordType .. "."
    end

    local data = {
        baseId = baseId,
    }

    if definition.name then
        data.name = definition.name
    end

    return data, baseId
end

local function removeTrackedGeneratedRecord(recordType, persistent)
    local recordId = getTrackedGeneratedId(recordType, persistent)
    if not recordId then
        return nil
    end

    recordStore.remove(recordType, recordId, { force = true })
    forgetGeneratedId(recordType, persistent)
    return recordId
end

local function upsertTestRecord(recordType, scope, persistent, data, baseIdOrLabel, options)
    if scope ~= "generated" and scope ~= "permanent" then
        return nil, "Unsupported record scope: " .. tostring(scope)
    end

    options = options or {}

    local recordId
    if scope == "generated" then
        if options.replaceTrackedGenerated ~= false then
            removeTrackedGeneratedRecord(recordType, persistent)
        end
        recordId = mp.generateDynamicRecordId(recordType)
        if not recordId then
            return nil, "Failed to allocate a generated ID for " .. recordType .. "."
        end
    else
        recordId = stablePermanentId(recordType)
    end

    local stored, storeErr = recordStore.ensure(recordType, data, {
        scope = scope,
        persistent = persistent,
        recordId = recordId,
        allowReuse = scope == "permanent",
    })
    if not stored then
        return nil, storeErr or ("recordStore.ensure failed for " .. recordType .. ".")
    end

    if scope == "generated" then
        recordId = stored.recordId
        rememberGeneratedId(recordType, persistent, recordId)
    end

    return {
        recordType = recordType,
        recordId = stored.recordId,
        baseId = baseIdOrLabel,
        scope = scope,
        persistent = persistent,
        reused = stored.reused == true,
    }
end

local function createTestRecord(recordType, scope, persistent, baseIdOverride)
    local data, baseIdOrError = buildRecordData(recordType, baseIdOverride)
    if not data then
        return nil, baseIdOrError
    end

    return upsertTestRecord(recordType, scope, persistent, data, baseIdOrError)
end

local function discardCreatedRecord(result)
    if not result or result.reused then
        return
    end

    recordStore.remove(result.recordType, result.recordId, { force = true })
    if result.scope == "generated" then
        forgetGeneratedId(result.recordType, result.persistent)
    end
end

local function removeKnownRecords(recordType)
    local removed = {}
    local permanentId = stablePermanentId(recordType)

    local removedPermanent = recordStore.remove(recordType, permanentId, { force = true })
    if removedPermanent then
        table.insert(removed, {
            recordId = permanentId,
            detail = "permanent",
        })
    end

    for _, persistent in ipairs({ true, false }) do
        local recordId = getTrackedGeneratedId(recordType, persistent)
        if recordId then
            local removedFromServer = recordStore.remove(recordType, recordId, { force = true })
            table.insert(removed, {
                recordId = recordId,
                detail = removedFromServer and formatScope("generated", persistent)
                    or formatScope("generated", persistent) .. " (tracking only)",
            })
            forgetGeneratedId(recordType, persistent)
        end
    end

    return removed
end

local function sendInfo(player, prefix, recordTypes)
    local function describe(recordType, recordId)
        local info = recordId and recordStore.getInfo(recordType, recordId)
        if not info then
            return "missing"
        end

        return string.format(
            "%s/%s links=%d loaded=%s",
            info.scope,
            info.persistent and "persistent" or "session",
            info.linkCount or 0,
            tostring(info.loaded)
        )
    end

    for _, recordType in ipairs(recordTypes) do
        local definition = TEST_DEFINITIONS[recordType]
        local permanentId = stablePermanentId(recordType)
        local generatedPersistentId = getTrackedGeneratedId(recordType, true) or "none"
        local generatedSessionId = getTrackedGeneratedId(recordType, false) or "none"
        player:sendMessage(string.format(
            "%s: baseId=%s permanentId=%s [%s] generatedPersistent=%s [%s] generatedSession=%s [%s] hint=%s",
            recordType,
            definition.defaultBaseId or "blank defaults",
            permanentId,
            describe(recordType, permanentId),
            generatedPersistentId,
            describe(recordType, generatedPersistentId ~= "none" and generatedPersistentId or nil),
            generatedSessionId,
            describe(recordType, generatedSessionId ~= "none" and generatedSessionId or nil),
            formatPlaceHint(prefix, recordType, "<id>")
        ))
    end
end

local function parseCreateOptions(args)
    local scope = DEFAULT_SCOPE
    local persistent = DEFAULT_PERSISTENT
    local baseId

    for index = 3, #args do
        local token = args[index]
        local normalizedScope = SCOPE_ALIASES[lower(token)]
        if normalizedScope and scope == DEFAULT_SCOPE then
            scope = normalizedScope
        else
            local normalizedPersistent = PERSISTENCE_ALIASES[lower(token)]
            if normalizedPersistent ~= nil and persistent == DEFAULT_PERSISTENT then
                persistent = normalizedPersistent
            elseif not baseId then
                baseId = token
            else
                return nil, "Too many arguments. Quote baseIds that contain spaces."
            end
        end
    end

    return {
        scope = scope,
        persistent = persistent,
        baseId = baseId,
    }
end

local function parseEnchantOptions(args)
    local scope = DEFAULT_SCOPE
    local persistent = DEFAULT_PERSISTENT
    local itemBaseId
    local enchantBaseId

    for index = 3, #args do
        local token = args[index]
        local normalizedScope = SCOPE_ALIASES[lower(token)]
        if normalizedScope and scope == DEFAULT_SCOPE then
            scope = normalizedScope
        else
            local normalizedPersistent = PERSISTENCE_ALIASES[lower(token)]
            if normalizedPersistent ~= nil and persistent == DEFAULT_PERSISTENT then
                persistent = normalizedPersistent
            elseif not itemBaseId then
                itemBaseId = token
            elseif not enchantBaseId then
                enchantBaseId = token
            else
                return nil, "Too many arguments. Quote baseIds that contain spaces."
            end
        end
    end

    return {
        scope = scope,
        persistent = persistent,
        itemBaseId = itemBaseId,
        enchantBaseId = enchantBaseId,
    }
end

local function parseMagicOptions(args, startIndex)
    local scope = DEFAULT_SCOPE
    local persistent = DEFAULT_PERSISTENT
    local preset = "firebite"
    local baseId

    for index = startIndex, #args do
        local token = args[index]
        local normalizedScope = SCOPE_ALIASES[lower(token)]
        if normalizedScope and scope == DEFAULT_SCOPE then
            scope = normalizedScope
        else
            local normalizedPersistent = PERSISTENCE_ALIASES[lower(token)]
            if normalizedPersistent ~= nil and persistent == DEFAULT_PERSISTENT then
                persistent = normalizedPersistent
            elseif preset == "firebite" and MAGIC_EFFECT_PRESETS[lower(token)] then
                preset = lower(token)
            elseif not baseId then
                baseId = token
            else
                return nil, "Too many arguments. Quote baseIds that contain spaces."
            end
        end
    end

    return {
        scope = scope,
        persistent = persistent,
        preset = preset,
        baseId = baseId,
    }
end

local function sendCreateResults(player, prefix, results)
    for _, result in ipairs(results) do
        player:sendMessage(string.format(
            "%s: upserted %s from %s (%s); %s",
            result.recordType,
            result.recordId,
            result.baseId,
            formatScope(result.scope, result.persistent),
            formatPlaceHint(prefix, result.recordType, result.recordId)
        ))
    end
end

local function handleCreate(player, args, env)
    local recordTypes = resolveTargetTypes(args[2])
    if not recordTypes then
        player:sendMessage("Unknown record type. Use /recordtest types.")
        return false
    end

    local options, optionError = parseCreateOptions(args)
    if not options then
        player:sendMessage(optionError)
        sendUsage(player, env.commandPrefix)
        return false
    end

    if #recordTypes > 1 and options.baseId then
        player:sendMessage("A custom baseId can only be used with a single record type.")
        return false
    end

    local results = {}
    for _, recordType in ipairs(recordTypes) do
        local result, err = createTestRecord(recordType, options.scope, options.persistent, options.baseId)
        if not result then
            player:sendMessage(recordType .. ": " .. err)
        else
            table.insert(results, result)
            mp.log(string.format(
                "[recorddynamic_test] create by %s type=%s id=%s baseId=%s scope=%s persistent=%s reused=%s",
                player.name,
                recordType,
                result.recordId,
                result.baseId,
                result.scope,
                tostring(result.persistent),
                tostring(result.reused)
            ))
        end
    end

    if #results == 0 then
        player:sendMessage("No records were created.")
        return false
    end

    sendCreateResults(player, env.commandPrefix, results)
    return false
end

local function handleEnchant(player, args, env)
    local recordType = normalizeRecordType(args[2])
    if not recordType or not ENCHANTABLE_TEST_TYPES[recordType] then
        player:sendMessage("Enchanted test records only support weapon, armor, book, or clothing.")
        return false
    end

    local options, optionError = parseEnchantOptions(args)
    if not options then
        player:sendMessage(optionError)
        sendUsage(player, env.commandPrefix)
        return false
    end

    if options.scope == "generated" then
        removeTrackedGeneratedRecord(recordType, options.persistent)
        removeTrackedGeneratedRecord("enchantment", options.persistent)
    end

    local enchantData, enchantBase = buildRecordData("enchantment", options.enchantBaseId)
    if not enchantData then
        player:sendMessage(enchantBase)
        return false
    end

    local enchantmentResult, enchantErr = upsertTestRecord(
        "enchantment", options.scope, options.persistent, enchantData, enchantBase, { replaceTrackedGenerated = false })
    if not enchantmentResult then
        player:sendMessage("enchantment: " .. enchantErr)
        return false
    end

    local itemData, itemBase = buildRecordData(recordType, options.itemBaseId)
    if not itemData then
        discardCreatedRecord(enchantmentResult)
        player:sendMessage(itemBase)
        return false
    end

    itemData.enchant = enchantmentResult.recordId
    if itemData.name then
        itemData.name = itemData.name .. " (Enchanted)"
    end

    local itemResult, itemErr = upsertTestRecord(
        recordType,
        options.scope,
        options.persistent,
        itemData,
        itemBase .. " + " .. enchantmentResult.recordId,
        { replaceTrackedGenerated = false }
    )
    if not itemResult then
        discardCreatedRecord(enchantmentResult)
        player:sendMessage(recordType .. ": " .. itemErr)
        return false
    end

    sendCreateResults(player, env.commandPrefix, { enchantmentResult, itemResult })
    player:sendMessage(string.format(
        "%s: %s now depends on enchantment %s",
        recordType,
        itemResult.recordId,
        enchantmentResult.recordId
    ))

    mp.log(string.format(
        "[recorddynamic_test] enchant by %s type=%s id=%s enchant=%s scope=%s persistent=%s",
        player.name,
        recordType,
        itemResult.recordId,
        enchantmentResult.recordId,
        itemResult.scope,
        tostring(itemResult.persistent)
    ))

    return false
end

local function handleSpell(player, args, env)
    local options, optionError = parseMagicOptions(args, 2)
    if not options then
        player:sendMessage(optionError)
        sendUsage(player, env.commandPrefix)
        return false
    end

    if options.scope == "generated" then
        removeTrackedGeneratedRecord("spell", options.persistent)
    end

    local spellData, spellBase = buildRecordData("spell", options.baseId)
    if not spellData then
        player:sendMessage(spellBase)
        return false
    end

    local effects, effectError = buildMagicEffects(options.preset)
    if not effects then
        player:sendMessage(effectError)
        return false
    end

    spellData.effects = effects
    spellData.name = string.format("RecordDynamic Test Spell (%s)", options.preset)

    local result, err = upsertTestRecord("spell", options.scope, options.persistent, spellData, spellBase)
    if not result then
        player:sendMessage("spell: " .. err)
        return false
    end

    sendCreateResults(player, env.commandPrefix, { result })
    player:sendMessage(string.format(
        "spell: %s uses preset %s with %d effect(s)",
        result.recordId,
        options.preset,
        #effects
    ))
    return false
end

local function handleTrap(player, args, env)
    local targetType = normalizeRecordType(args[2])
    if targetType ~= "book" and targetType ~= "spell" then
        player:sendMessage("Trap test records only support book or spell.")
        return false
    end

    local options, optionError = parseMagicOptions(args, 3)
    if not options then
        player:sendMessage(optionError)
        sendUsage(player, env.commandPrefix)
        return false
    end

    if options.scope == "generated" then
        removeTrackedGeneratedRecord("spell", options.persistent)
        removeTrackedGeneratedRecord(targetType, options.persistent)
    end

    local trapSpellData, trapSpellBase = buildRecordData("spell", nil)
    if not trapSpellData then
        player:sendMessage(trapSpellBase)
        return false
    end

    local effects, effectError = buildMagicEffects(options.preset)
    if not effects then
        player:sendMessage(effectError)
        return false
    end

    trapSpellData.effects = effects
    trapSpellData.name = string.format("RecordDynamic Trap Spell (%s)", options.preset)

    local trapSpellResult, trapSpellErr = upsertTestRecord(
        "spell",
        options.scope,
        options.persistent,
        trapSpellData,
        "trap spell " .. options.preset,
        { replaceTrackedGenerated = false }
    )
    if not trapSpellResult then
        player:sendMessage("spell: " .. trapSpellErr)
        return false
    end

    local targetData, targetBase = buildRecordData(targetType, options.baseId)
    if not targetData then
        discardCreatedRecord(trapSpellResult)
        player:sendMessage(targetBase)
        return false
    end

    if targetType == "book" then
        targetData.trap = trapSpellResult.recordId
    else
        targetData.spell = trapSpellResult.recordId
        targetData.name = string.format("RecordDynamic Trap Carrier (%s)", options.preset)
    end

    local targetResult, targetErr = upsertTestRecord(
        targetType,
        options.scope,
        options.persistent,
        targetData,
        targetBase .. " + trap " .. trapSpellResult.recordId,
        { replaceTrackedGenerated = false }
    )
    if not targetResult then
        discardCreatedRecord(trapSpellResult)
        player:sendMessage(targetType .. ": " .. targetErr)
        return false
    end

    sendCreateResults(player, env.commandPrefix, { trapSpellResult, targetResult })
    player:sendMessage(string.format(
        "%s: %s now depends on trap spell %s",
        targetType,
        targetResult.recordId,
        trapSpellResult.recordId
    ))
    return false
end

local function handleRemove(player, args)
    local recordTypes = resolveTargetTypes(args[2])
    if not recordTypes then
        player:sendMessage("Unknown record type. Use /recordtest types.")
        return false
    end

    local removedAny = false
    for _, recordType in ipairs(recordTypes) do
        local removed = removeKnownRecords(recordType)
        if #removed == 0 then
            player:sendMessage(recordType .. ": no known test records to remove.")
        else
            removedAny = true
            for _, entry in ipairs(removed) do
                player:sendMessage(string.format("%s: removed %s (%s)", recordType, entry.recordId, entry.detail))
            end
        end
    end

    if removedAny then
        mp.log(string.format("[recorddynamic_test] remove by %s target=%s", player.name, tostring(args[2])))
    end

    return false
end

function M.handleChat(player, data, env)
    local msg = trim(data.message or "")
    if not msg or msg == "" then
        return nil
    end

    local prefix = env.commandPrefix or "/"
    local command = prefix .. "recordtest"
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
    if subcommand == "types" then
        sendTypes(player)
        return false
    end
    if subcommand == "info" or subcommand == "defaults" then
        local recordTypes = resolveTargetTypes(args[2] or "all")
        if not recordTypes then
            player:sendMessage("Unknown record type. Use /recordtest types.")
            return false
        end
        sendInfo(player, prefix, recordTypes)
        return false
    end
    if subcommand == "create" then
        if not args[2] then
            sendUsage(player, prefix)
            return false
        end
        return handleCreate(player, args, env)
    end
    if subcommand == "enchant" then
        if not args[2] then
            sendUsage(player, prefix)
            return false
        end
        return handleEnchant(player, args, env)
    end
    if subcommand == "spell" then
        return handleSpell(player, args, env)
    end
    if subcommand == "trap" then
        if not args[2] then
            sendUsage(player, prefix)
            return false
        end
        return handleTrap(player, args, env)
    end
    if subcommand == "remove" then
        if not args[2] then
            sendUsage(player, prefix)
            return false
        end
        return handleRemove(player, args)
    end

    player:sendMessage("Unknown /recordtest subcommand.")
    sendUsage(player, prefix)
    return false
end

function M.onServerInit()
    clearSessionGeneratedIds()
end

return M
