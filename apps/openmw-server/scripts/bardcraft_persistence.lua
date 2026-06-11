local mp = require("mp")

local M = {
    interfaceName = "BardcraftPersistence",
}

local STORAGE_NAMESPACE = "Bardcraft"
local STORAGE_KEYS = {
    "performerStats",
    "knownSongs",
    "troupeRecords",
    "customSongs",
}
local RUNTIME_NAMESPACE = "BardcraftRuntime"
local RUNTIME_ACTIVE_SESSIONS_KEY = "activePerformanceSessions"
local RUNTIME_ACTIVE_BANDS_KEY = "activeBands"
local RUNTIME_PENDING_BAND_STARTS_KEY = "pendingBandStarts"

local MAX_HOSTED_MIDI_BYTES_PER_RESPONSE = 512 * 1024
local KNOWN_SONGS_PER_BOOTSTRAP_CHUNK = 40
local CUSTOM_SONG_ENCODED_CHARS_PER_CHUNK = 8192
local CUSTOM_SONG_SEGMENT_CHARS_PER_CHUNK = 8192
local MAX_CUSTOM_SONGS_PER_CHARACTER = 256
local MAX_CUSTOM_SONG_ENCODED_CHARS = 1024 * 1024
local MAX_CUSTOM_SONG_SEGMENT_CHARS = 1024 * 1024
local bootstrapSequence = 0
local performanceSessionSequence = 0
local joinRequestSequence = 0
local bandStartSequence = 0
local pendingJoinRequests = {}
local activePerformanceSessions = {}
local activeBandsByLeaderGuid = {}
local bandLeaderByMemberGuid = {}
local pendingBandInvitesByMemberGuid = {}
local pendingScheduledBandStartsByLeaderGuid = {}
local BAND_INVITE_TTL_SECONDS = 300
local BAND_SCHEDULED_START_LEAD_SECONDS = 3.0
local highChurnPerformanceEvents = {
    NoteEvent = true,
    NoteEndEvent = true,
    TempoEvent = true,
    NewBar = true,
}

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

local function serverUptime()
    if type(mp.getUptime) == "function" then
        return tonumber(mp.getUptime()) or 0
    end
    return 0
end

local function performanceSessionKey(guid, actorId)
    return tostring(guid) .. ":" .. tostring(actorId or "")
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

local function trim(value)
    return tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", "")
end

local function normalizeLookup(value)
    value = trim(value)
    if value == "" then
        return ""
    end
    return string.lower(value)
end

local function playerDisplayName(player)
    if not player then
        return "unknown"
    end
    return tostring(player.name or ("guid " .. tostring(player.guid)))
end

local function findOnlinePlayer(selector)
    selector = trim(selector)
    if selector == "" then
        return nil, "missing-selector"
    end

    local numericGuid = tonumber(selector)
    if numericGuid then
        local player = mp.getPlayer(numericGuid)
        if player then
            return player
        end
    end

    local wanted = normalizeLookup(selector)
    local exact = nil
    local partialMatches = {}
    for _, player in ipairs(mp.getPlayers()) do
        local name = normalizeLookup(player and player.name)
        if name == wanted then
            exact = player
            break
        end
        if wanted ~= "" and string.find(name, wanted, 1, true) then
            table.insert(partialMatches, player)
        end
    end

    if exact then
        return exact
    end
    if #partialMatches == 1 then
        return partialMatches[1]
    end
    if #partialMatches > 1 then
        return nil, "ambiguous"
    end
    return nil, "not-found"
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

local function runtimeSection()
    return mp.storage.globalSection(RUNTIME_NAMESPACE)
end

local function clearActivePerformanceSessionSnapshot()
    runtimeSection():set(RUNTIME_ACTIVE_SESSIONS_KEY, {})
end

local function clearActiveBandSnapshot()
    runtimeSection():set(RUNTIME_ACTIVE_BANDS_KEY, {})
end

local function clearPendingScheduledBandStartSnapshot()
    runtimeSection():set(RUNTIME_PENDING_BAND_STARTS_KEY, {})
end

local function writePendingScheduledBandStartSnapshot()
    local snapshot = {}
    for leaderGuid, pending in pairs(pendingScheduledBandStartsByLeaderGuid) do
        if type(pending) == "table" then
            snapshot[tostring(leaderGuid)] = {
                bandSessionId = pending.bandSessionId,
                startServerTime = pending.startServerTime,
                expiresAt = pending.expiresAt,
                selector = pending.selector,
                role = pending.role,
            }
        end
    end
    runtimeSection():set(RUNTIME_PENDING_BAND_STARTS_KEY, snapshot)
end

local function loadPendingScheduledBandStartSnapshot(reason)
    local snapshot = runtimeSection():getCopy(RUNTIME_PENDING_BAND_STARTS_KEY) or {}
    if type(snapshot) ~= "table" then
        return 0
    end

    local loaded = 0
    for leaderGuid, pending in pairs(snapshot) do
        local numericLeaderGuid = tonumber(leaderGuid)
        if numericLeaderGuid and type(pending) == "table" then
            pendingScheduledBandStartsByLeaderGuid[numericLeaderGuid] = {
                bandSessionId = pending.bandSessionId,
                startServerTime = tonumber(pending.startServerTime),
                expiresAt = tonumber(pending.expiresAt),
                selector = pending.selector,
                role = pending.role,
            }
            loaded = loaded + 1
        end
    end
    if loaded > 0 then
        mp.log(string.format(
            "[bardcraft] pending band start snapshot loaded reason=%s starts=%d",
            tostring(reason),
            loaded))
    end
    return loaded
end

local function writeActivePerformanceSessionSnapshot()
    local snapshot = {}
    for key, session in pairs(activePerformanceSessions) do
        snapshot[key] = {
            key = session.key,
            sourceGuid = session.sourceGuid,
            sourceCharacterId = session.sourceCharacterId,
            sourceName = session.sourceName,
            actorId = session.actorId,
            cell = session.cell,
            startMusicTime = session.startMusicTime,
            serverStartTime = session.serverStartTime,
            sessionSequence = session.sessionSequence,
            payload = copyRelayValue(session.payload, 0),
        }
    end
    runtimeSection():set(RUNTIME_ACTIVE_SESSIONS_KEY, snapshot)
end

local function writeActiveBandSnapshot()
    local snapshot = {}
    for leaderGuid, band in pairs(activeBandsByLeaderGuid) do
        local storedMembers = {}
        for memberGuid, member in pairs(band.members or {}) do
            table.insert(storedMembers, {
                guid = tonumber(memberGuid),
                name = member and member.name or nil,
                invitedAt = member and member.invitedAt or nil,
                acceptedAt = member and member.acceptedAt or nil,
            })
        end
        table.sort(storedMembers, function(left, right)
            return tostring(left.guid) < tostring(right.guid)
        end)
        table.insert(snapshot, {
            leaderGuid = tonumber(leaderGuid),
            leaderName = band.leaderName,
            createdAt = band.createdAt,
            members = storedMembers,
        })
    end
    runtimeSection():set(RUNTIME_ACTIVE_BANDS_KEY, snapshot)
end

local function loadActiveBandSnapshot(reason)
    local snapshot = runtimeSection():getCopy(RUNTIME_ACTIVE_BANDS_KEY) or {}
    if type(snapshot) ~= "table" then
        return 0
    end

    local loaded = 0
    for _, storedBand in pairs(snapshot) do
        if type(storedBand) == "table" then
            local leaderGuid = tonumber(storedBand.leaderGuid)
            if leaderGuid then
                local band = activeBandsByLeaderGuid[leaderGuid]
                if not band then
                    band = {
                        leaderGuid = leaderGuid,
                        leaderName = storedBand.leaderName,
                        members = {},
                        createdAt = tonumber(storedBand.createdAt) or serverUptime(),
                    }
                    activeBandsByLeaderGuid[leaderGuid] = band
                    loaded = loaded + 1
                else
                    band.leaderName = storedBand.leaderName or band.leaderName
                end

                for memberKey, storedMember in pairs(storedBand.members or {}) do
                    if type(storedMember) == "table" then
                        local memberGuid = tonumber(storedMember.guid or memberKey)
                        if memberGuid then
                            band.members[memberGuid] = {
                                guid = memberGuid,
                                name = storedMember.name,
                                invitedAt = storedMember.invitedAt,
                                acceptedAt = storedMember.acceptedAt,
                            }
                            bandLeaderByMemberGuid[memberGuid] = leaderGuid
                        end
                    end
                end
            end
        end
    end

    if loaded > 0 then
        mp.log(string.format(
            "[bardcraft] active band snapshot loaded reason=%s leaders=%d memberLinks=%d",
            tostring(reason),
            tableCount(activeBandsByLeaderGuid),
            tableCount(bandLeaderByMemberGuid)))
    end
    return loaded
end

local function activeBandForLeader(leaderGuid, reason)
    leaderGuid = tonumber(leaderGuid)
    if not leaderGuid then
        return nil
    end

    local band = activeBandsByLeaderGuid[leaderGuid]
    if band then
        return band
    end

    loadActiveBandSnapshot(reason)
    band = activeBandsByLeaderGuid[leaderGuid]
    if band then
        mp.log(string.format(
            "[bardcraft] active band recovered leader=%s reason=%s members=%d",
            tostring(leaderGuid),
            tostring(reason),
            tableCount(band.members)))
    end
    return band
end

local function activePerformanceSessionSource()
    if next(activePerformanceSessions) ~= nil then
        return activePerformanceSessions
    end

    local snapshot = runtimeSection():getCopy(RUNTIME_ACTIVE_SESSIONS_KEY) or {}
    if type(snapshot) ~= "table" then
        return {}
    end
    return snapshot
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
        actorId = data and data.actorId,
        sessionKey = relayField(event, "sessionKey"),
        sessionPlayback = relayField(event, "sessionPlayback"),
        serverTime = relayField(event, "serverTime"),
        serverStartTime = relayField(event, "serverStartTime"),
        sessionSequence = relayField(event, "sessionSequence"),
        joinedSessionKey = relayField(event, "joinedSessionKey"),
        bandSessionId = relayField(event, "bandSessionId"),
        scheduledBandStart = relayField(event, "scheduledBandStart") == true,
        joinStartMusicTime = relayField(event, "joinStartMusicTime"),
        joinStartServerTime = relayField(event, "joinStartServerTime"),
        perfType = relayField(event, "perfType"),
        songTitle = song.title,
        songId = song.id,
        songSourceFile = song.sourceFile,
        songTempo = song.tempo,
        songTempoMod = song.tempoMod,
        songTimeSigNum = timeSig[1],
        songTimeSigDen = timeSig[2],
        songLengthBars = song.lengthBars,
        songResolution = song.resolution,
        songSegmentRevision = song.segmentRevision,
        songIsMpCustom = song.isMpCustom == true,
        songIsServerCustom = song.isServerCustom == true,
        songServerHosted = song.serverHosted == true,
        partIndex = part.index,
        partInstrument = part.instrument,
        partTitle = part.title,
        partNumOfType = part.numOfType,
    }
end

local function updateActivePerformanceSession(guid, source, actorId, payload, relayEvent, sourceCharacterId)
    if type(payload) ~= "table" or type(relayEvent) ~= "table" then
        return nil
    end

    local now = serverUptime()
    local key = performanceSessionKey(guid, actorId)
    performanceSessionSequence = performanceSessionSequence + 1

    payload.sessionKey = key
    payload.sessionPlayback = true
    payload.serverTime = now
    payload.serverStartTime = now
    payload.sessionSequence = performanceSessionSequence
    payload.actorId = actorId

    activePerformanceSessions[key] = {
        key = key,
        sourceGuid = tonumber(guid),
        sourceCharacterId = tonumber(sourceCharacterId),
        sourceName = source and source.name or nil,
        actorId = actorId,
        cell = playerCellKey(source),
        parentSessionKey = payload.joinedSessionKey and tostring(payload.joinedSessionKey) or nil,
        bandSessionId = payload.bandSessionId and tostring(payload.bandSessionId) or nil,
        startMusicTime = tonumber(payload.time) or 0,
        serverStartTime = now,
        sessionSequence = performanceSessionSequence,
        payload = copyRelayValue(payload, 0),
        acked = {},
    }
    writeActivePerformanceSessionSnapshot()

    return activePerformanceSessions[key]
end

local function currentSessionPayload(session, now)
    if type(session) ~= "table" or type(session.payload) ~= "table" then
        return nil
    end

    now = tonumber(now) or serverUptime()
    local payload = copyRelayValue(session.payload, 0)
    payload.time = math.max(0, (tonumber(session.startMusicTime) or 0) + now - (tonumber(session.serverStartTime) or now))
    payload.serverTime = now
    payload.serverStartTime = session.serverStartTime
    payload.sessionKey = session.key
    payload.sessionPlayback = true
    payload.sessionSequence = session.sessionSequence
    payload.sourceGuid = session.sourceGuid
    payload.sourceName = session.sourceName
    payload.actorId = session.actorId
    payload.joinedSessionKey = session.parentSessionKey
    payload.eventType = "PerformStart"
    return payload
end

local function applyPendingScheduledBandStart(guid, session, payload)
    local leaderGuid = tonumber(guid)
    local pending = leaderGuid and pendingScheduledBandStartsByLeaderGuid[leaderGuid] or nil
    if not pending and leaderGuid then
        loadPendingScheduledBandStartSnapshot("relay-match")
        pending = pendingScheduledBandStartsByLeaderGuid[leaderGuid]
    end
    if type(pending) ~= "table" or type(session) ~= "table" or type(payload) ~= "table" then
        return false
    end

    local now = serverUptime()
    if now > (tonumber(pending.expiresAt) or 0) then
        pendingScheduledBandStartsByLeaderGuid[leaderGuid] = nil
        writePendingScheduledBandStartSnapshot()
        return false
    end

    payload.bandSessionId = pending.bandSessionId
    payload.scheduledBandStart = true
    payload.joinStartMusicTime = 0
    payload.joinStartServerTime = pending.startServerTime
    session.bandSessionId = pending.bandSessionId
    session.startMusicTime = 0
    session.serverStartTime = pending.startServerTime
    session.payload.bandSessionId = pending.bandSessionId
    session.payload.scheduledBandStart = true
    session.payload.joinStartMusicTime = 0
    session.payload.joinStartServerTime = pending.startServerTime
    session.payload.serverStartTime = pending.startServerTime
    pendingScheduledBandStartsByLeaderGuid[leaderGuid] = nil
    writePendingScheduledBandStartSnapshot()
    writeActivePerformanceSessionSnapshot()
    mp.log(string.format(
        "[bardcraft] band scheduled start matched leader=%s session=%s bandSession=%s startServerTime=%.3f now=%.3f",
        tostring(leaderGuid),
        tostring(session.key),
        tostring(pending.bandSessionId),
        tonumber(pending.startServerTime) or 0,
        now))
    return true
end

local function sessionSongIdentity(payload)
    if type(payload) ~= "table" then
        return nil
    end
    if payload.songId ~= nil then
        return "id:" .. tostring(payload.songId)
    end
    if payload.songSourceFile ~= nil then
        return "file:" .. tostring(payload.songSourceFile)
    end
    if payload.songTitle ~= nil then
        return "title:" .. tostring(payload.songTitle)
    end
    return nil
end

local function collectOccupiedPartIndices(joinSession, now)
    local occupied = {}
    local wantedPayload = currentSessionPayload(joinSession, now)
    local wantedSong = sessionSongIdentity(wantedPayload)
    if not wantedSong then
        return occupied
    end

    local seen = {}
    for _, session in pairs(activePerformanceSessionSource()) do
        if session.cell == joinSession.cell then
            local payload = currentSessionPayload(session, now)
            if sessionSongIdentity(payload) == wantedSong and payload.partIndex ~= nil then
                local part = tostring(payload.partIndex)
                if not seen[part] then
                    seen[part] = true
                    table.insert(occupied, payload.partIndex)
                end
            end
        end
    end
    table.sort(occupied, function(left, right)
        local leftNumber = tonumber(left)
        local rightNumber = tonumber(right)
        if leftNumber and rightNumber then
            return leftNumber < rightNumber
        end
        return tostring(left) < tostring(right)
    end)
    return occupied
end

local function activeSessionsForPlayer(player)
    local sessions = {}
    local playerGuid = player and tonumber(player.guid) or nil
    local targetCell = playerCellKey(player)
    for _, session in pairs(activePerformanceSessionSource()) do
        if tonumber(session.sourceGuid) ~= playerGuid and session.cell == targetCell and not session.parentSessionKey then
            table.insert(sessions, session)
        end
    end
    table.sort(sessions, function(left, right)
        local leftName = tostring(left.sourceName or "")
        local rightName = tostring(right.sourceName or "")
        if leftName ~= rightName then
            return leftName < rightName
        end
        return tostring(left.key) < tostring(right.key)
    end)
    return sessions
end

local function sendActivePerformanceSessions(guid, targetCellOverride)
    local target = guid and mp.getPlayer(guid) or nil
    if not target then
        return
    end

    local targetCell = targetCellOverride and tostring(targetCellOverride) or playerCellKey(target)
    local now = serverUptime()
    local sessions = {}
    for _, session in pairs(activePerformanceSessions) do
        if tonumber(session.sourceGuid) ~= tonumber(guid) and session.cell == targetCell then
            local payload = currentSessionPayload(session, now)
            if payload then
                table.insert(sessions, payload)
            end
        end
    end

    if #sessions > 0 then
        mp.send(tonumber(guid), "BC_BardcraftPerformanceSessions", {
            serverTime = now,
            cell = targetCell,
            sessions = sessions,
        })
    end

    mp.log(string.format(
        "[bardcraft] active performance sessions guid=%s cell=%s sent=%d",
        tostring(guid),
        tostring(targetCell),
        #sessions))
end

local function sendPerformanceSessionPayloadToCell(payload, cell, excludeGuid)
    if type(payload) ~= "table" or cell == nil then
        return 0
    end

    local sent = 0
    for _, target in ipairs(mp.getPlayers()) do
        local targetGuid = tonumber(target.guid)
        if targetGuid and targetGuid ~= tonumber(excludeGuid) and playerCellKey(target) == cell then
            mp.send(targetGuid, "BCPerfRelay", payload)
            sent = sent + 1
        end
    end
    return sent
end

local function makePerformanceSessionStopPayload(session, reason)
    if type(session) ~= "table" then
        return nil
    end

    return {
        sourceGuid = session.sourceGuid,
        sourceName = session.sourceName,
        actorId = session.actorId,
        sessionKey = session.key,
        sessionPlayback = true,
        sessionSequence = session.sessionSequence,
        eventType = "PerformStop",
        completion = 0,
        stopReason = reason,
        serverTime = serverUptime(),
    }
end

local function stopPerformanceSessionByKey(sessionKey, reason, notifySource)
    local session = sessionKey and activePerformanceSessions[tostring(sessionKey)] or nil
    if type(session) ~= "table" then
        return 0, 0, 0
    end

    local sourceGuid = tonumber(session.sourceGuid)
    local sourceNotified = 0
    if notifySource and sourceGuid and mp.getPlayer(sourceGuid) then
        mp.send(sourceGuid, "BC_BardcraftStopLocalPerformance", {
            reason = reason,
            sessionKey = session.key,
            parentSessionKey = session.parentSessionKey,
        })
        sourceNotified = 1
    end

    local sent = sendPerformanceSessionPayloadToCell(
        makePerformanceSessionStopPayload(session, reason),
        session.cell,
        sourceGuid)
    activePerformanceSessions[tostring(sessionKey)] = nil
    return 1, sent, sourceNotified
end

local function stopChildPerformanceSessions(parentSessionKey, reason)
    if not parentSessionKey then
        return 0
    end

    local childKeys = {}
    for key, session in pairs(activePerformanceSessions) do
        if tostring(session.parentSessionKey) == tostring(parentSessionKey) then
            table.insert(childKeys, key)
        end
    end

    local removed = 0
    local sent = 0
    local sourceNotified = 0
    for _, childKey in ipairs(childKeys) do
        local childRemoved, childSent, childSourceNotified = stopPerformanceSessionByKey(childKey, reason, true)
        removed = removed + childRemoved
        sent = sent + childSent
        sourceNotified = sourceNotified + childSourceNotified
    end

    if removed > 0 then
        writeActivePerformanceSessionSnapshot()
        mp.log(string.format(
            "[bardcraft] child performance sessions stopped parent=%s reason=%s sessions=%d sent=%d sourceNotified=%d",
            tostring(parentSessionKey),
            tostring(reason),
            removed,
            sent,
            sourceNotified))
    end

    return removed
end

local function stopBandChildSessionsForMember(leaderGuid, memberGuid, reason)
    leaderGuid = tonumber(leaderGuid)
    memberGuid = tonumber(memberGuid)
    if not leaderGuid or not memberGuid then
        return 0
    end

    local childKeys = {}
    for key, session in pairs(activePerformanceSessions) do
        local parent = session.parentSessionKey and activePerformanceSessions[tostring(session.parentSessionKey)] or nil
        if parent
            and tonumber(parent.sourceGuid) == leaderGuid
            and tonumber(session.sourceGuid) == memberGuid
        then
            table.insert(childKeys, key)
        end
    end

    local removed = 0
    local sent = 0
    local sourceNotified = 0
    for _, childKey in ipairs(childKeys) do
        local childRemoved, childSent, childSourceNotified = stopPerformanceSessionByKey(childKey, reason, true)
        removed = removed + childRemoved
        sent = sent + childSent
        sourceNotified = sourceNotified + childSourceNotified
    end

    if removed > 0 then
        writeActivePerformanceSessionSnapshot()
        mp.log(string.format(
            "[bardcraft] band child sessions stopped leader=%s member=%s reason=%s sessions=%d sent=%d sourceNotified=%d",
            tostring(leaderGuid),
            tostring(memberGuid),
            tostring(reason),
            removed,
            sent,
            sourceNotified))
    end

    return removed
end

local function removeBandMembership(memberGuid, reason)
    memberGuid = tonumber(memberGuid)
    local leaderGuid = memberGuid and bandLeaderByMemberGuid[memberGuid] or nil
    if not leaderGuid then
        return nil
    end

    local band = activeBandForLeader(leaderGuid, "remove-membership")
    if band and band.members then
        band.members[memberGuid] = nil
    end
    bandLeaderByMemberGuid[memberGuid] = nil
    stopBandChildSessionsForMember(leaderGuid, memberGuid, reason)
    writeActiveBandSnapshot()
    return leaderGuid
end

local function disbandBand(leaderGuid, reason, notifyPlayers)
    leaderGuid = tonumber(leaderGuid)
    local band = leaderGuid and activeBandForLeader(leaderGuid, "disband") or nil
    if not band then
        return 0
    end

    local removed = 0
    local memberGuids = {}
    for memberGuid, _ in pairs(band.members or {}) do
        table.insert(memberGuids, tonumber(memberGuid))
    end

    for _, memberGuid in ipairs(memberGuids) do
        bandLeaderByMemberGuid[memberGuid] = nil
        stopBandChildSessionsForMember(leaderGuid, memberGuid, reason)
        removed = removed + 1
        if notifyPlayers then
            local member = mp.getPlayer(memberGuid)
            if member then
                member:sendMessage("[Bardcraft] Your band has been disbanded.")
            end
        end
    end

    activeBandsByLeaderGuid[leaderGuid] = nil
    writeActiveBandSnapshot()
    mp.log(string.format(
        "[bardcraft] band disbanded leader=%s reason=%s members=%d",
        tostring(leaderGuid),
        tostring(reason),
        removed))
    return removed
end

local function stopActivePerformanceSessionsForGuid(guid, reason)
    guid = tonumber(guid)
    if not guid then
        return 0
    end

    local removed = 0
    local sent = 0
    local rootKeys = {}
    for key, session in pairs(activePerformanceSessions) do
        if tonumber(session.sourceGuid) == guid then
            if not session.parentSessionKey then
                table.insert(rootKeys, key)
            end
            sent = sent + sendPerformanceSessionPayloadToCell(
                makePerformanceSessionStopPayload(session, reason),
                session.cell,
                guid)
            activePerformanceSessions[key] = nil
            removed = removed + 1
        end
    end

    if removed > 0 then
        writeActivePerformanceSessionSnapshot()
        mp.log(string.format(
            "[bardcraft] performance sessions stopped guid=%s reason=%s sessions=%d sent=%d",
            tostring(guid),
            tostring(reason),
            removed,
            sent))
    end

    for _, rootKey in ipairs(rootKeys) do
        stopChildPerformanceSessions(rootKey, "root-" .. tostring(reason))
    end

    return removed
end

local function moveActivePerformanceSessionsForGuid(guid, oldCell, newCell)
    guid = tonumber(guid)
    if not guid or not newCell then
        return 0
    end

    local moved = 0
    for _, session in pairs(activePerformanceSessions) do
        if tonumber(session.sourceGuid) == guid then
            local previousCell = session.cell or oldCell
            if previousCell ~= newCell then
                local stopSent = 0
                if previousCell then
                    stopSent = sendPerformanceSessionPayloadToCell(
                        makePerformanceSessionStopPayload(session, "source-cell-change"),
                        previousCell,
                        guid)
                end

                session.cell = newCell
                session.acked = {}
                session.suppressionLogged = nil
                session.suppressedHighChurn = 0

                local startPayload = currentSessionPayload(session, serverUptime())
                local startSent = sendPerformanceSessionPayloadToCell(startPayload, newCell, guid)
                moved = moved + 1

                mp.log(string.format(
                    "[bardcraft] performance session moved guid=%s session=%s oldCell=%s newCell=%s stopSent=%d startSent=%d song=%s",
                    tostring(guid),
                    tostring(session.key),
                    tostring(previousCell),
                    tostring(newCell),
                    stopSent,
                    startSent,
                    tostring(startPayload and startPayload.songTitle)))
            end
        end
    end

    if moved > 0 then
        writeActivePerformanceSessionSnapshot()
    end

    return moved
end

local function findActiveSessionForCommand(player, selector)
    local sessions = activeSessionsForPlayer(player)
    if #sessions == 0 then
        return nil, sessions
    end

    if selector == nil or selector == "" then
        return #sessions == 1 and sessions[1] or nil, sessions
    end

    local index = tonumber(selector)
    if index and sessions[index] then
        return sessions[index], sessions
    end

    for _, session in ipairs(sessions) do
        if tostring(session.key) == tostring(selector) then
            return session, sessions
        end
    end

    return nil, sessions
end

local function splitCommandArgs(text)
    local args = {}
    for token in tostring(text or ""):gmatch("%S+") do
        table.insert(args, token)
    end
    return args
end

local function joinCommandArgs(args, startIndex)
    local parts = {}
    for index = startIndex, #args do
        table.insert(parts, args[index])
    end
    return table.concat(parts, " ")
end

local function sendBardcraftSessionList(player)
    local sessions = activeSessionsForPlayer(player)
    if #sessions == 0 then
        player:sendMessage("[Bardcraft] No active performances in this cell.")
    else
        player:sendMessage("[Bardcraft] Active performances in this cell:")
        for index, session in ipairs(sessions) do
            local payload = currentSessionPayload(session, serverUptime()) or {}
            player:sendMessage(string.format(
                "  %d. %s - %s part=%s session=%s",
                index,
                tostring(session.sourceName or session.sourceGuid),
                tostring(payload.songTitle or "unknown song"),
                tostring(payload.partIndex or "?"),
                tostring(session.key)))
        end
    end

    mp.log(string.format(
        "[bardcraft] command list guid=%s name=%s cell=%s sessions=%d",
        tostring(player and player.guid),
        tostring(player and player.name),
        tostring(playerCellKey(player)),
        #sessions))
end

local function sendBardcraftJoin(player, selector, requestedPart, options)
    options = options or {}
    local session, sessions = findActiveSessionForCommand(player, selector)
    if not session then
        if not options.silent then
            if #sessions > 1 and (selector == nil or selector == "") then
                player:sendMessage(
                    "[Bardcraft] Multiple performances are active. Use /bcperf list, then /bcperf join <number> [part].")
            else
                player:sendMessage("[Bardcraft] No matching active performance in this cell.")
            end
        end
        mp.log(string.format(
            "[bardcraft] command join skipped guid=%s name=%s selector=%s sessions=%d reason=%s",
            tostring(player and player.guid),
            tostring(player and player.name),
            tostring(selector),
            #sessions,
            tostring(options.reason)))
        return
    end

    local now = serverUptime()
    local payload = currentSessionPayload(session, now)
    if type(payload) ~= "table" then
        player:sendMessage("[Bardcraft] That performance is no longer joinable.")
        return
    end
    local occupiedPartIndices = collectOccupiedPartIndices(session, now)

    joinRequestSequence = joinRequestSequence + 1
    local joinRequestKey = tostring(now) .. ":" .. tostring(joinRequestSequence) .. ":" .. tostring(player.guid)
    pendingJoinRequests[joinRequestKey] = {
        playerGuid = tonumber(player.guid),
        sessionKey = session.key,
        requestedPart = requestedPart,
        occupiedPartIndices = occupiedPartIndices,
        createdAt = now,
    }
    mp.log(string.format(
        "[bardcraft] join request stored key=%s session=%s guid=%s pendingCount=%d",
        tostring(joinRequestKey),
        tostring(session.key),
        tostring(player.guid),
        tableCount(pendingJoinRequests)))

    local preparePayload = {
        joinRequestKey = joinRequestKey,
        sessionKey = session.key,
        requestedPartIndex = requestedPart,
        songId = payload.songId,
        songTitle = payload.songTitle,
        songSourceFile = payload.songSourceFile,
        songSegmentRevision = payload.songSegmentRevision,
        perfType = payload.perfType,
        sourceName = tostring(session.sourceName or session.sourceGuid),
        sourceGuid = tonumber(session.sourceGuid),
        serverTime = now,
        reason = options.reason,
        occupiedPartIndices = occupiedPartIndices,
    }
    mp.send(tonumber(player.guid), "BC_BardcraftJoinPrepare", preparePayload)
    mp.send(tonumber(player.guid), "BC_BardcraftJoinPreparePing", {
        joinRequestKey = joinRequestKey,
    })
    local legacyPreparePayload = copyRelayValue(preparePayload, 0)
    legacyPreparePayload.isPrepare = true
    mp.send(tonumber(player.guid), "BC_BardcraftJoinPerformance", legacyPreparePayload)
    if not options.silent then
        player:sendMessage(string.format(
            "[Bardcraft] Preparing to join %s - %s.",
            tostring(session.sourceName or session.sourceGuid),
            tostring(payload.songTitle or "unknown song")))
    end

    mp.log(string.format(
        "[bardcraft] join prepare sent guid=%s name=%s session=%s source=%s song=%s joinRequestKey=%s requestedPart=%s occupied=%d reason=%s",
        tostring(player.guid),
        tostring(player.name),
        tostring(session.key),
        tostring(session.sourceName or session.sourceGuid),
        tostring(payload.songTitle),
        joinRequestKey,
        tostring(requestedPart),
        #occupiedPartIndices,
        tostring(options.reason)))
end

local function computeFutureJoinStart(session, now)
    now = tonumber(now) or serverUptime()
    local leadTime = 3.0
    local currentMusicTime = math.max(0,
        (tonumber(session.startMusicTime) or 0)
        + now
        - (tonumber(session.serverStartTime) or now))
    return {
        currentMusicTime = currentMusicTime,
        startMusicTime = currentMusicTime + leadTime,
        startServerTime = now + leadTime,
        delaySeconds = leadTime,
    }
end

local function getOrCreateBand(leader)
    local leaderGuid = tonumber(leader and leader.guid)
    if not leaderGuid then
        return nil
    end

    local band = activeBandsByLeaderGuid[leaderGuid]
    if not band then
        band = {
            leaderGuid = leaderGuid,
            leaderName = playerDisplayName(leader),
            members = {},
            createdAt = serverUptime(),
        }
        activeBandsByLeaderGuid[leaderGuid] = band
    else
        band.leaderName = playerDisplayName(leader)
    end
    return band
end

local function bandMemberList(band)
    local names = {}
    for memberGuid, member in pairs(band and band.members or {}) do
        table.insert(names, string.format("%s(%s)", tostring(member.name or "unknown"), tostring(memberGuid)))
    end
    table.sort(names)
    return names
end

local function purgeExpiredBandInvites(memberGuid)
    memberGuid = tonumber(memberGuid)
    local invites = memberGuid and pendingBandInvitesByMemberGuid[memberGuid] or nil
    if not invites then
        return
    end

    local now = serverUptime()
    for leaderGuid, invite in pairs(invites) do
        if not invite or (now - (tonumber(invite.invitedAt) or 0)) > BAND_INVITE_TTL_SECONDS then
            invites[leaderGuid] = nil
        end
    end
    if next(invites) == nil then
        pendingBandInvitesByMemberGuid[memberGuid] = nil
    end
end

local function clearPendingBandInvitesFromLeader(leaderGuid)
    leaderGuid = tonumber(leaderGuid)
    if not leaderGuid then
        return 0
    end

    local removed = 0
    for memberGuid, invites in pairs(pendingBandInvitesByMemberGuid) do
        if invites and invites[leaderGuid] then
            invites[leaderGuid] = nil
            removed = removed + 1
            if next(invites) == nil then
                pendingBandInvitesByMemberGuid[memberGuid] = nil
            end
        end
    end
    if removed > 0 then
        mp.log(string.format(
            "[bardcraft] pending band invites cleared leader=%s removed=%d",
            tostring(leaderGuid),
            removed))
    end
    return removed
end

local function collectBandInviteList(memberGuid)
    memberGuid = tonumber(memberGuid)
    purgeExpiredBandInvites(memberGuid)

    local invites = memberGuid and pendingBandInvitesByMemberGuid[memberGuid] or nil
    local names = {}
    for leaderGuid, invite in pairs(invites or {}) do
        table.insert(names, string.format("%s(%s)", tostring(invite.leaderName or leaderGuid), tostring(leaderGuid)))
    end
    table.sort(names)
    return names
end

local function collectOutgoingBandInviteList(leaderGuid)
    leaderGuid = tonumber(leaderGuid)
    local names = {}
    if not leaderGuid then
        return names
    end

    local memberGuids = {}
    for memberGuid, _ in pairs(pendingBandInvitesByMemberGuid) do
        table.insert(memberGuids, memberGuid)
    end

    for _, memberGuid in ipairs(memberGuids) do
        purgeExpiredBandInvites(memberGuid)
        local invites = pendingBandInvitesByMemberGuid[memberGuid]
        local invite = invites and invites[leaderGuid]
        if invite then
            table.insert(names, string.format("%s(%s)", tostring(invite.memberName or memberGuid), tostring(memberGuid)))
        end
    end
    table.sort(names)
    return names
end

local function findPendingBandInvite(member, selector)
    local memberGuid = tonumber(member and member.guid)
    if not memberGuid then
        return nil, "missing-member"
    end

    purgeExpiredBandInvites(memberGuid)
    local invites = pendingBandInvitesByMemberGuid[memberGuid]
    if not invites then
        return nil, "no-invites"
    end

    selector = trim(selector)
    if selector == "" then
        local onlyLeaderGuid = nil
        local count = 0
        for leaderGuid, _ in pairs(invites) do
            onlyLeaderGuid = leaderGuid
            count = count + 1
        end
        if count == 1 then
            return tonumber(onlyLeaderGuid), invites[onlyLeaderGuid]
        end
        return nil, count > 1 and "ambiguous" or "no-invites"
    end

    local numericGuid = tonumber(selector)
    if numericGuid and invites[numericGuid] then
        return numericGuid, invites[numericGuid]
    end

    local wanted = normalizeLookup(selector)
    local exactLeaderGuid = nil
    local partialMatches = {}
    for leaderGuid, invite in pairs(invites) do
        local name = normalizeLookup(invite and invite.leaderName)
        if name == wanted then
            exactLeaderGuid = leaderGuid
            break
        end
        if wanted ~= "" and string.find(name, wanted, 1, true) then
            table.insert(partialMatches, leaderGuid)
        end
    end

    if exactLeaderGuid then
        return tonumber(exactLeaderGuid), invites[exactLeaderGuid]
    end
    if #partialMatches == 1 then
        local leaderGuid = partialMatches[1]
        return tonumber(leaderGuid), invites[leaderGuid]
    end
    if #partialMatches > 1 then
        return nil, "ambiguous"
    end
    return nil, "not-found"
end

local function sendBandStatus(player)
    local guid = tonumber(player and player.guid)
    if not guid then
        return
    end

    local ownBand = activeBandForLeader(guid, "status")
    if ownBand then
        local members = bandMemberList(ownBand)
        if #members == 0 then
            player:sendMessage("[Bardcraft] You are leading an empty band.")
        else
            player:sendMessage("[Bardcraft] Band members: " .. table.concat(members, ", "))
        end
        return
    end

    local leaderGuid = bandLeaderByMemberGuid[guid]
    if leaderGuid then
        local band = activeBandForLeader(leaderGuid, "status-member")
        player:sendMessage("[Bardcraft] You are in " .. tostring(band and band.leaderName or leaderGuid) .. "'s band.")
        return
    end

    local invites = collectBandInviteList(guid)
    if #invites > 0 then
        player:sendMessage("[Bardcraft] Pending invites: " .. table.concat(invites, ", "))
        player:sendMessage("[Bardcraft] Use /bc accept <leader> or /bc decline <leader>.")
        return
    end

    local outgoingInvites = collectOutgoingBandInviteList(guid)
    if #outgoingInvites > 0 then
        player:sendMessage("[Bardcraft] Pending outgoing invites: " .. table.concat(outgoingInvites, ", "))
        player:sendMessage("[Bardcraft] Use /bc disband to cancel pending invites.")
        return
    end

    player:sendMessage("[Bardcraft] You are not in a band. Use /bc invite <player> to create one.")
end

local function inviteBandMember(leader, selector)
    local leaderGuid = tonumber(leader and leader.guid)
    local target, reason = findOnlinePlayer(selector)
    if not leaderGuid or not target then
        leader:sendMessage("[Bardcraft] Could not find player: " .. tostring(selector))
        mp.log(string.format(
            "[bardcraft] band invite skipped leader=%s selector=%s reason=%s",
            tostring(leaderGuid),
            tostring(selector),
            tostring(reason)))
        return
    end

    local currentLeader = bandLeaderByMemberGuid[leaderGuid]
    if currentLeader then
        leader:sendMessage("[Bardcraft] Leave your current band before starting one.")
        mp.log(string.format(
            "[bardcraft] band invite skipped leader=%s selector=%s reason=leader-is-member currentLeader=%s",
            tostring(leaderGuid),
            tostring(selector),
            tostring(currentLeader)))
        return
    end

    local targetGuid = tonumber(target.guid)
    if targetGuid == leaderGuid then
        leader:sendMessage("[Bardcraft] You cannot invite yourself.")
        return
    end
    if activeBandForLeader(targetGuid, "invite-target-leading") then
        leader:sendMessage("[Bardcraft] " .. playerDisplayName(target) .. " is already leading a band.")
        return
    end
    local existingLeader = bandLeaderByMemberGuid[targetGuid]
    if existingLeader == leaderGuid then
        leader:sendMessage("[Bardcraft] " .. playerDisplayName(target) .. " is already in your band.")
        return
    end
    if existingLeader and existingLeader ~= leaderGuid then
        leader:sendMessage("[Bardcraft] " .. playerDisplayName(target) .. " is already in another band.")
        return
    end

    pendingBandInvitesByMemberGuid[targetGuid] = pendingBandInvitesByMemberGuid[targetGuid] or {}
    pendingBandInvitesByMemberGuid[targetGuid][leaderGuid] = {
        leaderGuid = leaderGuid,
        leaderName = playerDisplayName(leader),
        memberGuid = targetGuid,
        memberName = playerDisplayName(target),
        invitedAt = serverUptime(),
    }

    leader:sendMessage("[Bardcraft] Invited " .. playerDisplayName(target) .. " to your band.")
    target:sendMessage("[Bardcraft] " .. playerDisplayName(leader) .. " invited you to their band. Use /bc accept "
        .. playerDisplayName(leader) .. " to join.")
    mp.log(string.format(
        "[bardcraft] band invite pending leader=%s leaderName=%s member=%s memberName=%s pending=%d",
        tostring(leaderGuid),
        playerDisplayName(leader),
        tostring(targetGuid),
        playerDisplayName(target),
        tableCount(pendingBandInvitesByMemberGuid[targetGuid])))
end

local function acceptBandInvite(member, selector)
    local memberGuid = tonumber(member and member.guid)
    if not memberGuid then
        return
    end
    if activeBandForLeader(memberGuid, "accept-member-leading") then
        member:sendMessage("[Bardcraft] Disband your current band before joining another.")
        return
    end
    if bandLeaderByMemberGuid[memberGuid] then
        member:sendMessage("[Bardcraft] You are already in a band.")
        return
    end

    local leaderGuid, inviteOrReason = findPendingBandInvite(member, selector)
    if not leaderGuid then
        if inviteOrReason == "ambiguous" then
            member:sendMessage("[Bardcraft] Multiple band invites are pending. Use /bc status, then /bc accept <leader>.")
        else
            member:sendMessage("[Bardcraft] No matching band invite.")
        end
        mp.log(string.format(
            "[bardcraft] band accept skipped member=%s selector=%s reason=%s",
            tostring(memberGuid),
            tostring(selector),
            tostring(inviteOrReason)))
        return
    end

    local leader = mp.getPlayer(leaderGuid)
    if not leader then
        pendingBandInvitesByMemberGuid[memberGuid][leaderGuid] = nil
        member:sendMessage("[Bardcraft] That band leader is no longer online.")
        return
    end
    if bandLeaderByMemberGuid[leaderGuid] then
        pendingBandInvitesByMemberGuid[memberGuid][leaderGuid] = nil
        member:sendMessage("[Bardcraft] That player is no longer leading a band.")
        return
    end

    local band = getOrCreateBand(leader)
    band.members[memberGuid] = {
        guid = memberGuid,
        name = playerDisplayName(member),
        invitedAt = inviteOrReason and inviteOrReason.invitedAt or nil,
        acceptedAt = serverUptime(),
    }
    bandLeaderByMemberGuid[memberGuid] = leaderGuid
    pendingBandInvitesByMemberGuid[memberGuid] = nil
    writeActiveBandSnapshot()

    member:sendMessage("[Bardcraft] You joined " .. playerDisplayName(leader) .. "'s band.")
    leader:sendMessage("[Bardcraft] " .. playerDisplayName(member) .. " joined your band.")
    mp.log(string.format(
        "[bardcraft] band invite accepted leader=%s leaderName=%s member=%s memberName=%s members=%d leaders=%d memberLinks=%d",
        tostring(leaderGuid),
        playerDisplayName(leader),
        tostring(memberGuid),
        playerDisplayName(member),
        tableCount(band.members),
        tableCount(activeBandsByLeaderGuid),
        tableCount(bandLeaderByMemberGuid)))
end

local function declineBandInvite(member, selector)
    local memberGuid = tonumber(member and member.guid)
    local leaderGuid, inviteOrReason = findPendingBandInvite(member, selector)
    if not memberGuid or not leaderGuid then
        member:sendMessage("[Bardcraft] No matching band invite.")
        mp.log(string.format(
            "[bardcraft] band decline skipped member=%s selector=%s reason=%s",
            tostring(memberGuid),
            tostring(selector),
            tostring(inviteOrReason)))
        return
    end

    local invite = pendingBandInvitesByMemberGuid[memberGuid] and pendingBandInvitesByMemberGuid[memberGuid][leaderGuid]
    pendingBandInvitesByMemberGuid[memberGuid][leaderGuid] = nil
    if next(pendingBandInvitesByMemberGuid[memberGuid]) == nil then
        pendingBandInvitesByMemberGuid[memberGuid] = nil
    end

    member:sendMessage("[Bardcraft] Band invite declined.")
    local leader = mp.getPlayer(leaderGuid)
    if leader then
        leader:sendMessage("[Bardcraft] " .. playerDisplayName(member) .. " declined your band invite.")
    end
    mp.log(string.format(
        "[bardcraft] band invite declined leader=%s leaderName=%s member=%s memberName=%s",
        tostring(leaderGuid),
        tostring(invite and invite.leaderName),
        tostring(memberGuid),
        playerDisplayName(member)))
end

local function kickBandMember(leader, selector)
    local leaderGuid = tonumber(leader and leader.guid)
    local band = leaderGuid and activeBandForLeader(leaderGuid, "kick") or nil
    if not band then
        leader:sendMessage("[Bardcraft] You are not leading a band.")
        return
    end

    local target, reason = findOnlinePlayer(selector)
    local targetGuid = target and tonumber(target.guid) or tonumber(selector)
    if not targetGuid or not band.members[targetGuid] then
        leader:sendMessage("[Bardcraft] That player is not in your band.")
        mp.log(string.format(
            "[bardcraft] band kick skipped leader=%s selector=%s reason=%s",
            tostring(leaderGuid),
            tostring(selector),
            tostring(reason)))
        return
    end

    local memberName = band.members[targetGuid].name or (target and playerDisplayName(target)) or tostring(targetGuid)
    band.members[targetGuid] = nil
    bandLeaderByMemberGuid[targetGuid] = nil
    stopBandChildSessionsForMember(leaderGuid, targetGuid, "band-kick")
    writeActiveBandSnapshot()

    leader:sendMessage("[Bardcraft] Removed " .. tostring(memberName) .. " from your band.")
    if target then
        target:sendMessage("[Bardcraft] You were removed from " .. playerDisplayName(leader) .. "'s band.")
    end
    mp.log(string.format(
        "[bardcraft] band kick leader=%s member=%s remaining=%d",
        tostring(leaderGuid),
        tostring(targetGuid),
        tableCount(band.members)))
end

local function autoJoinBandMembersForSession(session)
    if type(session) ~= "table" or session.parentSessionKey then
        return 0
    end

    local leaderGuid = tonumber(session.sourceGuid)
    if type(session.payload) == "table"
        and (session.payload.scheduledBandStart == true or session.payload.bandSessionId ~= nil) then
        mp.log(string.format(
            "[bardcraft] band auto-join skipped leader=%s session=%s reason=scheduled-band-start bandSession=%s",
            tostring(leaderGuid),
            tostring(session.key),
            tostring(session.payload.bandSessionId)))
        return 0
    end

    local band = leaderGuid and activeBandForLeader(leaderGuid, "auto-join") or nil
    if not band or tableCount(band.members) == 0 then
        mp.log(string.format(
            "[bardcraft] band auto-join skipped leader=%s session=%s reason=no-band members=%d leaders=%d memberLinks=%d",
            tostring(leaderGuid),
            tostring(session.key),
            band and tableCount(band.members) or 0,
            tableCount(activeBandsByLeaderGuid),
            tableCount(bandLeaderByMemberGuid)))
        return 0
    end

    local sent = 0
    for memberGuid, _ in pairs(band.members) do
        local member = mp.getPlayer(tonumber(memberGuid))
        if member and playerCellKey(member) == session.cell then
            member:sendMessage("[Bardcraft] Band leader started a performance; joining.")
            sendBardcraftJoin(member, session.key, nil, {
                silent = true,
                reason = "band-start",
            })
            sent = sent + 1
        else
            mp.log(string.format(
                "[bardcraft] band auto-join skipped leader=%s member=%s session=%s reason=%s memberCell=%s sessionCell=%s",
                tostring(leaderGuid),
                tostring(memberGuid),
                tostring(session.key),
                member and "different-cell" or "offline",
                tostring(member and playerCellKey(member)),
                tostring(session.cell)))
        end
    end

    mp.log(string.format(
        "[bardcraft] band auto-join leader=%s session=%s members=%d sent=%d",
        tostring(leaderGuid),
        tostring(session.key),
        tableCount(band.members),
        sent))
    return sent
end

local function sendBardcraftInstrumentGrant(player)
    mp.send(tonumber(player.guid), "BC_BardcraftGiveAllInstruments", {})
    player:sendMessage("[Bardcraft] Adding Bardcraft instruments to your inventory.")
    mp.log(string.format(
        "[bardcraft] command instruments guid=%s name=%s",
        tostring(player.guid),
        tostring(player.name)))
end

local function normalizedPartIndex(value)
    local number = tonumber(value)
    if number and number > 0 then
        return math.floor(number)
    end
    return nil
end

local function occupiedPartList(occupied)
    local parts = {}
    for part, enabled in pairs(occupied or {}) do
        if enabled then
            table.insert(parts, part)
        end
    end
    table.sort(parts, function(left, right)
        return tonumber(left) < tonumber(right)
    end)
    return parts
end

local function firstAvailableSuggestedPart(occupied, preferred)
    local candidate = normalizedPartIndex(preferred) or 1
    while occupied[candidate] do
        candidate = candidate + 1
    end
    return candidate
end

local function sameCellBandMembers(leader, band)
    local members = {}
    local leaderCell = playerCellKey(leader)
    for memberGuid, _ in pairs(band and band.members or {}) do
        local member = mp.getPlayer(tonumber(memberGuid))
        if member and playerCellKey(member) == leaderCell then
            table.insert(members, {
                guid = tonumber(memberGuid),
                player = member,
                name = playerDisplayName(member),
            })
        else
            mp.log(string.format(
                "[bardcraft] band scheduled start member skipped leader=%s member=%s reason=%s memberCell=%s leaderCell=%s",
                tostring(leader and leader.guid),
                tostring(memberGuid),
                member and "different-cell" or "offline",
                tostring(member and playerCellKey(member)),
                tostring(leaderCell)))
        end
    end
    table.sort(members, function(left, right)
        return tonumber(left.guid) < tonumber(right.guid)
    end)
    return members
end

local function sendScheduledBandLocalPlay(target, selector, requestedPart, occupied, startServerTime, now, leader, bandSessionId, role)
    local delaySeconds = math.max(0, (tonumber(startServerTime) or now) - now)
    mp.send(tonumber(target.guid), "BC_BardcraftStartLocalPerformance", {
        selector = selector,
        requestedPartIndex = requestedPart,
        occupiedPartIndices = occupiedPartList(occupied),
        serverTime = now,
        startServerTime = startServerTime,
        delaySeconds = delaySeconds,
        bandSessionId = bandSessionId,
        bandLeaderGuid = tonumber(leader and leader.guid),
        bandLeaderName = playerDisplayName(leader),
        bandScheduledStart = true,
        reason = "band-scheduled-start",
        bandRole = role,
    })
end

local function trySendScheduledBandLocalPlay(player, selector, requestedPart)
    local leaderGuid = tonumber(player and player.guid)
    local band = leaderGuid and activeBandForLeader(leaderGuid, "command-play") or nil
    if not band or tableCount(band.members) == 0 then
        return false
    end

    local members = sameCellBandMembers(player, band)
    if #members == 0 then
        return false
    end

    local now = serverUptime()
    local startServerTime = now + BAND_SCHEDULED_START_LEAD_SECONDS
    bandStartSequence = bandStartSequence + 1
    local bandSessionId = string.format(
        "%s:band:%d:%d",
        tostring(leaderGuid),
        math.floor(startServerTime * 1000),
        bandStartSequence)
    pendingScheduledBandStartsByLeaderGuid[leaderGuid] = {
        bandSessionId = bandSessionId,
        startServerTime = startServerTime,
        expiresAt = startServerTime + 8.0,
        selector = selector,
        role = "leader",
    }
    local occupied = {}
    local leaderPart = normalizedPartIndex(requestedPart) or 1

    sendScheduledBandLocalPlay(
        player,
        selector,
        requestedPart or leaderPart,
        occupied,
        startServerTime,
        now,
        player,
        bandSessionId,
        "leader")
    occupied[leaderPart] = true

    for index, member in ipairs(members) do
        local memberPart = firstAvailableSuggestedPart(occupied, leaderPart + index)
        pendingScheduledBandStartsByLeaderGuid[tonumber(member.guid)] = {
            bandSessionId = bandSessionId,
            startServerTime = startServerTime,
            expiresAt = startServerTime + 8.0,
            selector = selector,
            role = "member",
        }
        sendScheduledBandLocalPlay(
            member.player,
            selector,
            memberPart,
            occupied,
            startServerTime,
            now,
            player,
            bandSessionId,
            "member")
        occupied[memberPart] = true
        member.player:sendMessage(string.format(
            "[Bardcraft] Band performance scheduled by %s.",
            playerDisplayName(player)))
    end
    writePendingScheduledBandStartSnapshot()

    player:sendMessage(string.format(
        "[Bardcraft] Band performance scheduled in %.1fs for %d member(s): %s",
        BAND_SCHEDULED_START_LEAD_SECONDS,
        #members,
        selector))
    mp.log(string.format(
        "[bardcraft] band scheduled start leader=%s name=%s selector=%s requestedPart=%s members=%d startServerTime=%.3f delay=%.3f bandSession=%s",
        tostring(player.guid),
        tostring(player.name),
        tostring(selector),
        tostring(requestedPart),
        #members,
        startServerTime,
        BAND_SCHEDULED_START_LEAD_SECONDS,
        tostring(bandSessionId)))
    return true
end

local function sendBardcraftLocalPlay(player, selector, requestedPart)
    selector = tostring(selector or "")
    if selector == "" then
        player:sendMessage(
            "[Bardcraft] Usage: /bcperf play <song id or title> or /bcperf playpart <part> <song id or title>")
        return
    end

    if trySendScheduledBandLocalPlay(player, selector, requestedPart) then
        return
    end

    mp.send(tonumber(player.guid), "BC_BardcraftStartLocalPerformance", {
        selector = selector,
        requestedPartIndex = requestedPart,
    })
    player:sendMessage("[Bardcraft] Starting local performance: " .. selector)
    mp.log(string.format(
        "[bardcraft] command play guid=%s name=%s selector=%s requestedPart=%s",
        tostring(player.guid),
        tostring(player.name),
        selector,
        tostring(requestedPart)))
end

local function commandRest(msg, command)
    if msg == command then
        return ""
    end
    if msg:sub(1, #command + 1) == command .. " " then
        return msg:sub(#command + 2)
    end
    return nil
end

local function sendBandUsage(player)
    player:sendMessage(
        "Usage: /bc invite <player|pid> | /bc accept <leader|pid> | /bc decline <leader|pid> | /bc kick <player|pid> | /bc disband | /bc leave | /bc status")
end

local function handleBandCommand(player, args)
    local subcommand = string.lower(args[1] or "status")

    if subcommand == "status" or subcommand == "list" or subcommand == "" then
        sendBandStatus(player)
    elseif subcommand == "invite" then
        inviteBandMember(player, joinCommandArgs(args, 2))
    elseif subcommand == "accept" then
        acceptBandInvite(player, joinCommandArgs(args, 2))
    elseif subcommand == "decline" or subcommand == "reject" then
        declineBandInvite(player, joinCommandArgs(args, 2))
    elseif subcommand == "kick" then
        kickBandMember(player, joinCommandArgs(args, 2))
    elseif subcommand == "disband" then
        local leaderGuid = tonumber(player and player.guid)
        if not leaderGuid then
            player:sendMessage("[Bardcraft] You are not leading a band.")
            return
        end
        local canceledInvites = clearPendingBandInvitesFromLeader(leaderGuid)
        if not activeBandForLeader(leaderGuid, "command-disband") then
            if canceledInvites > 0 then
                player:sendMessage(string.format("[Bardcraft] Canceled %d pending invite%s.",
                    canceledInvites,
                    canceledInvites == 1 and "" or "s"))
            else
                player:sendMessage("[Bardcraft] You are not leading a band.")
            end
            return
        end
        local removed = disbandBand(leaderGuid, "band-disband", true)
        player:sendMessage(string.format("[Bardcraft] Band disbanded. Removed %d member%s and canceled %d invite%s.",
            removed,
            removed == 1 and "" or "s",
            canceledInvites,
            canceledInvites == 1 and "" or "s"))
    elseif subcommand == "leave" then
        local guid = tonumber(player and player.guid)
        local leaderGuid = removeBandMembership(guid, "band-leave")
        if leaderGuid then
            player:sendMessage("[Bardcraft] You left the band.")
            local leader = mp.getPlayer(leaderGuid)
            if leader then
                leader:sendMessage("[Bardcraft] " .. playerDisplayName(player) .. " left your band.")
            end
            mp.log(string.format(
                "[bardcraft] band leave member=%s leader=%s",
                tostring(guid),
                tostring(leaderGuid)))
        else
            player:sendMessage("[Bardcraft] You are not in a band.")
        end
    else
        sendBandUsage(player)
    end
end

local function handleChat(player, data, env)
    local prefix = (env and env.commandPrefix) or "/"
    local msg = data and data.message or ""

    local bandMenuRest = commandRest(msg, prefix .. "bcm")
    if bandMenuRest ~= nil then
        sendBandStatus(player)
        player:sendMessage("[Bardcraft] Band menu UI is not implemented yet. Use /bc commands for now.")
        mp.log(string.format(
            "[bardcraft] command band menu guid=%s name=%s",
            tostring(player and player.guid),
            tostring(player and player.name)))
        return false
    end

    local bandRest = commandRest(msg, prefix .. "bc")
    if bandRest ~= nil then
        handleBandCommand(player, splitCommandArgs(bandRest))
        return false
    end

    local rest = commandRest(msg, prefix .. "bcperf")
    if rest == nil then
        return nil
    end

    local args = splitCommandArgs(rest)
    local subcommand = string.lower(args[1] or "list")

    if subcommand == "list" or subcommand == "" then
        sendBardcraftSessionList(player)
    elseif subcommand == "join" then
        if string.lower(args[2] or "") == "part" then
            sendBardcraftJoin(player, nil, args[3])
        else
            sendBardcraftJoin(player, args[2], args[3])
        end
    elseif subcommand == "leave" or subcommand == "stop" then
        mp.send(tonumber(player.guid), "BC_BardcraftStopLocalPerformance", {
            reason = "command",
        })
        player:sendMessage("[Bardcraft] Stopping local performance.")
        mp.log(string.format(
            "[bardcraft] command stop guid=%s name=%s",
            tostring(player.guid),
            tostring(player.name)))
    elseif subcommand == "instruments" or subcommand == "items" then
        sendBardcraftInstrumentGrant(player)
    elseif subcommand == "play" then
        sendBardcraftLocalPlay(player, joinCommandArgs(args, 2), nil)
    elseif subcommand == "playpart" then
        sendBardcraftLocalPlay(player, joinCommandArgs(args, 3), args[2])
    else
        player:sendMessage(
            "Usage: /bcperf list | /bcperf join [number|session] [part] | /bcperf join part <part> | /bcperf play <song> | /bcperf playpart <part> <song> | /bcperf instruments | /bcperf stop")
    end

    return false
end

M.interface = {
    handleChat = handleChat,
}

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
    local sessionKey = performanceSessionKey(guid, data.actorId)
    local session = activePerformanceSessions[sessionKey]
    if eventType == "PerformStart" then
        session = updateActivePerformanceSession(guid, source, data.actorId, payload, relayEvent, senderCharacterId(data))
        if session then
            applyPendingScheduledBandStart(guid, session, payload)
        end
    elseif session then
        if sourceCell and session.cell ~= sourceCell then
            moveActivePerformanceSessionsForGuid(guid, session.cell, sourceCell)
            session = activePerformanceSessions[sessionKey]
        else
            session.cell = sourceCell
        end
        if eventType == "PerformStop" then
            payload.sessionKey = session.key
            payload.sessionSequence = session.sessionSequence
        end
    end

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
    local suppressed = 0
    for _, target in ipairs(mp.getPlayers()) do
        local targetGuid = tonumber(target.guid)
        if targetGuid and targetGuid ~= tonumber(guid) and playerCellKey(target) == sourceCell then
            if session and highChurnPerformanceEvents[eventType] and session.acked[targetGuid] then
                suppressed = suppressed + 1
                session.suppressedHighChurn = (tonumber(session.suppressedHighChurn) or 0) + 1
            else
                if session and eventType == "PerformStart" then
                    payload = currentSessionPayload(session, serverUptime()) or payload
                end
                mp.send(targetGuid, "BCPerfRelay", payload)
                sent = sent + 1
            end
        end
    end

    if session and suppressed > 0 and not session.suppressionLogged then
        session.suppressionLogged = true
        mp.log(string.format(
            "[bardcraft] performance session suppressing high churn session=%s event=%s receivers=%d",
            tostring(session.key),
            tostring(eventType),
            suppressed))
    end

    if eventType == "PerformStop" then
        if session and not session.parentSessionKey then
            stopChildPerformanceSessions(sessionKey, "root-stop")
        end
        activePerformanceSessions[sessionKey] = nil
        writeActivePerformanceSessionSnapshot()
    end

    if eventType == "PerformStart" and session and not session.parentSessionKey then
        autoJoinBandMembersForSession(session)
    end

    if eventType == "PerformStart" or eventType == "PerformStop" then
        mp.log(string.format(
            "[bardcraft] performance relay guid=%s name=%s cell=%s event=%s sent=%d suppressed=%d song=%s session=%s parent=%s",
            tostring(guid),
            tostring(source.name),
            tostring(sourceCell),
            tostring(eventType),
            sent,
            suppressed,
            tostring(relayEvent.song and relayEvent.song.title),
            tostring(sessionKey),
            tostring(session and session.parentSessionKey)))
        mp.send(tonumber(guid), "BC_BardcraftPerformanceRelayAck", {
            eventType = eventType,
            sent = sent,
            suppressed = suppressed,
            song = relayEvent.song and relayEvent.song.title,
            sessionKey = sessionKey,
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
    local names = {}
    for _, file in ipairs(payload.files or {}) do
        if type(file) == "table" and file.name then
            table.insert(names, tostring(file.name))
        end
    end
    mp.log(string.format(
        "[bardcraft] hosted midi manifest guid=%s files=%d bytes=%d skipped=%d names=%s",
        tostring(guid),
        tableCount(payload.files),
        tonumber(payload.totalBytes) or 0,
        tonumber(payload.skipped) or 0,
        table.concat(names, " | ")))
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

    local sentNames = {}
    for _, file in ipairs(payload.files or {}) do
        if type(file) == "table" and file.name then
            table.insert(sentNames, tostring(file.name))
        end
    end
    mp.log(string.format(
        "[bardcraft] hosted midi files guid=%s requested=%d files=%d bytes=%d skipped=%d requestedNames=%s sentNames=%s skippedNames=%s",
        tostring(guid),
        tableCount(names),
        tableCount(payload.files),
        tonumber(payload.totalBytes) or 0,
        tonumber(payload.skipped) or 0,
        type(names) == "table" and table.concat(names, " | ") or "",
        table.concat(sentNames, " | "),
        table.concat(payload.skippedNames or {}, " | ")))
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

local function characterHasCustomSong(characterId, songId)
    if not characterId or songId == nil then
        return false
    end

    local wanted = tostring(songId)
    local customSongs = mp.getCharacterStorageForCharacter(characterId, STORAGE_NAMESPACE, "customSongs") or {}
    for _, song in ipairs(type(customSongs) == "table" and customSongs or {}) do
        if type(song) == "table" then
            if song.id ~= nil and tostring(song.id) == wanted then
                return true
            end

            local encodedSong = encodeCustomSong(song)
            if encodedSong and encodedSong.id ~= nil and tostring(encodedSong.id) == wanted then
                return true
            end
        end
    end
    return false
end

local function findPerformanceSongSourceCharacterId(session, songId)
    if not session or songId == nil then
        return nil, nil
    end

    local directCharacterId = tonumber(session.sourceCharacterId)
    if characterHasCustomSong(directCharacterId, songId) then
        return directCharacterId, nil
    end

    local wantedPayload = currentSessionPayload(session, serverUptime())
    local wantedSong = sessionSongIdentity(wantedPayload)
    for _, candidate in pairs(activePerformanceSessionSource()) do
        if candidate ~= session and candidate.cell == session.cell then
            local candidatePayload = currentSessionPayload(candidate, serverUptime())
            local candidateSong = sessionSongIdentity(candidatePayload)
            local candidateCharacterId = tonumber(candidate.sourceCharacterId)
            if candidateSong == wantedSong and characterHasCustomSong(candidateCharacterId, songId) then
                return candidateCharacterId, directCharacterId
            end
        end
    end

    return directCharacterId, nil
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

    return {
        sent = sent,
        encodedChunks = encodedChunks,
        segmentChunks = segmentChunks,
        skipped = skipped,
    }
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
    OnServerInit = function(_)
        activePerformanceSessions = {}
        activeBandsByLeaderGuid = {}
        bandLeaderByMemberGuid = {}
        pendingBandInvitesByMemberGuid = {}
        pendingScheduledBandStartsByLeaderGuid = {}
        clearActivePerformanceSessionSnapshot()
        clearActiveBandSnapshot()
        clearPendingScheduledBandStartSnapshot()
    end,

    OnPlayerDisconnect = function(data)
        local guid = tonumber(data and data.guid)
        stopActivePerformanceSessionsForGuid(guid, "source-disconnect")
        if guid then
            if activeBandsByLeaderGuid[guid] then
                disbandBand(guid, "leader-disconnect", true)
            end
            clearPendingBandInvitesFromLeader(guid)
            pendingBandInvitesByMemberGuid[guid] = nil

            local leaderGuid = removeBandMembership(guid, "member-disconnect")
            if leaderGuid then
                local leader = mp.getPlayer(leaderGuid)
                if leader then
                    leader:sendMessage("[Bardcraft] " .. tostring(guid) .. " left your band by disconnecting.")
                end
                mp.log(string.format(
                    "[bardcraft] band member disconnected member=%s leader=%s",
                    tostring(guid),
                    tostring(leaderGuid)))
            end
        end
    end,

    OnPlayerCellChange = function(data)
        local guid = data and data.guid
        local player = guid and mp.getPlayer(guid) or nil
        local newCell = data and data.newCell or playerCellKey(player)
        moveActivePerformanceSessionsForGuid(guid, data and data.oldCell, newCell)
        sendActivePerformanceSessions(guid, newCell)
    end,

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
        sendActivePerformanceSessions(guid)
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

    BC_RequestBardcraftPerformanceSongRecord = function(data)
        local guid = senderGuid(data)
        local sessionKey = data and data.sessionKey
        local session = sessionKey and activePerformanceSessions[tostring(sessionKey)] or nil
        local sourceCharacterId, fallbackFromCharacterId = findPerformanceSongSourceCharacterId(session,
            data and data.songId)
        local songId = data and data.songId
        if not guid or not sourceCharacterId or not songId then
            mp.log(string.format(
                "[bardcraft] performance song fetch skipped guid=%s session=%s sourceCharacterId=%s songId=%s",
                tostring(guid),
                tostring(sessionKey),
                tostring(sourceCharacterId),
                tostring(songId)))
            return
        end

        local stats = sendCustomSongRecords(guid, sourceCharacterId, data and data.token or "", { songId })
        mp.log(string.format(
            "[bardcraft] performance song fetch guid=%s session=%s sourceCharacterId=%s songId=%s sent=%d fallbackFrom=%s",
            tostring(guid),
            tostring(sessionKey),
            tostring(sourceCharacterId),
            tostring(songId),
            tonumber(stats and stats.sent) or 0,
            tostring(fallbackFromCharacterId)))
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
                serverTime = serverUptime(),
            })
        end
    end,

    BC_RequestBardcraftPerformanceSessions = function(data)
        local guid = senderGuid(data)
        if guid then
            sendActivePerformanceSessions(guid)
        end
    end,

    BC_RequestBardcraftPerformanceAutoJoin = function(data)
        local guid = senderGuid(data)
        local player = guid and mp.getPlayer(guid) or nil
        if player then
            sendBardcraftJoin(player, data and data.selector, data and data.part, {
                silent = true,
                reason = data and data.reason or "auto-join",
            })
        end
    end,

    BC_BardcraftJoinReady = function(data)
        local guid = senderGuid(data)
        local joinRequestKey = data and data.joinRequestKey
        mp.log(string.format(
            "[bardcraft] join ready lookup guid=%s key=%s pendingCount=%d",
            tostring(guid),
            tostring(joinRequestKey),
            tableCount(pendingJoinRequests)))

        local request = joinRequestKey and pendingJoinRequests[tostring(joinRequestKey)] or nil

        if not guid then
            mp.log(string.format(
                "[bardcraft] join ready skipped guid=%s joinRequestKey=%s reason=missing-guid",
                tostring(guid),
                tostring(joinRequestKey)))
            return
        end

        if not request then
            request = {
                playerGuid = tonumber(guid),
                sessionKey = data and data.sessionKey,
                requestedPart = data and data.partIndex,
                createdAt = serverUptime(),
                recovered = true,
            }
            mp.log(string.format(
                "[bardcraft] join ready recovered missing request guid=%s joinRequestKey=%s session=%s part=%s",
                tostring(guid),
                tostring(joinRequestKey),
                tostring(request.sessionKey),
                tostring(request.requestedPart)))
        end

        local session = activePerformanceSessions[tostring(request.sessionKey)]
        if not session then
            pendingJoinRequests[tostring(joinRequestKey)] = nil
            mp.log(string.format(
                "[bardcraft] join ready skipped guid=%s joinRequestKey=%s session=%s reason=missing-session",
                tostring(guid),
                tostring(joinRequestKey),
                tostring(request.sessionKey)))
            return
        end

        local now = serverUptime()
        local start = computeFutureJoinStart(session, now)

        local payload = currentSessionPayload(session, now) or {}
        payload.joinRequestKey = joinRequestKey
        payload.sessionKey = session.key
        payload.childSessionKey = tostring(guid) .. ":" .. tostring(data.actorId or "@0x1")
        payload.requestedPartIndex = data.partIndex or request.requestedPart
        payload.partIndex = data.partIndex or request.requestedPart
        payload.partInstrument = data.partInstrument
        payload.item = data.item
        payload.startMusicTime = start.startMusicTime
        payload.startServerTime = start.startServerTime
        payload.delaySeconds = start.delaySeconds
        payload.joinStartMusicTime = start.startMusicTime
        payload.joinStartServerTime = start.startServerTime
        payload.serverTime = now
        payload.sessionPlayback = true
        payload.joinedSessionKey = session.key
        payload.sourceGuid = session.sourceGuid
        payload.sourceName = session.sourceName

        mp.log(string.format(
            "[bardcraft] join ready received guid=%s name=%s session=%s joinRequestKey=%s part=%s song=%s",
            tostring(guid),
            tostring(mp.getPlayer(guid) and mp.getPlayer(guid).name),
            tostring(session.key),
            tostring(joinRequestKey),
            tostring(data and data.partIndex),
            tostring(payload.songTitle or data and data.songId)))

        mp.log(string.format(
            "[bardcraft] join ready fields guid=%s partIndex=%s instrument=%s partInstrument=%s item=%s",
            tostring(guid),
            tostring(data and data.partIndex),
            tostring(data and data.instrument),
            tostring(data and data.partInstrument),
            tostring(data and data.item)))

        mp.log(string.format(
            "[bardcraft] join future start chosen guid=%s session=%s currentMusicTime=%.3f startMusicTime=%.3f startServerTime=%.3f delaySeconds=%.3f",
            tostring(guid),
            tostring(session.key),
            start.currentMusicTime,
            start.startMusicTime,
            start.startServerTime,
            start.delaySeconds))

        -- Broadcast child session to source/nearby clients so they schedule the remote performer early
        local childRelayPayload = copyRelayValue(payload, 0)
        childRelayPayload.eventType = "PerformStart"
        childRelayPayload.sessionPlayback = true
        childRelayPayload.sessionKey = payload.childSessionKey
        childRelayPayload.joinedSessionKey = session.key
        childRelayPayload.joinStartMusicTime = start.startMusicTime
        childRelayPayload.joinStartServerTime = start.startServerTime
        childRelayPayload.delaySeconds = start.delaySeconds
        childRelayPayload.sourceGuid = tonumber(guid)
        childRelayPayload.sourceName = mp.getPlayer(guid) and mp.getPlayer(guid).name or nil
        childRelayPayload.actorId = data.actorId or "@0x1"
        childRelayPayload.instrument = data.partInstrument or data.instrument
        childRelayPayload.partIndex = data.partIndex
        childRelayPayload.partInstrument = data.partInstrument or data.instrument
        childRelayPayload.item = data.item
        childRelayPayload.sourceCharacterId = senderCharacterId(data)
        mp.log(string.format(
            "[bardcraft] join child relay fields guid=%s child=%s parent=%s partIndex=%s instrument=%s partInstrument=%s item=%s",
            tostring(guid),
            tostring(childRelayPayload.sessionKey),
            tostring(childRelayPayload.joinedSessionKey),
            tostring(childRelayPayload.partIndex),
            tostring(childRelayPayload.instrument),
            tostring(childRelayPayload.partInstrument),
            tostring(childRelayPayload.item)))
        performanceSessionSequence = performanceSessionSequence + 1
        activePerformanceSessions[childRelayPayload.sessionKey] = {
            key = childRelayPayload.sessionKey,
            sourceGuid = tonumber(guid),
            sourceCharacterId = senderCharacterId(data),
            sourceName = mp.getPlayer(guid) and mp.getPlayer(guid).name or nil,
            actorId = childRelayPayload.actorId,
            cell = session.cell,
            parentSessionKey = session.key,
            startMusicTime = start.startMusicTime,
            serverStartTime = start.startServerTime,
            sessionSequence = performanceSessionSequence,
            payload = copyRelayValue(childRelayPayload, 0),
            acked = {},
        }
        writeActivePerformanceSessionSnapshot()
        mp.log(string.format(
            "[bardcraft] join child session registered key=%s sourceGuid=%s sourceChar=%s parent=%s",
            tostring(childRelayPayload.sessionKey),
            tostring(guid),
            tostring(senderCharacterId(data)),
            tostring(session.key)))
        sendPerformanceSessionPayloadToCell(childRelayPayload, session.cell, guid)
        mp.log(string.format(
            "[bardcraft] join child relay broadcast guid=%s session=%s cell=%s",
            tostring(guid),
            tostring(session.key),
            tostring(session.cell)))

        mp.send(tonumber(guid), "BC_BardcraftJoinStart", payload)

        local legacyStartPayload = copyRelayValue(payload, 0)
        legacyStartPayload.isJoinStart = true
        mp.send(tonumber(guid), "BC_BardcraftJoinPerformance", legacyStartPayload)

        mp.log(string.format(
            "[bardcraft] join start sent guid=%s session=%s joinRequestKey=%s",
            tostring(guid),
            tostring(session.key),
            tostring(joinRequestKey)))

        mp.log(string.format(
            "[bardcraft] join start legacy sent guid=%s session=%s joinRequestKey=%s",
            tostring(guid),
            tostring(session.key),
            tostring(joinRequestKey)))

        pendingJoinRequests[tostring(joinRequestKey)] = nil
    end,

    BC_BardcraftPerformanceSessionAck = function(data)
        local guid = senderGuid(data)
        local sessionKey = data and data.sessionKey
        local session = sessionKey and activePerformanceSessions[tostring(sessionKey)] or nil
        if not guid or not session then
            return
        end

        session.acked[tonumber(guid)] = data.ok == true
        mp.log(string.format(
            "[bardcraft] performance session ack guid=%s session=%s ok=%s reason=%s",
            tostring(guid),
            tostring(sessionKey),
            tostring(data.ok == true),
            tostring(data.reason)))
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
