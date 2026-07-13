local I = require('openmw.interfaces')
local self = require('openmw.self')
local vfs = require('openmw.vfs')

local cells = require('scripts.starwind-compat.starwind-music-cells')

if not I.Music or not I.Music.registerPlaylist or not I.Music.setPlaylistActive then
    return {}
end

local AUDIO_EXTENSIONS = {
    ['.flac'] = true,
    ['.mp3'] = true,
    ['.ogg'] = true,
    ['.wav'] = true,
}

local function normalized(path)
    return string.lower(path:gsub('\\', '/'))
end

local function isAudioPath(path)
    return AUDIO_EXTENSIONS[path:sub(-5)] or AUDIO_EXTENSIONS[path:sub(-4)] or false
end

local function collectTracks(category, starwind)
    local result = {}
    local seen = {}
    local categorySegment = '/' .. category .. '/'
    for path in vfs.pathsWithPrefix('music/') do
        local lowerPath = normalized(path)
        local isStarwind = lowerPath:sub(1, 15) == 'music/starwind/'
        local matchesCategory = lowerPath:find(categorySegment, 1, true) ~= nil
        if isAudioPath(lowerPath) and matchesCategory and isStarwind == starwind and not seen[lowerPath] then
            seen[lowerPath] = true
            result[#result + 1] = path
        end
    end
    table.sort(result)
    return result
end

local playlists = {
    nonStarwindBattle = {
        id = 'fetcher_non_starwind_battle',
        priority = 6,
        randomize = true,
        tracks = collectTracks('battle', false),
    },
    starwindBattle = {
        id = 'fetcher_starwind_battle',
        priority = 5,
        randomize = true,
        tracks = collectTracks('battle', true),
    },
    nonStarwindExplore = {
        id = 'fetcher_non_starwind_explore',
        priority = 90,
        randomize = true,
        tracks = collectTracks('explore', false),
    },
    starwindExplore = {
        id = 'fetcher_starwind_explore',
        priority = 80,
        randomize = true,
        tracks = collectTracks('explore', true),
    },
}

for _, playlist in pairs(playlists) do
    I.Music.registerPlaylist(playlist)
end

local function isStarwindCell(cell)
    if not cell then
        return false
    end
    if cell.isExterior then
        return cells.exteriors[tostring(cell.gridX) .. ',' .. tostring(cell.gridY)] == true
    end
    return cells.interiors[string.lower(cell.name or '')] == true
end

local function setActive(playlist, active)
    I.Music.setPlaylistActive(playlist.id, active)
end

local function onFrame()
    local starwind = isStarwindCell(self.cell)
    local combat = I.Music.isCombatMusicActive and I.Music.isCombatMusicActive() or false

    setActive(playlists.starwindExplore, starwind)
    setActive(playlists.starwindBattle, starwind and combat)
    setActive(playlists.nonStarwindExplore, not starwind)
    setActive(playlists.nonStarwindBattle, not starwind and combat)
end

return {
    engineHandlers = {
        onFrame = onFrame,
    },
}
