local config = require("config")
local mp = require("mp")

local M = {}

local rawConfig = type(config.Bardcraft) == "table" and config.Bardcraft or {}
local STORAGE_SECTION = "BardcraftRuntimePolicy"
local runtimeStorage = mp.storage.globalSection(STORAGE_SECTION)
runtimeStorage:setLifeTime(mp.storage.LIFE_TIME.GameSession)

local function trim(value)
    return tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", "")
end

local function storage()
    assert(mp.storage and type(mp.storage.globalSection) == "function", "bardcraft_network_policy.lua requires mp.storage")
    return runtimeStorage
end

local function buildPolicy()
    local runtimeCommunityModeOverride = storage():getCopy("communitySongSharingMode")
    local communityMode = runtimeCommunityModeOverride
    if communityMode == nil then
        communityMode = rawConfig.communitySongSharingMode == true
    end

    local current = {
        requireLocalSongHash = rawConfig.requireLocalSongHash ~= false,
        communitySongSharingMode = communityMode == true,
        communitySongPackUrl = trim(rawConfig.communitySongPackUrl),
    }
    current.allowServerHostedMidiDownloads = current.communitySongSharingMode
        and (runtimeCommunityModeOverride == true or rawConfig.allowServerHostedMidiDownloads == true)
    current.allowPlayerSongUpload = current.communitySongSharingMode
        and rawConfig.allowPlayerSongUpload == true
    current.allowImportedMidiLiveRelayFallback = current.communitySongSharingMode
        and rawConfig.allowImportedMidiLiveRelayFallback == true
    return current
end

local policy = setmetatable({}, {
    __index = function(_, key)
        return buildPolicy()[key]
    end,
    __newindex = function()
        error("Bardcraft network policy is read only")
    end,
})

function M.get()
    return policy
end

function M.copy()
    return buildPolicy()
end

function M.applyFields(payload)
    if type(payload) ~= "table" then
        return payload
    end
    local current = buildPolicy()
    payload.bardcraftRequireLocalSongHash = current.requireLocalSongHash
    payload.bardcraftCommunitySongSharingMode = current.communitySongSharingMode
    payload.bardcraftAllowServerHostedMidiDownloads = current.allowServerHostedMidiDownloads
    payload.bardcraftAllowPlayerSongUpload = current.allowPlayerSongUpload
    payload.bardcraftAllowImportedMidiLiveRelayFallback = current.allowImportedMidiLiveRelayFallback
    payload.bardcraftCommunitySongPackUrl = current.communitySongPackUrl
    return payload
end

function M.setRuntimeCommunityMode(enabled)
    storage():set("communitySongSharingMode", enabled == true)
    return buildPolicy()
end

function M.reset()
    storage():set("communitySongSharingMode", nil)
    return buildPolicy()
end

return M
