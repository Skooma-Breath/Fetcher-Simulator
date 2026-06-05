local mp = require("mp")

local M = {}

local STORAGE_NAMESPACE = "Bardcraft"
local STORAGE_KEYS = {
    "performerStats",
    "knownSongs",
    "troupeRecords",
    "customSongs",
}

local MAX_HOSTED_MIDI_BYTES_PER_RESPONSE = 512 * 1024
local KNOWN_SONGS_PER_BOOTSTRAP_CHUNK = 40
local CUSTOM_SONG_ENCODED_CHARS_PER_CHUNK = 8192
local MAX_CUSTOM_SONGS_PER_CHARACTER = 256
local MAX_CUSTOM_SONG_ENCODED_CHARS = 1024 * 1024
local bootstrapSequence = 0

local function senderGuid(data)
    local guid = data and tonumber(data.pid) or 0
    return guid and guid > 0 and guid or nil
end

local function senderCharacterId(data)
    local characterId = data and tonumber(data.characterId) or 0
    return characterId and characterId > 0 and characterId or nil
end

local function tableCount(value)
    if type(value) ~= "table" then
        return 0
    end

    local count = 0
    for _, _ in pairs(value) do
        count = count + 1
    end
    return count
end

local function knownSongCount(stats)
    return type(stats) == "table" and tableCount(stats.knownSongs) or 0
end

local function directKnownSongCount(knownSongs)
    return tableCount(knownSongs)
end

local function knownSongsFromState(data)
    if type(data) ~= "table" then
        return nil
    end
    if type(data.knownSongs) == "table" then
        return data.knownSongs
    end
    if type(data.performerStats) == "table" and type(data.performerStats.knownSongs) == "table" then
        return data.performerStats.knownSongs
    end
    return nil
end

local function copyPerformerStatsWithoutKnownSongs(stats)
    if type(stats) ~= "table" then
        return stats
    end

    local copy = {}
    for key, value in pairs(stats) do
        if key ~= "knownSongs" then
            copy[key] = value
        end
    end
    return copy
end

local function makeHostedMidiManifest()
    local payload = {
        files = {},
        skipped = 0,
        totalBytes = 0,
    }

    if type(mp.listBardcraftHostedMidiFiles) ~= "function"
        or type(mp.readBardcraftHostedMidiFile) ~= "function"
    then
        return payload
    end

    local files = mp.listBardcraftHostedMidiFiles() or {}
    for _, entry in ipairs(files) do
        local fileName = type(entry) == "table" and entry.name or nil
        local size = type(entry) == "table" and tonumber(entry.size) or 0
        if fileName and size and size > 0 then
            payload.totalBytes = payload.totalBytes + size
            table.insert(payload.files, {
                name = fileName,
                size = size,
            })
        elseif fileName then
            payload.skipped = payload.skipped + 1
        end
    end

    return payload
end

local function sendHostedMidiCatalog(guid)
    local payload = makeHostedMidiManifest()
    mp.log(string.format(
        "[bardcraft] hosted midi manifest guid=%s files=%d bytes=%d skipped=%d",
        tostring(guid),
        tableCount(payload.files),
        tonumber(payload.totalBytes) or 0,
        tonumber(payload.skipped) or 0))
    mp.send(guid, "BC_BardcraftServerSongs", payload)
end

local function requestedNameSet(names)
    local set = {}
    if type(names) ~= "table" then
        return set
    end
    for _, name in ipairs(names) do
        if type(name) == "string" then
            set[name] = true
        end
    end
    return set
end

local function sendHostedMidiFiles(guid, names)
    local requested = requestedNameSet(names)
    local payload = {
        files = {},
        skipped = 0,
        skippedNames = {},
        totalBytes = 0,
    }

    if type(mp.listBardcraftHostedMidiFiles) == "function"
        and type(mp.readBardcraftHostedMidiFile) == "function"
    then
        local files = mp.listBardcraftHostedMidiFiles() or {}
        for _, entry in ipairs(files) do
            local fileName = type(entry) == "table" and entry.name or nil
            local size = type(entry) == "table" and tonumber(entry.size) or 0
            if fileName and requested[fileName] then
                if size and size > 0 and payload.totalBytes + size <= MAX_HOSTED_MIDI_BYTES_PER_RESPONSE then
                    local bytes = mp.readBardcraftHostedMidiFile(fileName)
                    if type(bytes) == "string" then
                        payload.totalBytes = payload.totalBytes + #bytes
                        table.insert(payload.files, {
                            name = fileName,
                            size = #bytes,
                            bytes = bytes,
                        })
                    else
                        payload.skipped = payload.skipped + 1
                        table.insert(payload.skippedNames, fileName)
                    end
                else
                    payload.skipped = payload.skipped + 1
                    table.insert(payload.skippedNames, fileName)
                end
            end
        end
    end

    mp.log(string.format(
        "[bardcraft] hosted midi files guid=%s requested=%d files=%d bytes=%d skipped=%d",
        tostring(guid),
        tableCount(names),
        tableCount(payload.files),
        tonumber(payload.totalBytes) or 0,
        tonumber(payload.skipped) or 0))
    mp.send(guid, "BC_BardcraftServerSongFiles", payload)
end

local function sortedTableKeys(value)
    local keys = {}
    if type(value) ~= "table" then
        return keys
    end

    for key, _ in pairs(value) do
        table.insert(keys, key)
    end
    table.sort(keys, function(left, right)
        return tostring(left) < tostring(right)
    end)
    return keys
end

local function nextBootstrapToken(characterId)
    bootstrapSequence = bootstrapSequence + 1
    return tostring(characterId) .. ":" .. tostring(bootstrapSequence)
end

local function sendKnownSongChunks(guid, token, knownSongs)
    local entries = {}
    local sent = 0
    for _, songId in ipairs(sortedTableKeys(knownSongs)) do
        table.insert(entries, {
            id = songId,
            value = knownSongs[songId],
        })
        if #entries >= KNOWN_SONGS_PER_BOOTSTRAP_CHUNK then
            sent = sent + #entries
            mp.send(guid, "BC_BardcraftKnownSongsChunk", {
                token = token,
                entries = entries,
            })
            entries = {}
        end
    end

    if #entries > 0 then
        sent = sent + #entries
        mp.send(guid, "BC_BardcraftKnownSongsChunk", {
            token = token,
            entries = entries,
        })
    end

    return sent
end

local function jsonEscape(value)
    return "\"" .. tostring(value):gsub('[%z\1-\31\\"]', function(char)
        return string.format("\\u%04x", char:byte())
    end) .. "\""
end

local function jsonEncode(value)
    local valueType = type(value)
    if valueType == "string" then
        return jsonEscape(value)
    elseif valueType == "number" or valueType == "boolean" then
        return tostring(value)
    elseif valueType == "table" then
        local isArray = #value > 0
        local out = {}
        if isArray then
            for _, entry in ipairs(value) do
                table.insert(out, jsonEncode(entry))
            end
            return "[" .. table.concat(out, ",") .. "]"
        end

        for key, entry in pairs(value) do
            if entry ~= nil then
                table.insert(out, jsonEscape(key) .. ":" .. jsonEncode(entry))
            end
        end
        return "{" .. table.concat(out, ",") .. "}"
    end

    return "null"
end

local function toBase64(data)
    local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    return ((data:gsub(".", function(char)
        local bits, byte = "", char:byte()
        for index = 8, 1, -1 do
            bits = bits .. (byte % 2 ^ index - byte % 2 ^ (index - 1) > 0 and "1" or "0")
        end
        return bits
    end) .. "0000"):gsub("%d%d%d?%d?%d?%d?", function(bits)
        if #bits < 6 then
            return ""
        end
        local value = 0
        for index = 1, 6 do
            value = value + (bits:sub(index, index) == "1" and 2 ^ (6 - index) or 0)
        end
        return alphabet:sub(value + 1, value + 1)
    end) .. ({ "", "==", "=" })[#data % 3 + 1])
end

local function encodeCustomSong(song)
    if type(song) ~= "table" then
        return nil
    end

    if type(song.encoded) == "string" then
        return {
            encoded = song.encoded,
            id = song.id,
            title = song.title,
            desc = song.desc,
            sourceFile = song.sourceFile,
            texture = song.texture,
            difficulty = song.difficulty,
            parts = song.parts,
            isServerCustom = song.isServerCustom == true,
            serverHosted = song.serverHosted == true,
        }
    end

    local noteStrings = {}
    for _, note in ipairs(type(song.notes) == "table" and song.notes or {}) do
        table.insert(noteStrings, string.format(
            "%d|%s|%d|%d|%d|%d",
            tonumber(note.id) or 0,
            tostring(note.type or ""),
            tonumber(note.note) or 0,
            tonumber(note.velocity) or 0,
            tonumber(note.part) or 0,
            tonumber(note.time) or 0))
    end

    local songData = {
        id = song.id or song.sourceFile or song.title or "custom",
        title = song.title or song.id or song.sourceFile or "Custom Song",
        desc = song.desc or "",
        tempo = tonumber(song.tempo) or 120,
        timeSig = type(song.timeSig) == "table" and song.timeSig or { 4, 4 },
        lengthBars = tonumber(song.lengthBars) or 4,
        loopBars = type(song.loopBars) == "table" and song.loopBars or { 0, tonumber(song.lengthBars) or 4 },
        resolution = tonumber(song.resolution) or 96,
        notes = table.concat(noteStrings, ","),
        parts = type(song.parts) == "table" and song.parts or {},
    }

    return {
        encoded = toBase64(jsonEncode(songData)),
        id = songData.id,
        title = songData.title,
        desc = songData.desc,
        sourceFile = song.sourceFile,
        texture = song.texture,
        difficulty = song.difficulty,
        parts = songData.parts,
    }
end

local function songRevision(encoded)
    if type(encoded) ~= "string" then
        return "0:0"
    end

    local hash = 0
    for index = 1, #encoded do
        hash = (hash * 131 + encoded:byte(index)) % 2147483647
    end
    return tostring(#encoded) .. ":" .. tostring(hash)
end

local function copyParts(parts)
    local copy = {}
    if type(parts) ~= "table" then
        return copy
    end

    for _, part in ipairs(parts) do
        if type(part) == "table" then
            table.insert(copy, {
                index = part.index,
                instrument = part.instrument,
                title = part.title,
                numOfType = part.numOfType,
            })
        end
    end
    return copy
end

local function makeCustomSongManifestEntry(song, storageIndex)
    local encodedSong = encodeCustomSong(song)
    if not encodedSong or type(encodedSong.encoded) ~= "string" then
        return nil
    end

    local encodedLength = #encodedSong.encoded
    if encodedLength <= 0 or encodedLength > MAX_CUSTOM_SONG_ENCODED_CHARS then
        return nil
    end

    return {
        id = encodedSong.id,
        title = encodedSong.title,
        desc = encodedSong.desc,
        sourceFile = encodedSong.sourceFile,
        texture = encodedSong.texture,
        difficulty = encodedSong.difficulty,
        parts = copyParts(encodedSong.parts),
        encodedLength = encodedLength,
        revision = songRevision(encodedSong.encoded),
        storageIndex = storageIndex,
        requiresServerFetch = true,
        isMpCustom = true,
        isServerCustom = encodedSong.isServerCustom == true,
        serverHosted = encodedSong.serverHosted == true,
    }
end

local function makeCustomSongManifest(customSongs)
    local manifest = {}
    local skipped = 0
    for index, song in ipairs(type(customSongs) == "table" and customSongs or {}) do
        local entry = makeCustomSongManifestEntry(song, index)
        if entry then
            table.insert(manifest, entry)
        else
            skipped = skipped + 1
        end
    end
    return manifest, skipped
end

local function requestedIdSet(ids)
    local set = {}
    if type(ids) ~= "table" then
        return set
    end
    for _, id in ipairs(ids) do
        if type(id) == "string" or type(id) == "number" then
            set[tostring(id)] = true
        end
    end
    return set
end

local function sendCustomSongRecords(guid, characterId, token, ids)
    local requested = requestedIdSet(ids)
    local customSongs = mp.getCharacterStorageForCharacter(characterId, STORAGE_NAMESPACE, "customSongs") or {}
    local sent = 0
    local encodedChunks = 0
    local skipped = 0

    for index, song in ipairs(type(customSongs) == "table" and customSongs or {}) do
        local encodedSong = encodeCustomSong(song)
        local id = encodedSong and encodedSong.id
        if id ~= nil and requested[tostring(id)] then
            local encoded = encodedSong.encoded or ""
            if #encoded > 0 and #encoded <= MAX_CUSTOM_SONG_ENCODED_CHARS then
                sent = sent + 1
                mp.send(guid, "BC_BardcraftCustomSongRecord", {
                    token = token,
                    index = sent,
                    id = encodedSong.id,
                    title = encodedSong.title,
                    desc = encodedSong.desc,
                    sourceFile = encodedSong.sourceFile,
                    texture = encodedSong.texture,
                    difficulty = encodedSong.difficulty,
                    parts = copyParts(encodedSong.parts),
                    encodedLength = #encoded,
                    revision = songRevision(encoded),
                    storageIndex = index,
                })

                for offset = 1, #encoded, CUSTOM_SONG_ENCODED_CHARS_PER_CHUNK do
                    mp.send(guid, "BC_BardcraftCustomSongRecordChunk", {
                        token = token,
                        index = sent,
                        text = encoded:sub(offset, offset + CUSTOM_SONG_ENCODED_CHARS_PER_CHUNK - 1),
                    })
                    encodedChunks = encodedChunks + 1
                end
            else
                skipped = skipped + 1
            end
        end
    end

    mp.send(guid, "BC_BardcraftCustomSongRecordsEnd", {
        token = token,
        sent = sent,
        skipped = skipped,
    })

    mp.log(string.format(
        "[bardcraft] custom song fetch guid=%s characterId=%s token=%s requested=%d sent=%d chunks=%d skipped=%d",
        tostring(guid),
        tostring(characterId),
        tostring(token),
        tableCount(ids),
        sent,
        encodedChunks,
        skipped))
end

local function upsertCustomSongRecord(characterId, record)
    local encodedSong = encodeCustomSong(record)
    if not encodedSong or encodedSong.id == nil or type(encodedSong.encoded) ~= "string" then
        return false
    end
    if #encodedSong.encoded <= 0 or #encodedSong.encoded > MAX_CUSTOM_SONG_ENCODED_CHARS then
        return false
    end

    local customSongs = mp.getCharacterStorageForCharacter(characterId, STORAGE_NAMESPACE, "customSongs")
    if type(customSongs) ~= "table" then
        customSongs = {}
    end

    local nextRecord = {
        id = encodedSong.id,
        title = encodedSong.title,
        desc = encodedSong.desc,
        sourceFile = encodedSong.sourceFile,
        texture = encodedSong.texture,
        difficulty = encodedSong.difficulty,
        parts = copyParts(encodedSong.parts),
        encoded = encodedSong.encoded,
    }

    local updated = false
    for index, existing in ipairs(customSongs) do
        if (nextRecord.id and existing.id == nextRecord.id)
            or (nextRecord.sourceFile and existing.sourceFile == nextRecord.sourceFile)
            or (nextRecord.title and existing.title == nextRecord.title)
        then
            customSongs[index] = nextRecord
            updated = true
            break
        end
    end

    if not updated then
        if #customSongs >= MAX_CUSTOM_SONGS_PER_CHARACTER then
            return false
        end
        table.insert(customSongs, nextRecord)
    end

    return mp.setCharacterStorageForCharacter(characterId, STORAGE_NAMESPACE, "customSongs", customSongs)
end

local function sendBootstrap(guid, characterId)
    local payload = {
        loaded = true,
    }

    for _, key in ipairs(STORAGE_KEYS) do
        payload[key] = mp.getCharacterStorageForCharacter(characterId, STORAGE_NAMESPACE, key)
    end

    if type(payload.knownSongs) ~= "table"
        and type(payload.performerStats) == "table"
        and type(payload.performerStats.knownSongs) == "table"
    then
        payload.knownSongs = payload.performerStats.knownSongs
        mp.setCharacterStorageForCharacter(characterId, STORAGE_NAMESPACE, "knownSongs", payload.knownSongs)
    end

    local knownSongs = type(payload.knownSongs) == "table" and payload.knownSongs or {}
    local customSongs = type(payload.customSongs) == "table" and payload.customSongs or {}
    local customSongManifest, customSongSkipped = makeCustomSongManifest(customSongs)
    payload.performerStats = copyPerformerStatsWithoutKnownSongs(payload.performerStats)
    payload.knownSongs = nil
    payload.customSongs = nil
    payload.customSongManifest = customSongManifest
    payload.customSongPayload = "manifest"
    payload.chunked = true
    payload.token = nextBootstrapToken(characterId)
    payload.knownSongCount = directKnownSongCount(knownSongs)
    payload.customSongCount = tableCount(customSongManifest)

    mp.log(string.format(
        "[bardcraft] bootstrap guid=%s characterId=%s token=%s knownSongs=%d troupeRecords=%d customSongs=%d customSkipped=%d",
        tostring(guid),
        tostring(characterId),
        tostring(payload.token),
        payload.knownSongCount,
        tableCount(payload.troupeRecords),
        payload.customSongCount,
        customSongSkipped))

    mp.send(guid, "BC_BardcraftPersistence", payload)
    local knownSent = sendKnownSongChunks(guid, payload.token, knownSongs)
    mp.send(guid, "BC_BardcraftPersistenceEnd", {
        token = payload.token,
    })

    mp.log(string.format(
        "[bardcraft] bootstrap chunks guid=%s characterId=%s token=%s knownSent=%d customManifest=%d customEncodedChunks=0",
        tostring(guid),
        tostring(characterId),
        tostring(payload.token),
        knownSent,
        payload.customSongCount))
end

local function persistSubmittedState(guid, characterId, data)
    local changed = false
    local customSongRecordUpserted = false
    if type(data.customSongRecord) == "table" then
        customSongRecordUpserted = upsertCustomSongRecord(characterId, data.customSongRecord)
        if not customSongRecordUpserted then
            mp.log(string.format(
                "[bardcraft] custom song upsert failed guid=%s characterId=%s id=%s",
                tostring(guid),
                tostring(characterId),
                tostring(data.customSongRecord.id)))
        end
        changed = customSongRecordUpserted or changed
    end

    for _, key in ipairs(STORAGE_KEYS) do
        local value = data[key]
        if key == "knownSongs" and value == nil then
            value = knownSongsFromState(data)
        elseif key == "performerStats" then
            value = copyPerformerStatsWithoutKnownSongs(value)
        end

        if key == "knownSongs" and type(value) == "table" and data.allowKnownSongsShrink ~= true then
            local existing = mp.getCharacterStorageForCharacter(characterId, STORAGE_NAMESPACE, "knownSongs")
            local incomingCount = tableCount(value)
            local existingCount = tableCount(existing)
            if type(existing) == "table" and incomingCount < existingCount then
                mp.log(string.format(
                    "[bardcraft] preserved knownSongs against shrink guid=%s characterId=%s incoming=%d existing=%d",
                    tostring(guid),
                    tostring(characterId),
                    incomingCount,
                    existingCount))
                value = nil
            end
        end

        if key == "customSongs" and type(value) == "table" and data.allowCustomSongsShrink ~= true then
            local existing = mp.getCharacterStorageForCharacter(characterId, STORAGE_NAMESPACE, "customSongs")
            local incomingCount = tableCount(value)
            local existingCount = tableCount(existing)
            if type(existing) == "table" and incomingCount < existingCount then
                mp.log(string.format(
                    "[bardcraft] preserved customSongs against shrink guid=%s characterId=%s incoming=%d existing=%d",
                    tostring(guid),
                    tostring(characterId),
                    incomingCount,
                    existingCount))
                value = nil
            end
        end

        if value ~= nil then
            local ok = mp.setCharacterStorageForCharacter(characterId, STORAGE_NAMESPACE, key, value)
            if not ok then
                mp.log(string.format(
                    "[bardcraft] save failed guid=%s characterId=%s key=%s",
                    tostring(guid),
                    tostring(characterId),
                    tostring(key)))
            end
            changed = ok or changed
        end
    end

    mp.log(string.format(
        "[bardcraft] save guid=%s characterId=%s changed=%s knownSongs=%d troupeRecords=%d customSongs=%d customRecord=%s",
        tostring(guid),
        tostring(characterId),
        tostring(changed),
        directKnownSongCount(knownSongsFromState(data)),
        tableCount(data.troupeRecords),
        tableCount(data.customSongs),
        tostring(customSongRecordUpserted)))

    return changed
end

M.eventHandlers = {
    BC_RequestBardcraftPersistence = function(data)
        local guid = senderGuid(data)
        local characterId = senderCharacterId(data)
        if not guid or not characterId then
            mp.log(string.format(
                "[bardcraft] bootstrap skipped missing sender guid=%s characterId=%s",
                tostring(guid),
                tostring(characterId)))
            return
        end

        sendBootstrap(guid, characterId)
    end,

    BC_RequestBardcraftServerSongs = function(data)
        local guid = senderGuid(data)
        if not guid then
            mp.log(string.format(
                "[bardcraft] hosted midi skipped missing sender guid=%s",
                tostring(data and data.pid)))
            return
        end

        sendHostedMidiCatalog(guid)
    end,

    BC_RequestBardcraftServerSongFiles = function(data)
        local guid = senderGuid(data)
        if not guid then
            mp.log(string.format(
                "[bardcraft] hosted midi files skipped missing sender guid=%s",
                tostring(data and data.pid)))
            return
        end

        sendHostedMidiFiles(guid, data and data.names or {})
    end,

    BC_RequestBardcraftCustomSongRecords = function(data)
        local guid = senderGuid(data)
        local characterId = senderCharacterId(data)
        if not guid or not characterId then
            mp.log(string.format(
                "[bardcraft] custom song fetch skipped missing sender guid=%s characterId=%s",
                tostring(guid),
                tostring(characterId)))
            return
        end

        sendCustomSongRecords(guid, characterId, data and data.token or "", data and data.ids or {})
    end,

    BC_SaveBardcraftPersistence = function(data)
        local guid = senderGuid(data)
        local characterId = senderCharacterId(data)
        if not guid or not characterId then
            mp.log(string.format(
                "[bardcraft] save skipped missing sender guid=%s characterId=%s",
                tostring(guid),
                tostring(characterId)))
            return
        end

        persistSubmittedState(guid, characterId, data or {})
    end,

    BC_BardcraftPersistenceAck = function(data)
        local guid = senderGuid(data)
        local characterId = senderCharacterId(data)
        mp.log(string.format(
            "[bardcraft] bootstrap ack guid=%s characterId=%s token=%s knownSongs=%d customSongs=%d",
            tostring(guid),
            tostring(characterId),
            tostring(data and data.token),
            tonumber(data and data.knownSongs) or 0,
            tonumber(data and data.customSongs) or 0))
    end,
}

return M
