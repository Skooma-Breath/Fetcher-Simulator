local mp = require("mp")
local networkPolicy = require("bardcraft_network_policy")
local config = require("config")

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
local performanceSessionSequence = 0
local joinRequestSequence = 0
local bandStartSequence = 0
local npcBandMemberSpawnSequence = 0
local npcBandMemberSpawnSlotsByOwnerGuid = {}
local pendingNpcBandMemberSpawns = {}
local npcBandMembersByOwnerGuid = {}
local pendingJoinRequests = {}
local activePerformanceSessions = {}
local activeBandsByLeaderGuid = {}
local bandLeaderByMemberGuid = {}
local disbandedBandLeadersByGuid = {}
local pendingBandInvitesByMemberGuid = {}
local pendingScheduledBandStartsByLeaderGuid = {}
local sheathedInstrumentByGuid = {}
local sendBandStateToGuid
local sendBandStateForBand
local BAND_INVITE_TTL_SECONDS = 300
local BAND_SCHEDULED_START_LEAD_SECONDS = 3.0
local highChurnPerformanceEvents = {
    NoteEvent = true,
    NoteEndEvent = true,
    TempoEvent = true,
    NewBar = true,
}
local bardcraftNpcRecords = {
    r_bc_n_camilla = true,
    r_bc_n_elara = true,
    r_bc_n_lucian = true,
    r_bc_n_rajira = true,
    r_bc_n_reeds = true,
    r_bc_n_rels = true,
    r_bc_n_sargon = true,
    r_bc_n_strumak = true,
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

local function playerHasActorCellLoaded(player, cell)
    cell = cell and tostring(cell) or nil
    if not player or not cell then
        return false
    end
    if playerCellKey(player) == cell then
        return true
    end
    for _, loadedCell in ipairs(player.loadedActorCells or {}) do
        if tostring(loadedCell) == cell then
            return true
        end
    end
    return false
end

local function playerActorInterestCells(player)
    local cells = {}
    for _, cell in ipairs((player and player.loadedActorCells) or {}) do
        cells[tostring(cell)] = true
    end
    local currentCell = playerCellKey(player)
    if currentCell then
        cells[currentCell] = true
    end
    return cells
end

local function sheathedInstrumentRecordId(value)
    if value == nil or value == false or value == "" then
        return nil
    end
    if type(value) ~= "string" or #value > 128 or value:find("[^%w_%- ]") then
        return nil
    end
    return value
end

local function sendSheathedInstrumentState(sourceGuid, targetGuid)
    sourceGuid = tonumber(sourceGuid)
    targetGuid = tonumber(targetGuid)
    local source = sourceGuid and mp.getPlayer(sourceGuid) or nil
    local target = targetGuid and mp.getPlayer(targetGuid) or nil
    if not source or not target or sourceGuid == targetGuid then
        return false
    end
    local sourceCell = playerCellKey(source)
    if not sourceCell or not playerHasActorCellLoaded(target, sourceCell) then
        return false
    end
    mp.send(targetGuid, "BC_BardcraftSheathedInstrumentState", {
        sourceGuid = sourceGuid,
        sourceName = source.name,
        recordId = sheathedInstrumentByGuid[sourceGuid],
    })
    return true
end

local function broadcastSheathedInstrumentState(sourceGuid)
    local sent = 0
    for _, target in ipairs(mp.getPlayers()) do
        if sendSheathedInstrumentState(sourceGuid, target.guid) then
            sent = sent + 1
        end
    end
    return sent
end

local function sendKnownSheathedInstrumentStates(targetGuid)
    local sent = 0
    for sourceGuid, _ in pairs(sheathedInstrumentByGuid) do
        if sendSheathedInstrumentState(sourceGuid, targetGuid) then
            sent = sent + 1
        end
    end
    return sent
end

local function serverUptime()
    if type(mp.getUptime) == "function" then
        return tonumber(mp.getUptime()) or 0
    end
    return 0
end

local function performanceActorKey(data)
    local actorMpNum = data and tonumber(data.actorMpNum) or nil
    if actorMpNum and actorMpNum > 0 then
        return "mp:" .. tostring(actorMpNum)
    end
    return "ref:" .. tostring(data and data.actorId or "")
end

local function performanceSessionKey(guid, actorData)
    return tostring(guid) .. ":" .. performanceActorKey(actorData)
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

local function bandMemberGuidList(band)
    local guids = {}
    for memberGuid, _ in pairs((band and band.members) or {}) do
        table.insert(guids, tonumber(memberGuid) or tostring(memberGuid))
    end

    table.sort(guids, function(left, right)
        local leftNumber = tonumber(left)
        local rightNumber = tonumber(right)
        if leftNumber and rightNumber then
            return leftNumber < rightNumber
        end
        return tostring(left) < tostring(right)
    end)

    for index, memberGuid in ipairs(guids) do
        guids[index] = tostring(memberGuid)
    end

    if #guids == 0 then
        return "-"
    end
    return table.concat(guids, ",")
end

local function trim(value)
    return tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", "")
end

local bardcraftNetworkPolicy = networkPolicy.get()
local copyNetworkPolicy = networkPolicy.copy
local applyNetworkPolicyFields = networkPolicy.applyFields

local function sendSongUnavailable(guid, reason, songTitle, expectedHash, actualHash)
    if not guid then
        return
    end
    mp.send(tonumber(guid), "BC_BardcraftSongUnavailable", applyNetworkPolicyFields({
        reason = reason,
        songTitle = songTitle,
        expectedHash = expectedHash,
        actualHash = actualHash,
    }))
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

local function sessionAckResolvesHighChurn(sessionPayload, status)
    if type(status) ~= "table" then
        return false
    end
    if status.ok == true or status.reason == "missing-remote-actor" then
        return true
    end

    local importedSong = sessionPayload.songIsImportedMidi == true
        or sessionPayload.songIsMpCustom == true
        or sessionPayload.songIsServerCustom == true
        or sessionPayload.songServerHosted == true
    if importedSong then
        return not bardcraftNetworkPolicy.allowImportedMidiLiveRelayFallback
    end
    return bardcraftNetworkPolicy.requireLocalSongHash
        and (status.reason == "missing-song" or status.reason == "missing-part-notes")
end

local function shouldDropHighChurnRelayBeforeCopy(guid, sourceCell, session, eventType)
    if not highChurnPerformanceEvents[eventType] then
        return false
    end

    if not session then
        return true
    end

    local sessionPayload = type(session.payload) == "table" and session.payload or {}
    local importedSong = sessionPayload.songIsImportedMidi == true
        or sessionPayload.songIsMpCustom == true
        or sessionPayload.songIsServerCustom == true
        or sessionPayload.songServerHosted == true
    if importedSong and not bardcraftNetworkPolicy.allowImportedMidiLiveRelayFallback then
        session.suppressedHighChurn = (tonumber(session.suppressedHighChurn) or 0) + 1
        if not session.importedRelayPolicyDropLogged then
            session.importedRelayPolicyDropLogged = true
            mp.log(string.format(
                "[bardcraft] imported midi high-churn relay disabled session=%s song=%s event=%s",
                tostring(session.key),
                tostring(sessionPayload.songTitle),
                tostring(eventType)))
        end
        return true
    end

    local receiverCount = 0
    local ackedCount = 0
    local waiting = {}
    for _, target in ipairs(mp.getPlayers()) do
        local targetGuid = tonumber(target.guid)
        if targetGuid and targetGuid ~= tonumber(guid) and playerHasActorCellLoaded(target, sourceCell) then
            receiverCount = receiverCount + 1
            local status = session.ackStatus and session.ackStatus[targetGuid] or nil
            if sessionAckResolvesHighChurn(sessionPayload, status) then
                if status.ok == true then
                    ackedCount = ackedCount + 1
                end
            else
                table.insert(waiting, status and string.format(
                    "%s:nack(%s)",
                    tostring(targetGuid),
                    tostring(status.reason)) or tostring(targetGuid))
            end
        end
    end

    if #waiting > 0 then
        local waitSeconds = serverUptime() - (tonumber(session.serverStartTime) or serverUptime())
        if waitSeconds >= 2 and not session.ackWaitLogged then
            session.ackWaitLogged = true
            mp.log(string.format(
                "[bardcraft] performance session waiting for receiver ack session=%s song=%s wait=%.3f receivers=%d waiting=%s",
                tostring(session.key),
                tostring(sessionPayload.songTitle),
                waitSeconds,
                receiverCount,
                table.concat(waiting, ",")))
        end
        return false
    end

    session.suppressedHighChurn = (tonumber(session.suppressedHighChurn) or 0) + receiverCount
    if not session.highChurnFastDropLogged then
        session.highChurnFastDropLogged = true
        mp.log(string.format(
            "[bardcraft] performance session fast-dropping high churn session=%s event=%s reason=%s receivers=%d acked=%d",
            tostring(session.key),
            tostring(eventType),
            receiverCount == 0 and "no-receivers" or "receivers-session-resolved",
            receiverCount,
            ackedCount))
    end

    return true
end

local function disbandedBandTime(leaderGuid)
    leaderGuid = tonumber(leaderGuid)
    if not leaderGuid then
        return nil
    end
    local cached = tonumber(disbandedBandLeadersByGuid[leaderGuid])
    if cached then
        return cached
    elseif disbandedBandLeadersByGuid[leaderGuid] then
        return 0
    end

    return nil
end

local function bandIsOlderThanDisband(leaderGuid, band)
    local disbandedAt = disbandedBandTime(leaderGuid)
    if not disbandedAt then
        return false
    end

    local createdAt = tonumber(band and band.createdAt)
    if createdAt and createdAt > disbandedAt then
        return false
    end
    return true
end

local function isBandDisbanded(leaderGuid)
    return disbandedBandTime(leaderGuid) ~= nil
end

local function markBandDisbanded(leaderGuid)
    leaderGuid = tonumber(leaderGuid)
    if not leaderGuid then
        return
    end

    local disbandedAt = serverUptime()
    disbandedBandLeadersByGuid[leaderGuid] = disbandedAt
end

local function clearBandDisbanded(leaderGuid)
    leaderGuid = tonumber(leaderGuid)
    if not leaderGuid then
        return
    end

    disbandedBandLeadersByGuid[leaderGuid] = nil
end

local function removeBandRuntimeStateForLeader(leaderGuid)
    leaderGuid = tonumber(leaderGuid)
    if not leaderGuid then
        return
    end

    for key, _ in pairs(activeBandsByLeaderGuid) do
        if tonumber(key) == leaderGuid then
            activeBandsByLeaderGuid[key] = nil
        end
    end
    for memberGuid, currentLeaderGuid in pairs(bandLeaderByMemberGuid) do
        if tonumber(currentLeaderGuid) == leaderGuid then
            bandLeaderByMemberGuid[memberGuid] = nil
        end
    end
    for key, pending in pairs(pendingScheduledBandStartsByLeaderGuid) do
        if tonumber(key) == leaderGuid
            or (type(pending) == "table" and tonumber(pending.leaderGuid) == leaderGuid) then
            pendingScheduledBandStartsByLeaderGuid[key] = nil
        end
    end
end

local function activeBandForLeader(leaderGuid)
    leaderGuid = tonumber(leaderGuid)
    if not leaderGuid then
        return nil
    end

    local band = activeBandsByLeaderGuid[leaderGuid]
    if band then
        if bandIsOlderThanDisband(leaderGuid, band) then
            activeBandsByLeaderGuid[leaderGuid] = nil
        else
            clearBandDisbanded(leaderGuid)
            return band
        end
    end

    return nil
end

local function activePerformanceSessionSource()
    return activePerformanceSessions
end

local function relayField(event, key)
    return event and event[key] or nil
end

local function flattenBardcraftPerformancePayload(guid, source, data, event)
    local song = type(event.song) == "table" and event.song or {}
    local part = type(event.part) == "table" and event.part or {}
    local timeSig = type(song.timeSig) == "table" and song.timeSig or {}

    return applyNetworkPolicyFields({
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
        actorMpNum = data and tonumber(data.actorMpNum),
        actorRecordId = data and data.actorRecordId,
        actorCell = data and data.actorCell,
        actorIsPlayer = data and data.actorIsPlayer == true,
        sessionKey = relayField(event, "sessionKey"),
        sessionPlayback = relayField(event, "sessionPlayback"),
        serverTime = relayField(event, "serverTime"),
        serverStartTime = relayField(event, "serverStartTime"),
        sessionSequence = relayField(event, "sessionSequence"),
        joinedSessionKey = relayField(event, "joinedSessionKey"),
        bandSessionId = relayField(event, "bandSessionId"),
        bandRole = relayField(event, "bandRole"),
        bandLeaderGuid = tonumber(relayField(event, "bandLeaderGuid")),
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
        songHash = song.contentHash,
        songIsMpCustom = song.isMpCustom == true,
        songIsServerCustom = song.isServerCustom == true,
        songServerHosted = song.serverHosted == true,
        songIsImportedMidi = song.isImportedMidi == true
            or song.isMpCustom == true
            or song.isServerCustom == true
            or song.serverHosted == true,
        partIndex = part.index,
        partInstrument = part.instrument,
        partTitle = part.title,
        partNumOfType = part.numOfType,
    })
end

local function updateActivePerformanceSession(guid, source, actorData, payload, relayEvent, sourceCharacterId, performerCell)
    if type(payload) ~= "table" or type(relayEvent) ~= "table" then
        return nil
    end

    local now = serverUptime()
    local key = performanceSessionKey(guid, actorData)
    performanceSessionSequence = performanceSessionSequence + 1

    payload.sessionKey = key
    payload.sessionPlayback = true
    payload.serverTime = now
    payload.serverStartTime = now
    payload.sessionSequence = performanceSessionSequence
    payload.actorId = actorData and actorData.actorId
    payload.actorMpNum = actorData and tonumber(actorData.actorMpNum)
    payload.actorRecordId = actorData and actorData.actorRecordId
    payload.actorCell = performerCell
    payload.actorIsPlayer = actorData and actorData.actorIsPlayer == true

    activePerformanceSessions[key] = {
        key = key,
        sourceGuid = tonumber(guid),
        sourceCharacterId = tonumber(sourceCharacterId),
        sourceName = source and source.name or nil,
        actorId = actorData and actorData.actorId,
        actorMpNum = actorData and tonumber(actorData.actorMpNum),
        actorRecordId = actorData and actorData.actorRecordId,
        actorIsPlayer = actorData and actorData.actorIsPlayer == true,
        cell = performerCell,
        parentSessionKey = payload.joinedSessionKey and tostring(payload.joinedSessionKey) or nil,
        bandSessionId = payload.bandSessionId and tostring(payload.bandSessionId) or nil,
        bandRole = payload.bandRole,
        bandLeaderGuid = tonumber(payload.bandLeaderGuid),
        startMusicTime = tonumber(payload.time) or 0,
        serverStartTime = now,
        sessionSequence = performanceSessionSequence,
        payload = copyRelayValue(payload, 0),
        acked = {},
        ackStatus = {},
        sourceStartAckSent = false,
    }
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
    payload.actorMpNum = session.actorMpNum
    payload.actorRecordId = session.actorRecordId
    payload.actorCell = session.cell
    payload.actorIsPlayer = session.actorIsPlayer == true
    payload.joinedSessionKey = session.parentSessionKey
    payload.bandSessionId = session.bandSessionId or payload.bandSessionId
    payload.bandRole = session.bandRole or payload.bandRole
    payload.bandLeaderGuid = session.bandLeaderGuid or payload.bandLeaderGuid
    payload.eventType = "PerformStart"
    return payload
end

local function applyPendingScheduledBandStart(guid, session, payload)
    local leaderGuid = tonumber(guid)
    local pending = leaderGuid and pendingScheduledBandStartsByLeaderGuid[leaderGuid] or nil
    if type(pending) ~= "table" or type(session) ~= "table" or type(payload) ~= "table" then
        return false
    end

    local now = serverUptime()
    if now > (tonumber(pending.expiresAt) or 0) then
        pendingScheduledBandStartsByLeaderGuid[leaderGuid] = nil
        return false
    end

    payload.bandSessionId = pending.bandSessionId
    payload.bandRole = pending.role
    payload.bandLeaderGuid = pending.leaderGuid or leaderGuid
    payload.perfType = payload.perfType or pending.perfType
    payload.scheduledBandStart = true
    payload.joinStartMusicTime = 0
    payload.joinStartServerTime = pending.startServerTime
    session.bandSessionId = pending.bandSessionId
    session.bandRole = pending.role
    session.bandLeaderGuid = pending.leaderGuid or leaderGuid
    session.startMusicTime = 0
    session.serverStartTime = pending.startServerTime
    session.payload.bandSessionId = pending.bandSessionId
    session.payload.bandRole = pending.role
    session.payload.bandLeaderGuid = pending.leaderGuid or leaderGuid
    session.payload.perfType = session.payload.perfType or pending.perfType
    session.payload.scheduledBandStart = true
    session.payload.joinStartMusicTime = 0
    session.payload.joinStartServerTime = pending.startServerTime
    session.payload.serverStartTime = pending.startServerTime
    pendingScheduledBandStartsByLeaderGuid[leaderGuid] = nil
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
    if trim(payload.songHash) ~= "" then
        return "hash:" .. tostring(payload.songHash)
    end
    if bardcraftNetworkPolicy.requireLocalSongHash then
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
    local targetCells = playerActorInterestCells(player)
    for _, session in pairs(activePerformanceSessionSource()) do
        if tonumber(session.sourceGuid) ~= playerGuid and targetCells[session.cell] and not session.parentSessionKey then
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

    local targetCells = playerActorInterestCells(target)
    if targetCellOverride then
        targetCells[tostring(targetCellOverride)] = true
    end
    local now = serverUptime()
    local sessions = {}
    for _, session in pairs(activePerformanceSessions) do
        if tonumber(session.sourceGuid) ~= tonumber(guid) and targetCells[session.cell] then
            local payload = currentSessionPayload(session, now)
            if payload then
                table.insert(sessions, payload)
            end
        end
    end

    if #sessions > 0 then
        mp.send(tonumber(guid), "BC_BardcraftPerformanceSessions", {
            serverTime = now,
            cell = playerCellKey(target),
            sessions = sessions,
        })
    end

    mp.log(string.format(
        "[bardcraft] active performance sessions guid=%s cell=%s sent=%d",
        tostring(guid),
        tostring(playerCellKey(target)),
        #sessions))
end

local function sendPerformanceSessionPayloadToCell(payload, cell, excludeGuid)
    if type(payload) ~= "table" or cell == nil then
        return 0
    end

    local sent = 0
    for _, target in ipairs(mp.getPlayers()) do
        local targetGuid = tonumber(target.guid)
        if targetGuid and targetGuid ~= tonumber(excludeGuid) and playerHasActorCellLoaded(target, cell) then
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
        actorMpNum = session.actorMpNum,
        actorRecordId = session.actorRecordId,
        actorCell = session.cell,
        actorIsPlayer = session.actorIsPlayer == true,
        sessionKey = session.key,
        sessionPlayback = true,
        sessionSequence = session.sessionSequence,
        bandSessionId = session.bandSessionId,
        bandRole = session.bandRole,
        bandLeaderGuid = session.bandLeaderGuid,
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
    local stopPayload = makePerformanceSessionStopPayload(session, reason)
    if notifySource and sourceGuid and mp.getPlayer(sourceGuid) then
        if session.actorIsPlayer == false then
            mp.send(sourceGuid, "BCPerfRelay", stopPayload)
        else
            mp.send(sourceGuid, "BC_BardcraftStopLocalPerformance", {
                reason = reason,
                sessionKey = session.key,
                parentSessionKey = session.parentSessionKey,
            })
        end
        sourceNotified = 1
    end

    local sent = sendPerformanceSessionPayloadToCell(
        stopPayload,
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

local function relaySongIdentity(song)
    if type(song) ~= "table" then
        return nil
    end
    if trim(song.contentHash) ~= "" then
        return "hash:" .. tostring(song.contentHash)
    end
    if bardcraftNetworkPolicy.requireLocalSongHash then
        return nil
    end
    if song.id ~= nil then
        return "id:" .. tostring(song.id)
    end
    if song.sourceFile ~= nil then
        return "file:" .. tostring(song.sourceFile)
    end
    if song.title ~= nil then
        return "title:" .. tostring(song.title)
    end
    return nil
end

local function sameOptionalValue(left, right)
    return tostring(left or "") == tostring(right or "")
end

local function isEquivalentActivePerformanceStart(session, sourceCell, event)
    if type(session) ~= "table" or type(session.payload) ~= "table" or type(event) ~= "table" then
        return false
    end
    if session.cell ~= sourceCell then
        return false
    end

    local song = type(event.song) == "table" and event.song or {}
    local part = type(event.part) == "table" and event.part or {}
    return sessionSongIdentity(session.payload) == relaySongIdentity(song)
        and sameOptionalValue(session.parentSessionKey, event.joinedSessionKey)
        and sameOptionalValue(session.bandSessionId, event.bandSessionId)
        and sameOptionalValue(session.payload.partIndex, part.index)
        and sameOptionalValue(session.payload.instrument, event.instrument)
        and sameOptionalValue(session.payload.item, event.item)
        and sameOptionalValue(session.payload.perfType, event.perfType)
end

local function stopBandSessionPerformanceSessions(bandSessionId, sourceSessionKey, reason)
    if not bandSessionId then
        return 0
    end

    local sessionKeys = {}
    for key, session in pairs(activePerformanceSessions) do
        if tostring(session.bandSessionId) == tostring(bandSessionId)
            and tostring(key) ~= tostring(sourceSessionKey) then
            table.insert(sessionKeys, key)
        end
    end

    local removed = 0
    local sent = 0
    local sourceNotified = 0
    for _, sessionKey in ipairs(sessionKeys) do
        local sessionRemoved, sessionSent, sessionSourceNotified = stopPerformanceSessionByKey(
            sessionKey,
            reason,
            true)
        removed = removed + sessionRemoved
        sent = sent + sessionSent
        sourceNotified = sourceNotified + sessionSourceNotified
    end

    if removed > 0 then
        mp.log(string.format(
            "[bardcraft] band performance sessions stopped bandSession=%s source=%s reason=%s sessions=%d sent=%d sourceNotified=%d",
            tostring(bandSessionId),
            tostring(sourceSessionKey),
            tostring(reason),
            removed,
            sent,
            sourceNotified))
    end

    return removed
end

local function stopBandPerformanceSessionsForLeader(leaderGuid, reason)
    leaderGuid = tonumber(leaderGuid)
    if not leaderGuid then
        return 0
    end

    local bandSessions = {}
    for _, session in pairs(activePerformanceSessions) do
        if session.bandSessionId
            and (tonumber(session.bandLeaderGuid) == leaderGuid or tonumber(session.sourceGuid) == leaderGuid) then
            bandSessions[tostring(session.bandSessionId)] = true
        end
    end

    local removed = 0
    for bandSessionId, _ in pairs(bandSessions) do
        removed = removed + stopBandSessionPerformanceSessions(bandSessionId, nil, reason)
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
        if ((parent and tonumber(parent.sourceGuid) == leaderGuid)
                or tonumber(session.bandLeaderGuid) == leaderGuid)
            and tonumber(session.sourceGuid) == memberGuid then
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
    sendBandStateToGuid(memberGuid, reason)
    sendBandStateForBand(leaderGuid, reason)
    mp.log(string.format(
        "[bardcraft] band membership removed leader=%s member=%s reason=%s remaining=%d memberGuids=%s",
        tostring(leaderGuid),
        tostring(memberGuid),
        tostring(reason),
        band and tableCount(band.members) or 0,
        bandMemberGuidList(band)))
    return leaderGuid
end

local function disbandBand(leaderGuid, reason, notifyPlayers)
    leaderGuid = tonumber(leaderGuid)
    local band = leaderGuid and activeBandForLeader(leaderGuid, "disband") or nil
    if not band then
        return 0
    end

    local removed = 0
    local memberGuidSummary = bandMemberGuidList(band)
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
    stopBandPerformanceSessionsForLeader(leaderGuid, reason)

    removeBandRuntimeStateForLeader(leaderGuid)
    markBandDisbanded(leaderGuid)
    sendBandStateToGuid(leaderGuid, reason)
    for _, memberGuid in ipairs(memberGuids) do
        sendBandStateToGuid(memberGuid, reason)
    end
    mp.log(string.format(
        "[bardcraft] band disbanded leader=%s reason=%s members=%d memberGuids=%s",
        tostring(leaderGuid),
        tostring(reason),
        removed,
        memberGuidSummary))
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

local function moveActivePerformanceSession(session, newCell, reason)
    if type(session) ~= "table" or not newCell or session.cell == newCell then
        return false
    end

    local previousCell = session.cell
    local sourceGuid = tonumber(session.sourceGuid)
    local stopPayload = makePerformanceSessionStopPayload(session, reason or "performer-cell-change")

    session.cell = newCell
    session.payload.actorCell = newCell
    session.acked = session.acked or {}
    session.ackStatus = session.ackStatus or {}
    session.suppressionLogged = nil
    session.suppressedHighChurn = 0

    local startPayload = currentSessionPayload(session, serverUptime())
    local stopSent = 0
    local startSent = 0
    local continued = 0
    for _, target in ipairs(mp.getPlayers()) do
        local targetGuid = tonumber(target.guid)
        if targetGuid and targetGuid ~= sourceGuid then
            local hadPreviousCell = previousCell and playerHasActorCellLoaded(target, previousCell) or false
            local hasNewCell = playerHasActorCellLoaded(target, newCell)
            if hadPreviousCell and not hasNewCell then
                mp.send(targetGuid, "BCPerfRelay", stopPayload)
                session.acked[targetGuid] = nil
                session.ackStatus[targetGuid] = nil
                stopSent = stopSent + 1
            elseif hasNewCell and not hadPreviousCell then
                mp.send(targetGuid, "BCPerfRelay", startPayload)
                session.acked[targetGuid] = nil
                session.ackStatus[targetGuid] = nil
                startSent = startSent + 1
            elseif hadPreviousCell and hasNewCell then
                continued = continued + 1
            end
        end
    end
    mp.log(string.format(
        "[bardcraft] performance session moved guid=%s session=%s actorMpNum=%s oldCell=%s newCell=%s stopSent=%d startSent=%d continued=%d song=%s",
        tostring(sourceGuid),
        tostring(session.key),
        tostring(session.actorMpNum),
        tostring(previousCell),
        tostring(newCell),
        stopSent,
        startSent,
        continued,
        tostring(startPayload and startPayload.songTitle)))
    return true
end

local function moveActivePerformanceSessionsForGuid(guid, oldCell, newCell)
    guid = tonumber(guid)
    if not guid or not newCell then
        return 0
    end

    local moved = 0
    for _, session in pairs(activePerformanceSessions) do
        if tonumber(session.sourceGuid) == guid and session.actorIsPlayer ~= false then
            if moveActivePerformanceSession(session, newCell, "source-cell-change") then
                moved = moved + 1
            end
        end
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
        songHash = payload.songHash,
        perfType = payload.perfType,
        sourceName = tostring(session.sourceName or session.sourceGuid),
        sourceGuid = tonumber(session.sourceGuid),
        serverTime = now,
        reason = options.reason,
        occupiedPartIndices = occupiedPartIndices,
    }
    applyNetworkPolicyFields(preparePayload)
    mp.send(tonumber(player.guid), "BC_BardcraftJoinPrepare", preparePayload)
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

    clearBandDisbanded(leaderGuid)
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
    sendBandStateForBand(leaderGuid, "band-accept")

    member:sendMessage("[Bardcraft] You joined " .. playerDisplayName(leader) .. "'s band.")
    leader:sendMessage("[Bardcraft] " .. playerDisplayName(member) .. " joined your band.")
    mp.log(string.format(
        "[bardcraft] band invite accepted leader=%s leaderName=%s member=%s memberName=%s members=%d memberGuids=%s leaders=%d memberLinks=%d",
        tostring(leaderGuid),
        playerDisplayName(leader),
        tostring(memberGuid),
        playerDisplayName(member),
        tableCount(band.members),
        bandMemberGuidList(band),
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
    sendBandStateToGuid(targetGuid, "band-kick")
    sendBandStateForBand(leaderGuid, "band-kick")

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

local function normalizedPartIndex(value)
    local number = tonumber(value)
    if number and number > 0 then
        return math.floor(number)
    end
    return nil
end

local function firstAvailableSuggestedPart(occupied, preferred)
    local candidate = normalizedPartIndex(preferred) or 1
    while occupied[candidate] do
        candidate = candidate + 1
    end
    return candidate
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
            "[bardcraft] band auto-join skipped leader=%s session=%s reason=no-band members=%d memberGuids=%s leaders=%d memberLinks=%d",
            tostring(leaderGuid),
            tostring(session.key),
            band and tableCount(band.members) or 0,
            bandMemberGuidList(band),
            tableCount(activeBandsByLeaderGuid),
            tableCount(bandLeaderByMemberGuid)))
        return 0
    end

    local memberEntries = {}
    for memberGuid, _ in pairs(band.members) do
        table.insert(memberEntries, tonumber(memberGuid))
    end
    table.sort(memberEntries, function(left, right)
        return tonumber(left) < tonumber(right)
    end)

    local sent = 0
    local now = serverUptime()
    local occupied = {}
    for _, part in ipairs(collectOccupiedPartIndices(session, now)) do
        local partIndex = normalizedPartIndex(part)
        if partIndex then
            occupied[partIndex] = true
        end
    end
    local leaderPart = normalizedPartIndex(session.payload and session.payload.partIndex) or 1

    for index, memberGuid in ipairs(memberEntries) do
        local member = mp.getPlayer(tonumber(memberGuid))
        if member and playerHasActorCellLoaded(member, session.cell) then
            local requestedPart = firstAvailableSuggestedPart(occupied, leaderPart + index)
            occupied[requestedPart] = true
            member:sendMessage("[Bardcraft] Band leader started a performance; joining.")
            sendBardcraftJoin(member, session.key, requestedPart, {
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
        "[bardcraft] band auto-join leader=%s session=%s members=%d memberGuids=%s sent=%d",
        tostring(leaderGuid),
        tostring(session.key),
        tableCount(band.members),
        bandMemberGuidList(band),
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

local function sameCellBandMembers(leader, band)
    local members = {}
    local leaderCell = playerCellKey(leader)
    for memberGuid, _ in pairs(band and band.members or {}) do
        local member = mp.getPlayer(tonumber(memberGuid))
        if member and playerHasActorCellLoaded(member, leaderCell) then
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

local function sendScheduledBandLocalPlay(target, selector, requestedPart, occupied, startServerTime, now, leader,
    bandSessionId, role, perfType, npcPartSlotOffset, npcPartSlotStride)
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
        includeNpcBandMembers = true,
        npcPartSlotOffset = npcPartSlotOffset,
        npcPartSlotStride = npcPartSlotStride,
        perfType = perfType,
    })
end

local function trySendScheduledBandLocalPlay(player, selector, requestedPart, perfType, reason)
    local leaderGuid = tonumber(player and player.guid)
    local band = leaderGuid and activeBandForLeader(leaderGuid, reason or "command-play") or nil
    if (not band or tableCount(band.members) == 0) and leaderGuid and isBandDisbanded(leaderGuid) then
        mp.log(string.format(
            "[bardcraft] band scheduled start skipped leader=%s reason=disbanded requestReason=%s",
            tostring(leaderGuid),
            tostring(reason)))
        return false
    end

    if not band or tableCount(band.members) == 0 then
        return false
    end
    clearBandDisbanded(leaderGuid)

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
        leaderGuid = leaderGuid,
        perfType = perfType,
    }
    local occupied = {}
    local leaderPart = normalizedPartIndex(requestedPart) or 1
    occupied[leaderPart] = true
    local participants = {
        {
            player = player,
            part = requestedPart or leaderPart,
            role = "leader",
        },
    }

    for index, member in ipairs(members) do
        local memberPart = firstAvailableSuggestedPart(occupied, leaderPart + index)
        occupied[memberPart] = true
        pendingScheduledBandStartsByLeaderGuid[tonumber(member.guid)] = {
            bandSessionId = bandSessionId,
            startServerTime = startServerTime,
            expiresAt = startServerTime + 8.0,
            selector = selector,
            role = "member",
            leaderGuid = leaderGuid,
            perfType = perfType,
        }
        table.insert(participants, {
            player = member.player,
            part = memberPart,
            role = "member",
        })
        member.player:sendMessage(string.format(
            "[Bardcraft] Band performance scheduled by %s.",
            playerDisplayName(player)))
    end

    local participantCount = #participants
    for index, participant in ipairs(participants) do
        sendScheduledBandLocalPlay(
            participant.player,
            selector,
            participant.part,
            occupied,
            startServerTime,
            now,
            player,
            bandSessionId,
            participant.role,
            perfType,
            index,
            participantCount)
    end
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

    if trySendScheduledBandLocalPlay(player, selector, requestedPart, nil, "command-play") then
        return
    end

    mp.send(tonumber(player.guid), "BC_BardcraftStartLocalPerformance", {
        selector = selector,
        requestedPartIndex = requestedPart,
        includeNpcBandMembers = true,
    })
    player:sendMessage("[Bardcraft] Starting local performance: " .. selector)
    mp.log(string.format(
        "[bardcraft] command play guid=%s name=%s selector=%s requestedPart=%s",
        tostring(player.guid),
        tostring(player.name),
        selector,
        tostring(requestedPart)))
end

local function performanceStartSelector(payload)
    if type(payload) ~= "table" then
        return nil
    end

    local selector = payload.songId or payload.songSourceFile or payload.songTitle
    if selector == nil or tostring(selector) == "" then
        return nil
    end
    return tostring(selector)
end

local function tryConvertBandUiStartToScheduled(source, payload)
    if not source or type(payload) ~= "table" then
        return false
    end
    if payload.scheduledBandStart == true or payload.bandSessionId ~= nil or payload.joinedSessionKey ~= nil then
        return false
    end

    local leaderGuid = tonumber(source.guid)
    local band = leaderGuid and activeBandForLeader(leaderGuid, "ui-start", false) or nil
    if not band or tableCount(band.members) == 0 then
        return false
    end

    local selector = performanceStartSelector(payload)
    if not selector then
        mp.log(string.format(
            "[bardcraft] band ui start conversion skipped leader=%s reason=missing-selector song=%s sourceFile=%s id=%s",
            tostring(leaderGuid),
            tostring(payload.songTitle),
            tostring(payload.songSourceFile),
            tostring(payload.songId)))
        return false
    end

    local converted = trySendScheduledBandLocalPlay(source, selector, payload.partIndex, payload.perfType, "ui-start")
    if converted then
        mp.log(string.format(
            "[bardcraft] band ui start converted leader=%s selector=%s part=%s members=%d",
            tostring(leaderGuid),
            tostring(selector),
            tostring(payload.partIndex),
            tableCount(band.members)))
    end
    return converted
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
        "Usage: /bc invite <player|pid> | /bc accept <leader|pid> | /bc decline <leader|pid> | /bc kick <player|pid> | /bc disband | /bc leave | /bc status | /bc npc [recordId|clear]")
end

local function spawnNpcBandMember(player, requestedRecordId)
    local recordId = normalizeLookup(requestedRecordId)
    if recordId == "" then
        recordId = "r_bc_n_camilla"
    end
    if not bardcraftNpcRecords[recordId] then
        player:sendMessage("[Bardcraft] Unknown bard NPC record: " .. tostring(recordId))
        return false
    end

    local cell = playerCellKey(player)
    local position = player and player.position or nil
    if not cell or type(position) ~= "table" then
        player:sendMessage("[Bardcraft] Your current position is not available.")
        return false
    end

    local ownerGuid = tonumber(player.guid) or 0
    local spawnSlot = npcBandMemberSpawnSlotsByOwnerGuid[ownerGuid] or 0
    npcBandMemberSpawnSlotsByOwnerGuid[ownerGuid] = spawnSlot + 1
    local slotsPerRing = 8
    local ring = math.floor(spawnSlot / slotsPerRing)
    local slotInRing = spawnSlot % slotsPerRing
    local spawnDistance = 180 + ring * 96
    local rotation = (tonumber(position.rz) or 0) + slotInRing * (math.pi * 2 / slotsPerRing)
    local spawnPosition = {
        x = (tonumber(position.x) or 0) + math.sin(-rotation) * spawnDistance,
        y = (tonumber(position.y) or 0) + math.cos(-rotation) * spawnDistance,
        z = tonumber(position.z) or 0,
        rx = 0,
        ry = 0,
        rz = rotation + math.pi,
    }
    if string.sub(cell, 1, 4) == "EXT:" then
        cell = string.format(
            "EXT:%d,%d",
            math.floor(spawnPosition.x / 8192),
            math.floor(spawnPosition.y / 8192))
    end

    npcBandMemberSpawnSequence = npcBandMemberSpawnSequence + 1
    local requestId = string.format("%s:%d:%d", tostring(player.guid), npcBandMemberSpawnSequence,
        math.floor(serverUptime() * 1000))
    if not mp.spawnActor(recordId, 0, 0, cell, spawnPosition, { persistent = false }) then
        player:sendMessage("[Bardcraft] Failed to queue bard NPC spawn.")
        return false
    end
    pendingNpcBandMemberSpawns[requestId] = {
        ownerGuid = ownerGuid,
        ownerName = playerDisplayName(player),
        recordId = recordId,
        cell = cell,
        position = spawnPosition,
        createdAt = serverUptime(),
    }

    local provisionRecipients = 0
    for _, target in ipairs(mp.getPlayers()) do
        if playerHasActorCellLoaded(target, cell) then
            mp.send(tonumber(target.guid), "BC_BardcraftProvisionNpcBandMember", {
                requestId = requestId,
                recordId = recordId,
                cell = cell,
                position = spawnPosition,
                ownerGuid = tonumber(player.guid),
                ownerName = playerDisplayName(player),
                isOwner = tonumber(target.guid) == tonumber(player.guid),
            })
            provisionRecipients = provisionRecipients + 1
        end
    end
    player:sendMessage("[Bardcraft] Spawning and recruiting " .. recordId .. ".")
    mp.log(string.format(
        "[bardcraft] npc band member spawn queued guid=%s name=%s request=%s record=%s cell=%s slot=%d ring=%d recipients=%d",
        tostring(player.guid),
        tostring(player.name),
        requestId,
        recordId,
        cell,
        spawnSlot,
        ring,
        provisionRecipients))
    return true
end

local function recordNpcBandMemberProvision(guid, data)
    if type(data) ~= "table" or data.ok ~= true then
        return
    end

    local actorMpNum = tonumber(data.actorMpNum)
    if not actorMpNum or actorMpNum <= 0 then
        return
    end

    local requestId = data.requestId and tostring(data.requestId) or nil
    local pending = requestId and pendingNpcBandMemberSpawns[requestId] or nil
    local ownerGuid = tonumber(data.ownerGuid) or tonumber(pending and pending.ownerGuid) or tonumber(guid)
    if not ownerGuid then
        return
    end

    npcBandMembersByOwnerGuid[ownerGuid] = npcBandMembersByOwnerGuid[ownerGuid] or {}
    local record = npcBandMembersByOwnerGuid[ownerGuid][actorMpNum] or {}
    record.ownerGuid = ownerGuid
    record.actorMpNum = actorMpNum
    record.recordId = data.recordId and tostring(data.recordId) or record.recordId or (pending and pending.recordId) or nil
    record.cell = record.cell or (pending and pending.cell) or nil
    record.requestId = requestId or record.requestId
    record.provisionGuid = tonumber(guid)
    record.provisionedAt = serverUptime()
    record.actorIdsByGuid = record.actorIdsByGuid or {}
    if data.actorId ~= nil and guid then
        record.actorIdsByGuid[tonumber(guid)] = tostring(data.actorId)
        if data.isOwner == true or record.actorId == nil then
            record.actorId = tostring(data.actorId)
        end
    end
    npcBandMembersByOwnerGuid[ownerGuid][actorMpNum] = record
    if requestId then
        pendingNpcBandMemberSpawns[requestId] = nil
    end
end

local function updateNpcBandMemberCell(guid, data)
    local player = guid and mp.getPlayer(guid) or nil
    local actorMpNum = data and tonumber(data.actorMpNum) or nil
    local actorCell = trim(data and data.actorCell)
    if not player or not actorMpNum or actorMpNum <= 0 or actorCell == "" then
        return
    end
    if not playerHasActorCellLoaded(player, actorCell) then
        return
    end

    for ownerGuid, records in pairs(npcBandMembersByOwnerGuid) do
        local record = records[actorMpNum]
        if record then
            if record.cell ~= actorCell then
                local previousCell = record.cell
                record.cell = actorCell
                record.cellReporterGuid = tonumber(guid)
                record.cellUpdatedAt = serverUptime()
                mp.log(string.format(
                    "[bardcraft] npc band member cell updated owner=%s actorMpNum=%s from=%s to=%s reporter=%s",
                    tostring(ownerGuid),
                    tostring(actorMpNum),
                    tostring(previousCell),
                    tostring(actorCell),
                    tostring(guid)))
            end
            return
        end
    end
end

local function stopNpcBandMemberSessions(ownerGuid, actorMpNum, reason)
    local keys = {}
    for key, session in pairs(activePerformanceSessions) do
        if tonumber(session.sourceGuid) == tonumber(ownerGuid)
            and session.actorIsPlayer == false
            and tonumber(session.actorMpNum) == tonumber(actorMpNum)
        then
            table.insert(keys, key)
        end
    end

    local removed = 0
    local sent = 0
    local sourceNotified = 0
    for _, key in ipairs(keys) do
        local childRemoved, childSent, childSourceNotified = stopPerformanceSessionByKey(key, reason, true)
        removed = removed + childRemoved
        sent = sent + childSent
        sourceNotified = sourceNotified + childSourceNotified
    end
    return removed, sent, sourceNotified
end

local function npcBandMemberRemovalCells(player, record, actorMpNum)
    local cells = {}
    local seen = {}
    local function addCell(cell)
        cell = trim(cell)
        if cell ~= "" and not seen[cell] then
            seen[cell] = true
            table.insert(cells, cell)
        end
    end

    addCell(record and record.cell)
    for _, session in pairs(activePerformanceSessions) do
        if tonumber(session.actorMpNum) == tonumber(actorMpNum) then
            addCell(session.cell)
        end
    end
    if #cells == 0 then
        for cell, _ in pairs(playerActorInterestCells(player)) do
            addCell(cell)
        end
    end
    return cells
end

local function sendNpcBandMembersDespawned(ownerGuid, records, reason)
    local actorMpNums = {}
    local actorIds = {}
    local recordIds = {}
    for actorMpNum, record in pairs(records or {}) do
        table.insert(actorMpNums, tonumber(actorMpNum))
        if record.actorId then
            table.insert(actorIds, record.actorId)
        end
        if record.recordId then
            table.insert(recordIds, record.recordId)
        end
    end
    table.sort(actorMpNums)

    local payload = {
        ownerGuid = tonumber(ownerGuid),
        actorMpNums = actorMpNums,
        actorIds = actorIds,
        recordIds = recordIds,
        reason = reason,
    }
    local sent = 0
    for _, target in ipairs(mp.getPlayers()) do
        local targetGuid = tonumber(target.guid)
        if targetGuid then
            mp.send(targetGuid, "BC_BardcraftNpcBandMembersDespawned", payload)
            sent = sent + 1
        end
    end
    return sent
end

local function despawnNpcBandMembers(player)
    local ownerGuid = tonumber(player and player.guid)
    if not ownerGuid then
        return false
    end

    local records = npcBandMembersByOwnerGuid[ownerGuid] or {}
    if next(records) == nil then
        player:sendMessage("[Bardcraft] You do not have any spawned NPC band members to clear.")
        return false
    end

    local removedSessions = 0
    local stopSent = 0
    local sourceNotified = 0
    local removals = {}
    for actorMpNum, record in pairs(records) do
        removals[actorMpNum] = npcBandMemberRemovalCells(player, record, actorMpNum)
        local sessionRemoved, sessionSent, sessionSourceNotified
            = stopNpcBandMemberSessions(ownerGuid, actorMpNum, "npc-band-despawn")
        removedSessions = removedSessions + sessionRemoved
        stopSent = stopSent + sessionSent
        sourceNotified = sourceNotified + sessionSourceNotified
    end

    npcBandMembersByOwnerGuid[ownerGuid] = nil
    local cleanupSent = sendNpcBandMembersDespawned(ownerGuid, records, "npc-band-despawn")
    local removeQueued = 0
    for actorMpNum, removalCells in pairs(removals) do
        for _, cell in ipairs(removalCells) do
            if mp.removeActor(tonumber(actorMpNum), cell) then
                removeQueued = removeQueued + 1
            end
        end
    end

    player:sendMessage(string.format(
        "[Bardcraft] Cleared %d spawned NPC band member%s.",
        tableCount(records),
        tableCount(records) == 1 and "" or "s"))
    mp.log(string.format(
        "[bardcraft] npc band members despawned owner=%s count=%d sessions=%d stopSent=%d sourceNotified=%d removeQueued=%d cleanupSent=%d",
        tostring(ownerGuid),
        tableCount(records),
        removedSessions,
        stopSent,
        sourceNotified,
        removeQueued,
        cleanupSent))
    return true
end

local function bandMemberGuids(band)
    local guids = {}
    for memberGuid, _ in pairs((band and band.members) or {}) do
        table.insert(guids, tonumber(memberGuid))
    end
    table.sort(guids, function(left, right)
        return tonumber(left) < tonumber(right)
    end)
    return guids
end

sendBandStateToGuid = function(guid, reason)
    guid = tonumber(guid)
    if not guid then
        return
    end

    local player = mp.getPlayer(guid)
    if not player then
        return
    end

    local leaderBand = activeBandForLeader(guid, "band-state-leader", false)
    if leaderBand then
        mp.send(guid, "BC_BardcraftBandState", {
            role = "leader",
            leaderGuid = guid,
            leaderName = leaderBand.leaderName,
            memberCount = tableCount(leaderBand.members),
            memberGuids = bandMemberGuids(leaderBand),
            reason = reason,
        })
        mp.log(string.format(
            "[bardcraft] band state sent guid=%s role=leader members=%d reason=%s",
            tostring(guid),
            tableCount(leaderBand.members),
            tostring(reason)))
        return
    end

    local leaderGuid = bandLeaderByMemberGuid[guid]
    local memberBand = leaderGuid and activeBandForLeader(leaderGuid, "band-state-member", false) or nil
    if memberBand then
        mp.send(guid, "BC_BardcraftBandState", {
            role = "member",
            leaderGuid = tonumber(leaderGuid),
            leaderName = memberBand.leaderName,
            memberCount = tableCount(memberBand.members),
            memberGuids = bandMemberGuids(memberBand),
            reason = reason,
        })
        mp.log(string.format(
            "[bardcraft] band state sent guid=%s role=member leader=%s members=%d reason=%s",
            tostring(guid),
            tostring(leaderGuid),
            tableCount(memberBand.members),
            tostring(reason)))
        return
    end

    mp.send(guid, "BC_BardcraftBandState", {
        role = "none",
        memberCount = 0,
        memberGuids = {},
        reason = reason,
    })
    mp.log(string.format(
        "[bardcraft] band state sent guid=%s role=none reason=%s",
        tostring(guid),
        tostring(reason)))
end

sendBandStateForBand = function(leaderGuid, reason)
    leaderGuid = tonumber(leaderGuid)
    local band = leaderGuid and activeBandForLeader(leaderGuid, "band-state-all", false) or nil
    sendBandStateToGuid(leaderGuid, reason)
    for memberGuid, _ in pairs((band and band.members) or {}) do
        sendBandStateToGuid(memberGuid, reason)
    end
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
    elseif subcommand == "npc" or subcommand == "spawnnpc" then
        local npcArg = string.lower(args[2] or "")
        if npcArg == "clear" or npcArg == "despawn" or npcArg == "despawnall" or npcArg == "remove" then
            despawnNpcBandMembers(player)
        else
            spawnNpcBandMember(player, args[2])
        end
    elseif subcommand == "despawnnpc" or subcommand == "despawnnpcs" or subcommand == "clearnpc" or subcommand == "clearnpcs" then
        despawnNpcBandMembers(player)
    else
        sendBandUsage(player)
    end
end

local function handleChat(player, data, env)
    local prefix = (env and env.commandPrefix) or "/"
    local msg = data and data.message or ""

    local bandJoinRest = commandRest(msg, prefix .. "bcjoin")
    if bandJoinRest ~= nil then
        acceptBandInvite(player, bandJoinRest)
        return false
    end

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

local function resolvePerformerCell(source, data, session)
    local sourceCell = playerCellKey(source)
    if type(data) ~= "table" or data.actorIsPlayer == true then
        return sourceCell
    end

    local actorCell = trim(data.actorCell)
    if actorCell == "" then
        return sourceCell
    end
    if playerHasActorCellLoaded(source, actorCell) then
        return actorCell
    end
    if type(session) == "table" and session.actorIsPlayer == false and session.cell == actorCell then
        return actorCell
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
    local sessionKey = performanceSessionKey(guid, data)
    local session = activePerformanceSessions[sessionKey]
    local performerCell = resolvePerformerCell(source, data, session)
    if not performerCell then
        mp.log(string.format(
            "[bardcraft] performance relay rejected guid=%s name=%s actorId=%s actorMpNum=%s actorCell=%s sourceCell=%s reason=actor-cell-not-loaded",
            tostring(guid),
            tostring(source.name),
            tostring(data.actorId),
            tostring(data.actorMpNum),
            tostring(data.actorCell),
            tostring(sourceCell)))
        return
    end

    if session and session.cell ~= performerCell then
        moveActivePerformanceSession(session, performerCell, "performer-cell-change")
    end
    if eventType == "PerformerCell" then
        if not session then
            mp.log(string.format(
                "[bardcraft] performer cell update ignored guid=%s actorId=%s actorMpNum=%s cell=%s reason=missing-session",
                tostring(guid),
                tostring(data.actorId),
                tostring(data.actorMpNum),
                tostring(performerCell)))
        end
        return
    end

    if eventType == "PerformStart" and isEquivalentActivePerformanceStart(session, performerCell, data.event) then
        session.duplicateStartCount = (tonumber(session.duplicateStartCount) or 0) + 1
        if session.duplicateStartCount == 1 then
            mp.log(string.format(
                "[bardcraft] duplicate performance start suppressed guid=%s name=%s session=%s song=%s",
                tostring(guid),
                tostring(source.name),
                tostring(sessionKey),
                tostring(session.payload.songTitle)))
        end
        if not session.sourceStartAckSent then
            session.sourceStartAckSent = true
            mp.send(tonumber(guid), "BC_BardcraftPerformanceRelayAck", {
                eventType = eventType,
                sent = 0,
                suppressed = 0,
                suppressHighChurn = session.payload.songIsImportedMidi == true
                    and not bardcraftNetworkPolicy.allowImportedMidiLiveRelayFallback,
                duplicate = true,
                song = session.payload.songTitle,
                sessionKey = sessionKey,
                actorId = tostring(data.actorId),
            })
        end
        return
    end
    if shouldDropHighChurnRelayBeforeCopy(guid, performerCell, session, eventType) then
        return
    end

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
    if eventType == "PerformStart" then
        if bardcraftNetworkPolicy.requireLocalSongHash and trim(payload.songHash) == "" then
            sendSongUnavailable(guid, "local-song-hash-missing", payload.songTitle, nil, nil)
            mp.log(string.format(
                "[bardcraft] performance start rejected guid=%s name=%s song=%s reason=missing-song-hash",
                tostring(guid),
                tostring(source.name),
                tostring(payload.songTitle)))
            mp.send(tonumber(guid), "BC_BardcraftPerformanceRelayAck", {
                eventType = eventType,
                sent = 0,
                suppressed = 0,
                rejected = true,
                reason = "local-song-hash-missing",
                actorId = tostring(data.actorId),
            })
            return
        end
        if payload.actorIsPlayer == true and tryConvertBandUiStartToScheduled(source, payload) then
            mp.send(tonumber(guid), "BC_BardcraftPerformanceRelayAck", {
                eventType = eventType,
                sent = 0,
                suppressed = 0,
                sessionKey = sessionKey,
                actorId = tostring(data.actorId),
                consumedForScheduledBandStart = true,
            })
            return
        end
        session = updateActivePerformanceSession(
            guid, source, data, payload, relayEvent, senderCharacterId(data), performerCell)
        if session then
            applyPendingScheduledBandStart(guid, session, payload)
        end
    elseif session then
        session.cell = performerCell
        if eventType == "PerformStop" then
            payload.sessionKey = session.key
            payload.sessionSequence = session.sessionSequence
        end
    elseif highChurnPerformanceEvents[eventType] then
        return
    elseif eventType == "PerformStop" then
        mp.log(string.format(
            "[bardcraft] performance stop ignored guid=%s name=%s session=%s reason=missing-session",
            tostring(guid),
            tostring(source.name),
            tostring(sessionKey)))
        return
    end

    if eventType == "PerformStart" or eventType == "PerformStop" then
        mp.log(string.format(
            "[bardcraft] performance relay received guid=%s name=%s cell=%s actorId=%s event=%s song=%s",
            tostring(guid),
            tostring(source.name),
            tostring(performerCell),
            tostring(data.actorId),
            tostring(eventType),
            tostring(relayEvent.song and relayEvent.song.title)))
    end

    local sent = 0
    local suppressed = 0
    for _, target in ipairs(mp.getPlayers()) do
        local targetGuid = tonumber(target.guid)
        if targetGuid and targetGuid ~= tonumber(guid) and playerHasActorCellLoaded(target, performerCell) then
            if session and highChurnPerformanceEvents[eventType]
                and sessionAckResolvesHighChurn(session.payload or {}, session.ackStatus and session.ackStatus[targetGuid])
            then
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
        if session and session.bandSessionId
            and (session.bandRole == "leader" or tonumber(session.bandLeaderGuid) == tonumber(session.sourceGuid)) then
            stopBandSessionPerformanceSessions(session.bandSessionId, session.key, "band-leader-stop")
        elseif session and not session.parentSessionKey then
            stopChildPerformanceSessions(sessionKey, "root-stop")
        end
        activePerformanceSessions[sessionKey] = nil
    end

    if eventType == "PerformStart" and session and not session.parentSessionKey then
        autoJoinBandMembersForSession(session)
    end

    if eventType == "PerformStart" or eventType == "PerformStop" then
        mp.log(string.format(
            "[bardcraft] performance relay guid=%s name=%s cell=%s event=%s sent=%d suppressed=%d song=%s session=%s parent=%s",
            tostring(guid),
            tostring(source.name),
            tostring(performerCell),
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
            suppressHighChurn = eventType == "PerformStart"
                and payload.songIsImportedMidi == true
                and not bardcraftNetworkPolicy.allowImportedMidiLiveRelayFallback,
            song = relayEvent.song and relayEvent.song.title,
            sessionKey = sessionKey,
            actorId = tostring(data.actorId),
        })
        if eventType == "PerformStart" and session then
            session.sourceStartAckSent = true
        end
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
    if not bardcraftNetworkPolicy.allowServerHostedMidiDownloads then
        mp.send(guid, "BC_BardcraftServerSongs", applyNetworkPolicyFields({
            files = {},
            skipped = 0,
            totalBytes = 0,
            disabled = true,
        }))
        return
    end

    local payload = makeHostedMidiManifest()
    applyNetworkPolicyFields(payload)
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

    if not bardcraftNetworkPolicy.allowServerHostedMidiDownloads then
        payload.disabled = true
        applyNetworkPolicyFields(payload)
        mp.send(guid, "BC_BardcraftServerSongFiles", payload)
        return
    end

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
            contentHash = song.contentHash,
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
            contentHash = song.contentHash,
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
        contentHash = song.contentHash,
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
        contentHash = encodedSong.contentHash,
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
                    contentHash = encodedSong.contentHash,
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
        contentHash = encodedSong.contentHash,
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
        networkPolicy = copyNetworkPolicy(),
    }
    applyNetworkPolicyFields(payload)

    for _, key in ipairs(STORAGE_KEYS) do
        if key ~= "customSongs" or bardcraftNetworkPolicy.allowPlayerSongUpload then
            payload[key] = mp.getCharacterStorageForCharacter(characterId, STORAGE_NAMESPACE, key)
        end
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
    local customSongManifest, customSongSkipped = {}, 0
    if bardcraftNetworkPolicy.allowPlayerSongUpload then
        customSongManifest, customSongSkipped = makeCustomSongManifest(customSongs)
    end
    payload.performerStats = copyPerformerStatsWithoutKnownSongs(payload.performerStats)
    payload.knownSongs = nil
    payload.customSongs = nil
    payload.customSongManifest = customSongManifest
    payload.customSongPayload = bardcraftNetworkPolicy.allowPlayerSongUpload and "manifest" or "local-only"
    payload.preserveLocalCustomSongs = not bardcraftNetworkPolicy.allowPlayerSongUpload
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
    if type(data.customSongRecord) == "table" and bardcraftNetworkPolicy.allowPlayerSongUpload then
        customSongRecordUpserted = upsertCustomSongRecord(characterId, data.customSongRecord)
        if not customSongRecordUpserted then
            mp.log(string.format(
                "[bardcraft] custom song upsert failed guid=%s characterId=%s id=%s",
                tostring(guid),
                tostring(characterId),
                tostring(data.customSongRecord.id)))
        end
        changed = customSongRecordUpserted or changed
    elseif type(data.customSongRecord) == "table" then
        mp.log(string.format(
            "[bardcraft] custom song upload rejected guid=%s characterId=%s reason=disabled",
            tostring(guid),
            tostring(characterId)))
    end

    for _, key in ipairs(STORAGE_KEYS) do
        local value = data[key]
        if key == "customSongs" and not bardcraftNetworkPolicy.allowPlayerSongUpload then
            value = nil
        end
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
        networkPolicy.reset()
        pendingJoinRequests = {}
        activePerformanceSessions = {}
        activeBandsByLeaderGuid = {}
        bandLeaderByMemberGuid = {}
        disbandedBandLeadersByGuid = {}
        pendingBandInvitesByMemberGuid = {}
        pendingScheduledBandStartsByLeaderGuid = {}
        sheathedInstrumentByGuid = {}
        mp.log(string.format(
            "[bardcraft] network policy localHash=%s communityMode=%s hostedDownloads=%s playerUpload=%s importedRelayFallback=%s packUrl=%s",
            tostring(bardcraftNetworkPolicy.requireLocalSongHash),
            tostring(bardcraftNetworkPolicy.communitySongSharingMode),
            tostring(bardcraftNetworkPolicy.allowServerHostedMidiDownloads),
            tostring(bardcraftNetworkPolicy.allowPlayerSongUpload),
            tostring(bardcraftNetworkPolicy.allowImportedMidiLiveRelayFallback),
            bardcraftNetworkPolicy.communitySongPackUrl ~= "" and bardcraftNetworkPolicy.communitySongPackUrl or "-"))
    end,

    OnPlayerDisconnect = function(data)
        local guid = tonumber(data and data.guid)
        stopActivePerformanceSessionsForGuid(guid, "source-disconnect")
        if guid then
            sheathedInstrumentByGuid[guid] = nil
            if activeBandsByLeaderGuid[guid] then
                disbandBand(guid, "leader-disconnect", true)
            end
            clearPendingBandInvitesFromLeader(guid)
            pendingBandInvitesByMemberGuid[guid] = nil

            local leaderGuid = bandLeaderByMemberGuid[guid]
            if leaderGuid then
                stopBandChildSessionsForMember(leaderGuid, guid, "member-disconnect")
                local band = activeBandForLeader(leaderGuid, "member-disconnect")
                mp.log(string.format(
                    "[bardcraft] band membership retained on disconnect member=%s leader=%s members=%d memberGuids=%s",
                    tostring(guid),
                    tostring(leaderGuid),
                    band and tableCount(band.members) or 0,
                    bandMemberGuidList(band)))
            end
        end
    end,

    OnPlayerCellChange = function(data)
        local guid = data and data.guid
        local player = guid and mp.getPlayer(guid) or nil
        local newCell = data and data.newCell or playerCellKey(player)
        local oldCell = data and data.oldCell or nil
        if not guid or not newCell or oldCell == newCell then
            return
        end
        moveActivePerformanceSessionsForGuid(guid, oldCell, newCell)
        sendActivePerformanceSessions(guid, newCell)
        broadcastSheathedInstrumentState(guid)
        sendKnownSheathedInstrumentStates(guid)
    end,

    OnPlayerSendMessage = function(data)
        local player = data and data.guid and mp.getPlayer(data.guid) or nil
        if not player then
            return nil
        end
        return handleChat(player, data, {
            commandPrefix = config.COMMAND_PREFIX or "/",
        })
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
        sendBandStateToGuid(guid, "bootstrap")
        sendActivePerformanceSessions(guid)
        sendKnownSheathedInstrumentStates(guid)
    end,

    BC_BardcraftSheathedInstrument = function(data)
        local guid = senderGuid(data)
        local player = guid and mp.getPlayer(guid) or nil
        if not player then
            return
        end
        local requested = data and data.recordId or nil
        local recordId = sheathedInstrumentRecordId(requested)
        if requested ~= nil and requested ~= false and requested ~= "" and not recordId then
            mp.log(string.format(
                "[bardcraft] rejected invalid sheathed instrument guid=%s record=%s",
                tostring(guid),
                tostring(requested)))
            return
        end
        sheathedInstrumentByGuid[guid] = recordId
        local sent = broadcastSheathedInstrumentState(guid)
        mp.log(string.format(
            "[bardcraft] sheathed instrument state guid=%s name=%s record=%s sent=%d",
            tostring(guid),
            tostring(player.name),
            tostring(recordId),
            sent))
    end,

    BC_BardcraftNpcProvisioned = function(data)
        local guid = senderGuid(data)
        local player = guid and mp.getPlayer(guid) or nil
        if not player then
            return
        end
        recordNpcBandMemberProvision(guid, data)
        if data and data.ok == true and data.isOwner == true then
            player:sendMessage(string.format(
                "[Bardcraft] NPC band member ready (mpNum=%s, instruments=%s).",
                tostring(data.actorMpNum),
                tostring(data.instrumentsAdded or 0)))
        elseif data and data.ok ~= true and data.isOwner == true then
            player:sendMessage("[Bardcraft] NPC band member spawned but could not be recruited locally.")
        end
        mp.log(string.format(
            "[bardcraft] npc band member provision result guid=%s request=%s ok=%s actorId=%s actorMpNum=%s record=%s instruments=%s reason=%s",
            tostring(guid),
            tostring(data and data.requestId),
            tostring(data and data.ok == true),
            tostring(data and data.actorId),
            tostring(data and data.actorMpNum),
            tostring(data and data.recordId),
            tostring(data and data.instrumentsAdded),
            tostring(data and data.reason)))
    end,

    BC_BardcraftNpcBandMemberCell = function(data)
        updateNpcBandMemberCell(senderGuid(data), data)
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

        if not bardcraftNetworkPolicy.allowPlayerSongUpload then
            mp.send(guid, "BC_BardcraftCustomSongRecordsEnd", {
                token = data and data.token or "",
                sent = 0,
                skipped = tableCount(data and data.ids),
                disabled = true,
            })
            return
        end

        sendCustomSongRecords(guid, characterId, data and data.token or "", data and data.ids or {})
    end,

    BC_RequestBardcraftPerformanceSongRecord = function(data)
        local guid = senderGuid(data)
        if guid and not bardcraftNetworkPolicy.allowPlayerSongUpload then
            mp.send(guid, "BC_BardcraftCustomSongRecordsEnd", {
                token = data and data.token or "",
                sent = 0,
                skipped = data and data.songId and 1 or 0,
                disabled = true,
            })
            return
        end
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

    BC_RequestBardcraftBandPerformanceStart = function(data)
        local guid = senderGuid(data)
        local player = guid and mp.getPlayer(guid) or nil
        local selector = data and (data.selector or data.songId or data.songSourceFile or data.songTitle)
        local requestedPart = data and (data.requestedPartIndex or data.partIndex or data.part)
        if not player or not selector or tostring(selector) == "" then
            if guid then
                mp.send(guid, "BC_BardcraftBandPerformanceStartRejected", {
                    reason = "invalid-request",
                })
            end
            mp.log(string.format(
                "[bardcraft] band start request rejected guid=%s reason=invalid-request selector=%s",
                tostring(guid),
                tostring(selector)))
            return
        end

        if trySendScheduledBandLocalPlay(player, tostring(selector), requestedPart, data and data.perfType, "ui-request") then
            return
        end

        mp.send(guid, "BC_BardcraftStartLocalPerformance", {
            selector = selector,
            requestedPartIndex = requestedPart,
            perfType = data and data.perfType,
            reason = isBandDisbanded(guid) and "ui-solo-after-disband" or "ui-solo",
        })
        mp.log(string.format(
            "[bardcraft] ui performance start routed solo guid=%s name=%s selector=%s part=%s reason=%s",
            tostring(guid),
            tostring(player.name),
            tostring(selector),
            tostring(requestedPart),
            isBandDisbanded(guid) and "disbanded" or "no-active-band"))
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

        local sessionPayload = currentSessionPayload(session, serverUptime()) or {}
        local expectedHash = trim(sessionPayload.songHash)
        local actualHash = trim(data and data.songHash)
        if bardcraftNetworkPolicy.requireLocalSongHash
            and (expectedHash == "" or actualHash == "" or expectedHash ~= actualHash)
        then
            pendingJoinRequests[tostring(joinRequestKey)] = nil
            local reason = expectedHash == "" and "session-missing-song-hash"
                or actualHash == "" and "local-song-hash-missing"
                or "local-song-hash-mismatch"
            sendSongUnavailable(guid, reason, sessionPayload.songTitle, expectedHash, actualHash)
            mp.log(string.format(
                "[bardcraft] join ready rejected guid=%s session=%s reason=%s expectedHash=%s actualHash=%s",
                tostring(guid),
                tostring(session.key),
                reason,
                expectedHash ~= "" and expectedHash or "-",
                actualHash ~= "" and actualHash or "-"))
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
            ackStatus = {},
            sourceStartAckSent = false,
        }
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

        mp.log(string.format(
            "[bardcraft] join start sent guid=%s session=%s joinRequestKey=%s",
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

        local targetGuid = tonumber(guid)
        session.acked[targetGuid] = data.ok == true
        session.ackStatus = session.ackStatus or {}
        session.ackStatus[targetGuid] = {
            ok = data.ok == true,
            reason = data.reason,
            receivedAt = serverUptime(),
        }
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

M.interfaceName = "BardcraftAdmin"
M.interface = {
    handleCommand = function(data)
        local guid = data and tonumber(data.guid) or nil
        local player = guid and mp.getPlayer(guid) or nil
        local message = data and tostring(data.message or "") or ""
        if not player then
            return { ok = false, error = "player_not_found" }
        end
        if message == "" then
            return { ok = false, error = "message_required" }
        end

        local result = handleChat(player, { message = message }, {
            commandPrefix = config.COMMAND_PREFIX or "/",
        })
        if result == nil then
            return { ok = false, error = "command_not_handled" }
        end
        return { ok = true }
    end,
}

return M
