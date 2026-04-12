local registry = {
    evaluators = {},
    validators = {},
    handlers = {},
    ops = {},
}

local function getEntries(bucket, intentName)
    local entries = bucket[intentName]
    if not entries then
        entries = {}
        bucket[intentName] = entries
    end
    return entries
end

local function appendEntry(bucket, intentName, name, fn)
    assert(type(intentName) == "string" and intentName ~= "", "expected intent name")
    assert(type(fn) == "function", "expected function")
    local entries = getEntries(bucket, intentName)
    entries[#entries + 1] = {
        name = tostring(name or ("handler_" .. tostring(#entries + 1))),
        fn = fn,
    }
    return fn
end

local function applyDecision(result, decision)
    if type(decision) ~= "table" then
        return
    end

    if decision.accepted ~= nil then
        result.accepted = decision.accepted == true
    end
    if decision.reason ~= nil then
        result.reason = tostring(decision.reason)
    end
    if decision.action ~= nil then
        result.action = tostring(decision.action)
    end
    if decision.mutation ~= nil then
        result.mutation = tostring(decision.mutation)
    end
    if decision.classificationSource ~= nil then
        result.classificationSource = tostring(decision.classificationSource)
    end
    if decision.verificationSource ~= nil then
        result.verificationSource = tostring(decision.verificationSource)
    end
    if decision.applyStandardAction ~= nil then
        result.applyStandardAction = decision.applyStandardAction == true
    end
    if decision.stop ~= nil then
        result.stopProcessing = decision.stop == true
    end
    if decision.ops ~= nil then
        result.ops = result.ops or {}
        for _, op in ipairs(decision.ops) do
            result.ops[#result.ops + 1] = op
        end
    end
end

local function failResult(result, stageName, entryName, reason, err)
    result.accepted = false
    result.reason = reason or (stageName .. "_failed")
    result.mutation = stageName .. "-failed"
    result.applyStandardAction = false
    result.failedStage = stageName
    result.failedHandler = entryName
    result.failedError = err and tostring(err) or nil
end

local function runStage(entries, stageName, context)
    for _, entry in ipairs(entries or {}) do
        local ok, decision = pcall(entry.fn, context)
        if not ok then
            failResult(context.result, stageName, entry.name, stageName .. "_error", decision)
            return false
        end

        if decision == false then
            failResult(context.result, stageName, entry.name, stageName .. "_rejected")
            return false
        end

        applyDecision(context.result, decision)
        if context.result.accepted == false then
            context.result.applyStandardAction = false
            context.result.failedStage = context.result.failedStage or stageName
            context.result.failedHandler = context.result.failedHandler or entry.name
            return false
        end

        if context.result.stopProcessing then
            return true
        end
    end

    return true
end

function registry.registerIntent(intentName, evaluator)
    assert(type(intentName) == "string" and intentName ~= "", "expected intent name")
    assert(type(evaluator) == "function", "expected evaluator")
    registry.evaluators[intentName] = evaluator
    return evaluator
end

function registry.registerValidator(intentName, name, fn)
    return appendEntry(registry.validators, intentName, name, fn)
end

function registry.registerHandler(intentName, name, fn)
    return appendEntry(registry.handlers, intentName, name, fn)
end

function registry.ops.grantInventory(guid, refId, count)
    return {
        type = "GrantInventory",
        guid = guid,
        refId = refId,
        count = count,
    }
end

function registry.ops.removePlacedObject(mpNum, cellId)
    return {
        type = "RemovePlacedObject",
        mpNum = mpNum,
        cellId = cellId,
    }
end

function registry.evaluate(intentName, data)
    local evaluator = registry.evaluators[intentName]
    if type(evaluator) ~= "function" then
        return {
            accepted = false,
            reason = "missing_intent_evaluator",
            mutation = "intent-missing",
            applyStandardAction = false,
        }
    end

    local ok, contextOrErr = pcall(evaluator, data or {})
    if not ok then
        return {
            accepted = false,
            reason = "intent_evaluator_error",
            mutation = "intent-error",
            applyStandardAction = false,
            failedStage = "evaluator",
            failedError = tostring(contextOrErr),
        }
    end

    local context = contextOrErr or {}
    local result = context.result or {}
    context.result = result

    if result.accepted == nil then
        result.accepted = true
    end
    if result.applyStandardAction == nil then
        result.applyStandardAction = result.accepted == true
    end

    if result.accepted then
        if not runStage(registry.validators[intentName], "validator", context) then
            result.stopProcessing = nil
            return result
        end
        if not result.stopProcessing and not runStage(registry.handlers[intentName], "handler", context) then
            result.stopProcessing = nil
            return result
        end
    else
        result.applyStandardAction = false
    end

    result.stopProcessing = nil
    result.intentName = intentName
    result.applyStandardAction = result.applyStandardAction == true and result.accepted == true
    return result
end

function registry.getCounts(intentName)
    if intentName then
        return {
            validators = #(registry.validators[intentName] or {}),
            handlers = #(registry.handlers[intentName] or {}),
        }
    end

    local counts = {}
    for name, _ in pairs(registry.evaluators) do
        counts[name] = registry.getCounts(name)
    end
    return counts
end

return registry
