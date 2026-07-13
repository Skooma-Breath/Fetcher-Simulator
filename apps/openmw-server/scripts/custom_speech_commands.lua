local commandFactory = require("race_speech_commands")
local speechData = require("custom_speech_data")

return commandFactory.new({
    data = speechData,
    commands = { "customspeech", "customvoice", "cv" },
    helpCommands = { "customspeechhelp", "customvoicehelp" },
    label = "Custom-race speech",
    usageCommand = "/customspeech",
    logTag = "custom-race-speech",
})
