local handlers = {
    require("starwind_speech_commands"),
    require("custom_speech_commands"),
}

local M = {}

function M.handleChat(player, data, env)
    for _, handler in ipairs(handlers) do
        local handled = handler.handleChat(player, data, env)
        if handled ~= nil then
            return handled
        end
    end
    return nil
end

return M
