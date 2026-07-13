local commandFactory = require("race_speech_commands")
local speechData = require("starwind_speech_data")

return commandFactory.new({
    data = speechData,
    commands = { "swspeech", "swvoice", "swv" },
    helpCommands = { "swspeechhelp", "swvoicehelp" },
    label = "Starwind speech",
    usageCommand = "/swspeech",
    logTag = "starwind-speech",
})
