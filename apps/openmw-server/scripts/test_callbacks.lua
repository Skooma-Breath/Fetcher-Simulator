-- server-scripts/test_callbacks.lua
-- ─────────────────────────────────────────────────────────────────────────────
-- Callback test harness for the OpenMW multiplayer server.
-- Load this alongside core.lua to verify every event handler fires correctly.
--
-- HOW TO USE
-- ──────────
-- 1. Make sure this file is in server-scripts/ alongside core.lua.
-- 2. Start the server normally.
-- 3. Follow the test checklist in the console — each test prints a clearly
--    labelled [TEST PASS] or gives you an action to perform.
-- 4. Remove or rename this file before going live.
--
-- IMPORTANT: this file defines its own versions of every callback. Because
-- scripts are loaded alphabetically, "test_callbacks.lua" loads AFTER
-- "core.lua" (t > c), which means these definitions will OVERWRITE core.lua's
-- versions. That is intentional for isolated testing. Do not use both in
-- production.
-- ─────────────────────────────────────────────────────────────────────────────

local PASS = "[TEST PASS]"
local FAIL = "[TEST FAIL]"
local INFO = "[TEST INFO]"

local function pass(name, detail)
    mp.log(PASS .. " " .. name .. (detail and (": " .. detail) or ""))
end

local function fail(name, detail)
    mp.log(FAIL .. " " .. name .. (detail and (": " .. detail) or ""))
end

local function info(msg)
    mp.log(INFO .. " " .. msg)
end

-- ─────────────────────────────────────────────────────────────────────────────
-- TEST 1: OnServerInit
-- Expected: fires once immediately at startup, before any player connects.
-- ─────────────────────────────────────────────────────────────────────────────
function OnServerInit()
    -- Verify mp.* server functions are accessible at init time
    local uptime = mp.getUptime()
    local count  = mp.getPlayerCount()
    local hour   = mp.getWorldTime()

    if type(uptime) == "number" then
        pass("OnServerInit / mp.getUptime", string.format("%.3fs", uptime))
    else
        fail("OnServerInit / mp.getUptime", "returned " .. type(uptime))
    end

    if type(count) == "number" then
        pass("OnServerInit / mp.getPlayerCount", tostring(count) .. " players")
    else
        fail("OnServerInit / mp.getPlayerCount", "returned " .. type(count))
    end

    if type(hour) == "number" then
        pass("OnServerInit / mp.getWorldTime", string.format("%.2fh", hour))
    else
        fail("OnServerInit / mp.getWorldTime", "returned " .. type(hour))
    end

    info("────────────────────────────────────────")
    info("OnServerInit complete. Now connect a client to test the remaining callbacks.")
    info("────────────────────────────────────────")
end

-- ─────────────────────────────────────────────────────────────────────────────
-- TEST 2: OnPlayerConnect
-- Expected: fires once when a client completes the handshake.
-- Verify: player.name, player.guid, mp.getPlayerCount(), mp.getPlayers()
-- ─────────────────────────────────────────────────────────────────────────────
function OnPlayerConnect(player)
    info("OnPlayerConnect fired for: " .. tostring(player.name))

    -- player.name
    if type(player.name) == "string" and #player.name > 0 then
        pass("OnPlayerConnect / player.name", player.name)
    else
        fail("OnPlayerConnect / player.name", "got: " .. tostring(player.name))
    end

    -- player.guid
    if type(player.guid) == "number" and player.guid > 0 then
        pass("OnPlayerConnect / player.guid", tostring(player.guid))
    else
        fail("OnPlayerConnect / player.guid", "got: " .. tostring(player.guid))
    end

    -- mp.getPlayerCount() should now be >= 1
    local count = mp.getPlayerCount()
    if count >= 1 then
        pass("OnPlayerConnect / mp.getPlayerCount", tostring(count))
    else
        fail("OnPlayerConnect / mp.getPlayerCount", "returned " .. tostring(count))
    end

    -- mp.getPlayers() should return a table containing this player
    local players = mp.getPlayers()
    local found = false
    for _, p in ipairs(players) do
        if p.guid == player.guid then found = true break end
    end
    if found then
        pass("OnPlayerConnect / mp.getPlayers contains player")
    else
        fail("OnPlayerConnect / mp.getPlayers does not contain connecting player")
    end

    -- mp.getPlayer(guid) round-trip
    local fetched = mp.getPlayer(player.guid)
    if fetched and fetched.name == player.name then
        pass("OnPlayerConnect / mp.getPlayer round-trip", fetched.name)
    else
        fail("OnPlayerConnect / mp.getPlayer round-trip")
    end

    -- player:setData / getData round-trip
    player:setData("test_key", "hello_world")
    local val = player:getData("test_key")
    if val == "hello_world" then
        pass("OnPlayerConnect / setData+getData round-trip")
    else
        fail("OnPlayerConnect / setData+getData", "got: " .. tostring(val))
    end

    -- player:sendMessage (no crash = pass; check the client received it)
    player:sendMessage("[TEST] sendMessage working - you should see this in-game.")
    pass("OnPlayerConnect / sendMessage (no crash)")

    -- mp.broadcast (check all connected clients see it)
    mp.broadcast("[TEST] broadcast working - all clients should see this.")
    pass("OnPlayerConnect / mp.broadcast (no crash)")

    info("ACTION REQUIRED: move your character to a new cell to test OnPlayerCellChange.")
    info("ACTION REQUIRED: type any message in chat to test OnPlayerSendMessage.")
    info("ACTION REQUIRED: open/close a door to test OnDoorState.")
end

-- ─────────────────────────────────────────────────────────────────────────────
-- TEST 3: OnPlayerCellChange
-- Expected: fires every time a player crosses a cell boundary.
-- Trigger: walk through a door or load door to a new cell.
-- ─────────────────────────────────────────────────────────────────────────────
function OnPlayerCellChange(player, newCell, newCell2)
    -- Note: parameter names from the C++ side are (player, newCell, oldCell)
    local oldCell = newCell2  -- third param is oldCell
    info("OnPlayerCellChange fired: " .. tostring(oldCell) .. " -> " .. tostring(newCell))

    if type(newCell) == "string" and #newCell > 0 then
        pass("OnPlayerCellChange / newCell", newCell)
    else
        fail("OnPlayerCellChange / newCell", "got: " .. tostring(newCell))
    end

    if type(oldCell) == "string" then
        pass("OnPlayerCellChange / oldCell", oldCell)
    else
        fail("OnPlayerCellChange / oldCell", "got: " .. tostring(oldCell))
    end

    -- player.cell should reflect the new cell by the time this fires
    if player.cell == newCell then
        pass("OnPlayerCellChange / player.cell matches newCell")
    else
        fail("OnPlayerCellChange / player.cell mismatch",
             "player.cell=" .. tostring(player.cell) .. " newCell=" .. newCell)
    end
end

-- ─────────────────────────────────────────────────────────────────────────────
-- TEST 4: OnPlayerSendMessage
-- Expected: fires for every chat message. Return false = suppressed.
-- Trigger: type messages in chat. Test commands:
--   "!test_suppress"  → should NOT appear in other clients' chat
--   "!test_relay"     → should appear in chat normally
--   anything else     → relayed normally
-- ─────────────────────────────────────────────────────────────────────────────
function OnPlayerSendMessage(player, msg)
    info("OnPlayerSendMessage fired: [" .. player.name .. "] " .. msg)

    -- Verify arguments
    if type(player.name) ~= "string" or type(msg) ~= "string" then
        fail("OnPlayerSendMessage / argument types")
        return
    end
    pass("OnPlayerSendMessage / argument types OK")

    -- Test suppression
    if msg == "!test_suppress" then
        player:sendMessage("[TEST] Suppress working - this message was NOT relayed.")
        pass("OnPlayerSendMessage / suppress test triggered — check other clients did NOT see the message")
        return false  -- suppress relay

    -- Test relay
    elseif msg == "!test_relay" then
        player:sendMessage("[TEST] Relay working - this message WAS relayed to all clients.")
        pass("OnPlayerSendMessage / relay test triggered — check all clients saw the message")
        -- do NOT return false — let it relay

    -- Test mp.setWorldTime via chat command
    elseif msg == "!test_settime" then
        local original = mp.getWorldTime()
        mp.setWorldTime(12.0)
        local after = mp.getWorldTime()
        if math.abs(after - 12.0) < 0.01 then
            pass("OnPlayerSendMessage / mp.setWorldTime", "set to 12.0h")
            mp.setWorldTime(original)  -- restore
        else
            fail("OnPlayerSendMessage / mp.setWorldTime", "got " .. tostring(after))
        end
        player:sendMessage("[TEST] setWorldTime tested. Time restored.")
        return false
    end
    -- anything else relays normally (nil return = relay)
end

-- ─────────────────────────────────────────────────────────────────────────────
-- TEST 5: OnDoorState
-- Expected: fires once per door entry when a player activates a door.
-- Trigger: open or close any door in the game world.
-- ─────────────────────────────────────────────────────────────────────────────
function OnDoorState(cellId, refId, isOpen)
    info("OnDoorState fired: cell=" .. tostring(cellId)
         .. " ref=" .. tostring(refId)
         .. " open=" .. tostring(isOpen))

    if type(cellId) == "string" and #cellId > 0 then
        pass("OnDoorState / cellId", cellId)
    else
        fail("OnDoorState / cellId", "got: " .. tostring(cellId))
    end

    if type(refId) == "string" then
        pass("OnDoorState / refId", refId)
    else
        fail("OnDoorState / refId", "got: " .. tostring(refId))
    end

    if type(isOpen) == "boolean" then
        pass("OnDoorState / isOpen", tostring(isOpen))
    else
        fail("OnDoorState / isOpen", "got: " .. type(isOpen))
    end
end

-- ─────────────────────────────────────────────────────────────────────────────
-- TEST 6: OnWorldWeather
-- Expected: fires when the host client sends a weather report.
-- Trigger: weather changes naturally, or wait ~30 seconds after connect.
--   (Host sends weather packets periodically.)
-- ─────────────────────────────────────────────────────────────────────────────
function OnWorldWeather(region, current, next, transitionFactor)
    info("OnWorldWeather fired: region=" .. tostring(region)
         .. " current=" .. tostring(current)
         .. " next=" .. tostring(next)
         .. " factor=" .. tostring(transitionFactor))

    if type(region) == "string" then
        pass("OnWorldWeather / region", region)
    else
        fail("OnWorldWeather / region", "got: " .. type(region))
    end

    if type(current) == "number" then
        pass("OnWorldWeather / current", tostring(current))
    else
        fail("OnWorldWeather / current", "got: " .. type(current))
    end

    if type(next) == "number" then
        pass("OnWorldWeather / next", tostring(next))
    else
        fail("OnWorldWeather / next", "got: " .. type(next))
    end

    if type(transitionFactor) == "number" then
        pass("OnWorldWeather / transitionFactor", string.format("%.3f", transitionFactor))
    else
        fail("OnWorldWeather / transitionFactor", "got: " .. type(transitionFactor))
    end
end

-- ─────────────────────────────────────────────────────────────────────────────
-- TEST 7: OnPlayerDisconnect
-- Expected: fires when a player closes the game or disconnects.
-- Trigger: disconnect the test client.
-- ─────────────────────────────────────────────────────────────────────────────
function OnPlayerDisconnect(player, reason)
    info("OnPlayerDisconnect fired: " .. tostring(player.name) .. " (" .. tostring(reason) .. ")")

    if type(player.name) == "string" and #player.name > 0 then
        pass("OnPlayerDisconnect / player.name", player.name)
    else
        fail("OnPlayerDisconnect / player.name", "got: " .. tostring(player.name))
    end

    if type(reason) == "string" then
        pass("OnPlayerDisconnect / reason", reason)
    else
        fail("OnPlayerDisconnect / reason", "got: " .. type(reason))
    end

    -- After disconnect, getPlayer(guid) should return nil
    -- (The cleanup in C++ runs after this callback returns, so we schedule
    --  a tick check instead of testing immediately.)
    local savedGuid = player.guid
    local checkDone = false
    local _orig_tick = OnServerTick
    function OnServerTick(dt)
        if _orig_tick then _orig_tick(dt) end
        if not checkDone then
            checkDone = true
            local gone = mp.getPlayer(savedGuid)
            if gone == nil then
                pass("OnPlayerDisconnect / mp.getPlayer returns nil after disconnect")
            else
                fail("OnPlayerDisconnect / mp.getPlayer still returns player after disconnect")
            end
            -- Restore original tick
            OnServerTick = _orig_tick
        end
    end
end

-- ─────────────────────────────────────────────────────────────────────────────
-- TEST 8: OnServerTick
-- Expected: fires ~20 times per second. dt should be ~0.05s.
-- Auto-validates after 3 seconds then unregisters itself.
-- ─────────────────────────────────────────────────────────────────────────────
local tickCount  = 0
local tickTotal  = 0.0
local tickTested = false

function OnServerTick(dt)
    if tickTested then return end

    tickCount = tickCount + 1
    tickTotal = tickTotal + dt

    -- After ~3 seconds of real time, evaluate
    if tickTotal >= 3.0 then
        tickTested = true

        -- Should have ~60 ticks in 3 seconds at 20 Hz
        if tickCount >= 50 and tickCount <= 80 then
            pass("OnServerTick / tick rate",
                 string.format("%d ticks in %.2fs (~%.1f Hz)",
                               tickCount, tickTotal, tickCount / tickTotal))
        else
            fail("OnServerTick / tick rate",
                 string.format("got %d ticks in %.2fs — expected ~60",
                               tickCount, tickTotal))
        end

        -- dt should be a small positive number
        if type(dt) == "number" and dt > 0 and dt < 1.0 then
            pass("OnServerTick / dt value", string.format("%.4fs", dt))
        else
            fail("OnServerTick / dt value", "got: " .. tostring(dt))
        end

        info("OnServerTick test complete. You can now disconnect the client.")
    end
end
