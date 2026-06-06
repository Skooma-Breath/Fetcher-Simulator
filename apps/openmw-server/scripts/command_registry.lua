local M = {}

local CATEGORY_ORDER = {
    "General",
    "Voice",
    "Bardcraft",
    "Travel",
    "Surf",
    "Records",
    "Lua",
    "Admin",
}

local COMMANDS = {
    {
        id = "help",
        category = "General",
        usage = "/help",
        summary = "Show the shared command catalog.",
    },
    {
        id = "who",
        category = "General",
        usage = "/who",
        summary = "List online players.",
    },
    {
        id = "list",
        category = "General",
        usage = "/list",
        summary = "List online players and their pids.",
    },
    {
        id = "f",
        category = "General",
        usage = "/f",
        summary = "Pay respects in chat.",
    },
    {
        id = "suicide",
        category = "General",
        usage = "/suicide [message]",
        summary = "Kill your current character with an optional custom death message.",
    },
    {
        id = "time",
        category = "General",
        usage = "/time",
        summary = "Show the authoritative server time.",
    },
    {
        id = "uptime",
        category = "General",
        usage = "/uptime",
        summary = "Show server uptime.",
    },
    {
        id = "nick",
        category = "General",
        usage = "/nick <name>|off",
        summary = "Set or clear your nickname.",
    },
    {
        id = "speech",
        category = "Voice",
        usage = "/speech <type> <index>",
        summary = "Play a race/gender voice line on your character.",
    },
    {
        id = "s",
        category = "Voice",
        usage = "/s <type> <index>",
        summary = "Short alias for /speech.",
    },
    {
        id = "speechhelp",
        category = "Voice",
        usage = "/speechhelp",
        summary = "List valid speech types and indexes.",
    },
    {
        id = "bcperf",
        category = "Bardcraft",
        usage = "/bcperf list|join|play|playpart|instruments|stop",
        summary = "Inspect, start, join, or prepare Bardcraft performances in your cell.",
    },
    {
        id = "login",
        category = "General",
        usage = "/login <password>",
        summary = "Authenticate as an admin.",
    },
    {
        id = "mark",
        category = "Travel",
        usage = "/mark <name>",
        summary = "Save your current position under a mark name.",
    },
    {
        id = "recall",
        category = "Travel",
        usage = "/recall <name>",
        summary = "Teleport to a saved mark.",
    },
    {
        id = "marks",
        category = "Travel",
        usage = "/marks",
        summary = "List your saved marks.",
    },
    {
        id = "unmark",
        category = "Travel",
        usage = "/unmark <name>",
        summary = "Delete a saved mark.",
    },
    {
        id = "surf",
        category = "Surf",
        usage = "/surf [setting value|on|off|clear]",
        summary = "Inspect or edit your surf settings.",
    },
    {
        id = "recordtest",
        category = "Records",
        usage = "/recordtest ...",
        summary = "Create canned dynamic-record test content.",
        adminOnly = true,
    },
    {
        id = "recordstore",
        category = "Records",
        usage = "/recordstore ...",
        summary = "Inspect and manage the dynamic-record catalog.",
        adminOnly = true,
    },
    {
        id = "luabroadcast",
        category = "Lua",
        usage = "/luabroadcast [note]",
        summary = "Broadcast a multiplayer Lua test event.",
        adminOnly = true,
    },
    {
        id = "luasend",
        category = "Lua",
        usage = "/luasend <name> [note]",
        summary = "Send a multiplayer Lua test event to one player.",
        adminOnly = true,
    },
    {
        id = "luacell",
        category = "Lua",
        usage = "/luacell [cell]",
        summary = "Broadcast a multiplayer Lua test event to one cell.",
        adminOnly = true,
    },
    {
        id = "luaping",
        category = "Lua",
        usage = "/luaping [name]",
        summary = "Ask one or all clients to send back ping test events.",
        adminOnly = true,
    },
    {
        id = "luaburst",
        category = "Lua",
        usage = "/luaburst <name> [count]",
        summary = "Ask a client to emit a burst of ping events.",
        adminOnly = true,
    },
    {
        id = "luastorage",
        category = "Lua",
        usage = "/luastorage [value]",
        summary = "Exercise the multiplayer Lua storage bridge.",
        adminOnly = true,
    },
    {
        id = "luatypes",
        category = "Lua",
        usage = "/luatypes",
        summary = "Dump multiplayer Lua type-bridge diagnostics.",
        adminOnly = true,
    },
    {
        id = "helpmenu",
        category = "General",
        usage = "/helpmenu",
        summary = "Open the in-game multiplayer help menu.",
    },
    {
        id = "kick",
        category = "Admin",
        usage = "/kick <name>",
        summary = "Disconnect a player from the server.",
        adminOnly = true,
    },
    {
        id = "placeat",
        category = "Admin",
        usage = "/placeat <refId|\"ref id\"> [count] [distance] [direction]",
        summary = "Place a server-authoritative object in your current cell.",
        adminOnly = true,
    },
    {
        id = "delete",
        category = "Admin",
        usage = "/delete <mpNum> [cell]",
        summary = "Delete a server-authoritative placed object or spawned actor.",
        adminOnly = true,
    },
    {
        id = "resetcell",
        category = "Admin",
        usage = "/resetcell [cell]",
        summary = "Clear server test state for a cell; relog or reload before retesting.",
        adminOnly = true,
    },
    {
        id = "mpwhere",
        category = "Admin",
        usage = "/mpwhere <mpNum>",
        summary = "Show the current server-tracked location for an actor or object mpNum.",
        adminOnly = true,
    },
    {
        id = "tomp",
        category = "Admin",
        usage = "/tomp <mpNum>",
        summary = "Teleport to a server-tracked actor or object mpNum.",
        adminOnly = true,
    },
    {
        id = "tpto",
        category = "Admin",
        usage = "/tpto <pid|name>",
        summary = "Teleport yourself to an online player.",
        adminOnly = true,
    },
    {
        id = "tp",
        category = "Admin",
        usage = "/tp <pid|name>",
        summary = "Teleport an online player to you.",
        adminOnly = true,
    },
    {
        id = "bringmp",
        category = "Admin",
        usage = "/bringmp <mpNum> [distance] [direction]",
        summary = "Respawn a live server-spawned actor near you with the same mpNum.",
        adminOnly = true,
    },
    {
        id = "spawnat",
        category = "Admin",
        usage = "/spawnat <refId|\"ref id\"> [distance] [direction] [refNum] [mpNum] [persistent|session]",
        summary = "Spawn a server-authoritative actor in your current cell.",
        adminOnly = true,
    },
    {
        id = "spawner",
        category = "Admin",
        usage = "/spawner create|count|move|list|info|reset|remove ...",
        summary = "Manage destructible actor-backed spawners.",
        adminOnly = true,
    },
    {
        id = "settime",
        category = "Admin",
        usage = "/settime <hour>",
        summary = "Set the shared world time.",
        adminOnly = true,
    },
}

local function formatUsage(prefix, usage)
    if prefix and prefix ~= "/" and usage:sub(1, 1) == "/" then
        return prefix .. usage:sub(2)
    end
    return usage
end

function M.getCatalog(prefix, includeAdmin)
    local catalog = {}

    for _, entry in ipairs(COMMANDS) do
        if includeAdmin or not entry.adminOnly then
            catalog[#catalog + 1] = {
                id = entry.id,
                category = entry.category,
                usage = formatUsage(prefix or "/", entry.usage),
                summary = entry.summary,
                adminOnly = entry.adminOnly == true,
            }
        end
    end

    return catalog
end

function M.getSections(prefix, includeAdmin)
    local grouped = {}
    local indexByCategory = {}

    for _, entry in ipairs(M.getCatalog(prefix, includeAdmin)) do
        local sectionIndex = indexByCategory[entry.category]
        if not sectionIndex then
            sectionIndex = #grouped + 1
            indexByCategory[entry.category] = sectionIndex
            grouped[sectionIndex] = {
                category = entry.category,
                entries = {},
            }
        end

        grouped[sectionIndex].entries[#grouped[sectionIndex].entries + 1] = entry
    end

    table.sort(grouped, function(left, right)
        local leftIndex = #CATEGORY_ORDER + 1
        local rightIndex = #CATEGORY_ORDER + 1

        for index, name in ipairs(CATEGORY_ORDER) do
            if name == left.category then
                leftIndex = index
            end
            if name == right.category then
                rightIndex = index
            end
        end

        if leftIndex ~= rightIndex then
            return leftIndex < rightIndex
        end

        return left.category < right.category
    end)

    return grouped
end

function M.sendHelp(player, prefix, includeAdmin)
    for _, section in ipairs(M.getSections(prefix, includeAdmin)) do
        local usages = {}
        for _, entry in ipairs(section.entries) do
            usages[#usages + 1] = entry.usage
        end
        player:sendMessage(string.format("%s: %s", section.category, table.concat(usages, "  ")))
    end
end

return M
