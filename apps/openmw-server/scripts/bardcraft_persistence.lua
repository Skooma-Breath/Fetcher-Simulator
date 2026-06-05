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
local CUSTOM_SONG_SEGMENT_CHARS_PER_CHUNK = 8192
local MAX_CUSTOM_SONGS_PER_CHARACTER = 256
local MAX_CUSTOM_SONG_ENCODED_CHARS = 1024 * 1024
local MAX_CUSTOM_SONG_SEGMENT_CHARS = 1024 * 1024
local bootstrapSequence = 0

local function senderGuid(data)
    local guid = data and tonumber(data.pid) or 0
    return guid and guid > 0 and guid or nil
end

local function senderCharacterId(data)
    local characterId = data and tonumber(data.characterId) or 0
    return characterId and characterId > 0 and characterId or nil
end

local function playerCellKey(player)
    if not player or player.cell == nil then
        return nil
    end
    return tostring(player.cell)
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

local function copyRelayValue(value, depth)
    local valueType = type(value)
    if valueType == "nil" or valueType == "string" or valueType == "number" or valueType == "boolean" then
        return value
    end
    if valueType ~= "table" or (depth or 0) > 8 then
        return nil
    end

    local copy = {}
    for key, entry in pairs(value) do
        local copiedKey = copyRelayValue(key, (depth or 0) + 1)
        local copiedEntry = copyRelayValue(entry, (depth or 0) + 1)
        if copiedKey ~= nil and copiedEntry ~= nil then
            copy[copiedKey] = copiedEntry
        end
    end
    return copy
end

local function relayField(event, key)
    return event and event[key] or nil
end

local function flattenBardcraftPerformancePayload(guid, source, data, event)
    local song = type(event.song) == "table" and event.song or {}
    local part = type(event.part) == "table" and event.part or {}
    local timeSig = type(song.timeSig) == "table" and song.timeSig or {}

    return {
        sourceGuid = guid,
        sourceName = source and source.name or nil,
        eventType = event.type,
        time = relayField(event, "time"),
        completion = relayField(event, "completion"),
        note = relayField(event, "note"),
        id = relayField(event, "id"),
        velocity = relayField(event, "velocity"),
        stopSound = relayField(event, "stopSound"),
        bpm = relayField(event, "bpm"),
        bar = relayField(event, "bar"),
        instrument = relayField(event, "instrument"),
        item = relayField(event, "item"),
        songTitle = song.title,
        songTempo = song.tempo,
        songTempoMod = song.tempoMod,
        songTimeSigNum = timeSig[1],
        songTimeSigDen = timeSig[2],
        partInstrument = part.instrument,
    }
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

local function relayBardcraftPerformanceEvent(guid, data)
    local source = guid and mp.getPlayer(guid) or nil
    if not source or type(data) ~= "table" or type(data.event) ~= "table" then
        mp.log(string.format(
            "[bardcraft] performance relay rejected guid=%s hasSource=%s dataType=%s eventType=%s",
            tostring(guid),
            tostring(source ~= nil),
            type(data),
            type(data and data.event)))
        return
    end

    local eventType = data.event.type
    if type(eventType) ~= "string" then
        mp.log(string.format(
            "[bardcraft] performance relay rejected guid=%s name=%s actorId=%s eventTypeType=%s",
            tostring(guid),
            tostring(source.name),
            tostring(data.actorId),
            type(eventType)))
        return
    end

    local sourceCell = playerCellKey(source)
    local relayEvent = copyRelayValue(data.event, 0)
    if type(relayEvent) ~= "table" or type(relayEvent.type) ~= "string" then
        mp.log(string.format(
            "[bardcraft] performance relay rejected guid=%s name=%s actorId=%s invalid copied event",
            tostring(guid),
            tostring(source.name),
            tostring(data.actorId)))
        return
    end

    local payload = flattenBardcraftPerformancePayload(guid, source, data, relayEvent)

    if eventType == "PerformStart" or eventType == "PerformStop" then
        mp.log(string.format(
            "[bardcraft] performance relay received guid=%s name=%s cell=%s actorId=%s event=%s song=%s",
            tostring(guid),
            tostring(source.name),
            tostring(sourceCell),
            tostring(data.actorId),
            tostring(eventType),
            tostring(relayEvent.song and relayEvent.song.title)))
    end

    local sent = 0
    for _, target in ipairs(mp.getPlayers()) do
        local targetGuid = tonumber(target.guid)
        if targetGuid and targetGuid ~= tonumber(guid) and playerCellKey(target) == sourceCell then
            mp.send(targetGuid, "BCPerfRelay", payload)
            sent = sent + 1
        end
    end

    if eventType == "PerformStart" or eventType == "PerformStop" then
        mp.log(string.format(
            "[bardcraft] performance relay guid=%s name=%s cell=%s event=%s sent=%d song=%s",
            tostring(guid),
            tostring(source.name),
            tostring(sourceCell),
            tostring(eventType),
            sent,
            tostring(relayEvent.song and relayEvent.song.title)))
        mp.send(tonumber(guid), "BC_BardcraftPerformanceRelayAck", {
            eventType = eventType,
            sent = sent,
            song = relayEvent.song and relayEvent.song.title,
        })
    end
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

local copySegments
local segmentRevision
local makeNoteSegments

local function encodeCustomSong(song)
    if type(song) ~= "table" then
        return nil
    end

    local existingSegments = copySegments and copySegments(song.noteSegments) or {}
    if #existingSegments > 0 then
        local revision, noteCount, byteCount = segmentRevision(existingSegments)
        return {
            encoded = type(song.encoded) == "string" and song.encoded or nil,
            id = song.id,
            title = song.title,
            desc = song.desc,
            sourceFile = song.sourceFile,
            texture = song.texture,
            difficulty = song.difficulty,
            tempo = song.tempo,
            tempoEvents = song.tempoEvents,
            tempoMod = song.tempoMod,
            timeSig = song.timeSig,
            scale = song.scale,
            lengthBars = song.lengthBars,
            loopBars = song.loopBars,
            loopTimes = song.loopTimes,
            resolution = song.resolution,
            lyrics = song.lyrics,
            parts = song.parts,
            noteSegments = existingSegments,
            segmentTickSize = song.segmentTickSize,
            segmentRevision = song.segmentRevision or revision,
            segmentNoteCount = song.segmentNoteCount or noteCount,
            segmentBytes = song.segmentBytes or byteCount,
            isServerCustom = song.isServerCustom == true,
            serverHosted = song.serverHosted == true,
        }
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
            tempo = song.tempo,
            tempoEvents = song.tempoEvents,
            tempoMod = song.tempoMod,
            timeSig = song.timeSig,
            scale = song.scale,
            lengthBars = song.lengthBars,
            loopBars = song.loopBars,
            loopTimes = song.loopTimes,
            resolution = song.resolution,
            lyrics = song.lyrics,
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

    if #noteStrings == 0 then
        return nil
    end

    local noteSegments, segmentRevisionValue, segmentNoteCount, segmentBytes, segmentTickSize = makeNoteSegments(song)
    local songData = {
        id = song.id or song.sourceFile or song.title or "custom",
        title = song.title or song.id or song.sourceFile or "Custom Song",
        desc = song.desc or "",
        tempo = tonumber(song.tempo) or 120,
        tempoEvents = song.tempoEvents,
        tempoMod = song.tempoMod,
        timeSig = type(song.timeSig) == "table" and song.timeSig or { 4, 4 },
        scale = song.scale,
        lengthBars = tonumber(song.lengthBars) or 4,
        loopBars = type(song.loopBars) == "table" and song.loopBars or { 0, tonumber(song.lengthBars) or 4 },
        loopTimes = song.loopTimes,
        resolution = tonumber(song.resolution) or 96,
        lyrics = song.lyrics,
        notes = table.concat(noteStrings, ","),
        parts = type(song.parts) == "table" and song.parts or {},
    }

    return {
        encoded = #noteSegments == 0 and toBase64(jsonEncode(songData)) or nil,
        id = songData.id,
        title = songData.title,
        desc = songData.desc,
        sourceFile = song.sourceFile,
        texture = song.texture,
        difficulty = song.difficulty,
        parts = songData.parts,
        tempo = songData.tempo,
        tempoEvents = songData.tempoEvents,
        tempoMod = songData.tempoMod,
        timeSig = songData.timeSig,
        scale = songData.scale,
        lengthBars = songData.lengthBars,
        loopBars = songData.loopBars,
        loopTimes = songData.loopTimes,
        resolution = songData.resolution,
        lyrics = songData.lyrics,
        noteSegments = #noteSegments > 0 and noteSegments or nil,
        segmentTickSize = #noteSegments > 0 and segmentTickSize or nil,
        segmentRevision = #noteSegments > 0 and segmentRevisionValue or nil,
        segmentNoteCount = #noteSegments > 0 and segmentNoteCount or nil,
        segmentBytes = #noteSegments > 0 and segmentBytes or nil,
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

copySegments = function(segments)
    local copy = {}
    if type(segments) ~= "table" then
        return copy
    end

    for _, segment in ipairs(segments) do
        if type(segment) == "table" and type(segment.notes) == "string" then
            table.insert(copy, {
                index = segment.index,
                startTick = segment.startTick,
                endTick = segment.endTick,
                noteCount = segment.noteCount,
                notes = segment.notes,
            })
        end
    end
    return copy
end

local function countNotesInSegment(segment)
    local notes = type(segment) == "table" and segment.notes or ""
    local count = 0
    for _ in tostring(notes):gmatch("([^,]+)") do
        count = count + 1
    end
    return count
end

segmentRevision = function(segments)
    local hash = 0
    local bytes = 0
    local notes = 0
    for _, segment in ipairs(segments or {}) do
        local text = tostring(segment.notes or "")
        bytes = bytes + #text
        notes = notes + (tonumber(segment.noteCount) or countNotesInSegment(segment))
        for index = 1, #text do
            hash = (hash * 131 + text:byte(index)) % 2147483647
        end
    end
    return tostring(notes) .. ":" .. tostring(bytes) .. ":" .. tostring(hash), notes, bytes
end

local function noteEventToString(note)
    return string.format(
        "%d|%s|%d|%d|%d|%d",
        tonumber(note.id) or 0,
        tostring(note.type or ""),
        tonumber(note.note) or 0,
        tonumber(note.velocity) or 0,
        tonumber(note.part) or 0,
        tonumber(note.time) or 0)
end

makeNoteSegments = function(song)
    if type(song) ~= "table" or type(song.notes) ~= "table" then
        return {}, nil, 0, 0, 0
    end

    local resolution = tonumber(song.resolution) or 96
    local timeSig = type(song.timeSig) == "table" and song.timeSig or { 4, 4 }
    local ticksPerBar = resolution * 4 * ((tonumber(timeSig[1]) or 4) / (tonumber(timeSig[2]) or 4))
    local segmentTickSize = math.max(1, math.floor(ticksPerBar * 4))
    local buckets = {}
    local orderedNotes = {}

    for _, note in ipairs(song.notes) do
        table.insert(orderedNotes, note)
    end
    table.sort(orderedNotes, function(left, right)
        local leftTime = tonumber(left.time) or 0
        local rightTime = tonumber(right.time) or 0
        if leftTime ~= rightTime then
            return leftTime < rightTime
        end
        return (tonumber(left.id) or 0) < (tonumber(right.id) or 0)
    end)

    for _, note in ipairs(orderedNotes) do
        local noteTime = math.max(0, tonumber(note.time) or 0)
        local index = math.floor(noteTime / segmentTickSize) + 1
        local bucket = buckets[index]
        if not bucket then
            bucket = {
                index = index,
                startTick = (index - 1) * segmentTickSize,
                endTick = index * segmentTickSize - 1,
                noteStrings = {},
                noteCount = 0,
            }
            buckets[index] = bucket
        end
        table.insert(bucket.noteStrings, noteEventToString(note))
        bucket.noteCount = bucket.noteCount + 1
    end

    local segments = {}
    local indexes = {}
    for index, _ in pairs(buckets) do
        table.insert(indexes, index)
    end
    table.sort(indexes)
    for _, index in ipairs(indexes) do
        local bucket = buckets[index]
        table.insert(segments, {
            index = bucket.index,
            startTick = bucket.startTick,
            endTick = bucket.endTick,
            noteCount = bucket.noteCount,
            notes = table.concat(bucket.noteStrings, ","),
        })
    end

    local revision, noteCount, byteCount = segmentRevision(segments)
    return segments, revision, noteCount, byteCount, segmentTickSize
end

local function makeCustomSongManifestEntry(song, storageIndex)
    local encodedSong = encodeCustomSong(song)
    if not encodedSong then
        return nil
    end

    local encodedLength = type(encodedSong.encoded) == "string" and #encodedSong.encoded or 0
    local hasEncoded = encodedLength > 0
    local hasSegments = type(encodedSong.noteSegments) == "table" and #encodedSong.noteSegments > 0
    if (hasEncoded and encodedLength > MAX_CUSTOM_SONG_ENCODED_CHARS)
        or (hasSegments and (tonumber(encodedSong.segmentBytes) or 0) > MAX_CUSTOM_SONG_SEGMENT_CHARS)
        or (not hasEncoded and not hasSegments)
    then
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
        tempo = encodedSong.tempo,
        tempoEvents = encodedSong.tempoEvents,
        tempoMod = encodedSong.tempoMod,
        timeSig = encodedSong.timeSig,
        scale = encodedSong.scale,
        lengthBars = encodedSong.lengthBars,
        loopBars = encodedSong.loopBars,
        loopTimes = encodedSong.loopTimes,
        resolution = encodedSong.resolution,
        lyrics = encodedSong.lyrics,
        encodedLength = hasEncoded and encodedLength or nil,
        revision = hasEncoded and songRevision(encodedSong.encoded) or nil,
        segmentCount = hasSegments and #encodedSong.noteSegments or nil,
        segmentTickSize = encodedSong.segmentTickSize,
        segmentRevision = encodedSong.segmentRevision,
        segmentNoteCount = encodedSong.segmentNoteCount,
        segmentBytes = encodedSong.segmentBytes,
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
    local segmentChunks = 0
    local skipped = 0

    for index, song in ipairs(type(customSongs) == "table" and customSongs or {}) do
        local encodedSong = encodeCustomSong(song)
        local id = encodedSong and encodedSong.id
        if id ~= nil and requested[tostring(id)] then
            local encoded = encodedSong.encoded or ""
            local segments = copySegments(encodedSong.noteSegments)
            local hasSegments = #segments > 0
            local hasEncoded = #encoded > 0 and #encoded <= MAX_CUSTOM_SONG_ENCODED_CHARS
            local segmentBytes = tonumber(encodedSong.segmentBytes) or 0
            if (hasSegments and segmentBytes <= MAX_CUSTOM_SONG_SEGMENT_CHARS) or hasEncoded then
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
                    tempo = encodedSong.tempo,
                    tempoEvents = encodedSong.tempoEvents,
                    tempoMod = encodedSong.tempoMod,
                    timeSig = encodedSong.timeSig,
                    scale = encodedSong.scale,
                    lengthBars = encodedSong.lengthBars,
                    loopBars = encodedSong.loopBars,
                    loopTimes = encodedSong.loopTimes,
                    resolution = encodedSong.resolution,
                    lyrics = encodedSong.lyrics,
                    encodedLength = hasEncoded and #encoded or 0,
                    revision = hasEncoded and songRevision(encoded) or nil,
                    segmentCount = hasSegments and #segments or 0,
                    segmentTickSize = encodedSong.segmentTickSize,
                    segmentRevision = encodedSong.segmentRevision,
                    segmentNoteCount = encodedSong.segmentNoteCount,
                    segmentBytes = encodedSong.segmentBytes,
                    storageIndex = index,
                })

                if hasSegments then
                    for segmentIndex, segment in ipairs(segments) do
                        local text = segment.notes or ""
                        local partCount = math.max(1, math.ceil(#text / CUSTOM_SONG_SEGMENT_CHARS_PER_CHUNK))
                        for offset = 1, #text, CUSTOM_SONG_SEGMENT_CHARS_PER_CHUNK do
                            local partIndex = math.floor((offset - 1) / CUSTOM_SONG_SEGMENT_CHARS_PER_CHUNK) + 1
                            mp.send(guid, "BC_BardcraftCustomSongSegment", {
                                token = token,
                                index = sent,
                                segmentIndex = segmentIndex,
                                startTick = segment.startTick,
                                endTick = segment.endTick,
                                noteCount = segment.noteCount,
                                partIndex = partIndex,
                                partCount = partCount,
                                text = text:sub(offset, offset + CUSTOM_SONG_SEGMENT_CHARS_PER_CHUNK - 1),
                            })
                            segmentChunks = segmentChunks + 1
                        end
                        if #text == 0 then
                            mp.send(guid, "BC_BardcraftCustomSongSegment", {
                                token = token,
                                index = sent,
                                segmentIndex = segmentIndex,
                                startTick = segment.startTick,
                                endTick = segment.endTick,
                                noteCount = segment.noteCount,
                                partIndex = 1,
                                partCount = 1,
                                text = "",
                            })
                            segmentChunks = segmentChunks + 1
                        end
                    end
                elseif hasEncoded then
                    for offset = 1, #encoded, CUSTOM_SONG_ENCODED_CHARS_PER_CHUNK do
                        mp.send(guid, "BC_BardcraftCustomSongRecordChunk", {
                            token = token,
                            index = sent,
                            text = encoded:sub(offset, offset + CUSTOM_SONG_ENCODED_CHARS_PER_CHUNK - 1),
                        })
                        encodedChunks = encodedChunks + 1
                    end
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
        "[bardcraft] custom song fetch guid=%s characterId=%s token=%s requested=%d sent=%d encodedChunks=%d segmentChunks=%d skipped=%d",
        tostring(guid),
        tostring(characterId),
        tostring(token),
        tableCount(ids),
        sent,
        encodedChunks,
        segmentChunks,
        skipped))
end

local function upsertCustomSongRecord(characterId, record)
    local encodedSong = encodeCustomSong(record)
    if not encodedSong or encodedSong.id == nil then
        return false
    end

    local encodedLength = type(encodedSong.encoded) == "string" and #encodedSong.encoded or 0
    local segments = copySegments(encodedSong.noteSegments)
    local segmentBytes = tonumber(encodedSong.segmentBytes) or 0
    if (encodedLength > MAX_CUSTOM_SONG_ENCODED_CHARS)
        or (segmentBytes > MAX_CUSTOM_SONG_SEGMENT_CHARS)
        or (encodedLength <= 0 and #segments == 0)
    then
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
        tempo = encodedSong.tempo,
        tempoEvents = encodedSong.tempoEvents,
        tempoMod = encodedSong.tempoMod,
        timeSig = encodedSong.timeSig,
        scale = encodedSong.scale,
        lengthBars = encodedSong.lengthBars,
        loopBars = encodedSong.loopBars,
        loopTimes = encodedSong.loopTimes,
        resolution = encodedSong.resolution,
        lyrics = encodedSong.lyrics,
        parts = copyParts(encodedSong.parts),
        encoded = encodedLength > 0 and encodedSong.encoded or nil,
        noteSegments = #segments > 0 and segments or nil,
        segmentTickSize = encodedSong.segmentTickSize,
        segmentRevision = encodedSong.segmentRevision,
        segmentNoteCount = encodedSong.segmentNoteCount,
        segmentBytes = encodedSong.segmentBytes,
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

    BC_BardcraftPerformanceRelayWake = function(data)
        local guid = senderGuid(data)
        if guid then
            local source = mp.getPlayer(guid)
            mp.log(string.format(
                "[bardcraft] performance relay wake guid=%s name=%s eventType=%s",
                tostring(guid),
                tostring(source and source.name),
                tostring(data and data.eventType)))
        end
    end,

    BCPerfPing = function(data)
        local guid = senderGuid(data)
        if guid then
            mp.send(tonumber(guid), "BCPerfPong", {
                ok = true,
                ts = data and data.ts,
            })
        end
    end,

    BC_BardcraftPerformanceRelay = function(data)
        local guid = senderGuid(data)
        if not guid then
            return
        end

        relayBardcraftPerformanceEvent(guid, data)
    end,
}

return M
