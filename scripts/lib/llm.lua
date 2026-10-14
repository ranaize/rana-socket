-- scripts/lib/llm.lua
--
-- The LLM ask hop: fuzzy-route first, otherwise call the configured model and
-- coerce its reply into an {action, payload} intent.
local rana = require("rana")
local fuzzy = require("lib.fuzzy")

local M = {}

-- Keep the LAST well-formed JSON object in the reply. Weak models echo the
-- prompt or repeat their answer; the routing object is flat, so scan every
-- non-greedy {..} fragment and keep the last one that decodes.
local function extract_json(text)
    if not text then return nil end
    local best, i0 = nil, 1
    while true do
        local s, e = string.find(text, "{.-}", i0)
        if not s then break end
        local j, _ = rana.json_decode(string.sub(text, s, e))
        if j then best = j end
        i0 = e + 1
    end
    return best
end

-- Accept both schemas the model may drift across: {command, text} and
-- {action, payload/target}. The fallback NEVER reads the model's rambling back
-- to the user (that is how JSON/garbage got spoken): long or chatty output
-- (echoed routing prose) collapses to a short generic reply.
function M.parse_action(content)
    local j = extract_json(content)
    if j then
        local cmd = j.command or j.action
        local payload = j.text or j.payload or j.target
        if cmd and payload and payload ~= "" then
            return tostring(cmd), tostring(payload)
        end
    end
    local t = content or ""
    while true do
        local s, e = string.find(t, "{.-}")
        if not s then break end
        t = t:sub(1, s - 1) .. t:sub(e + 1)
    end
    t = t:gsub("%(%w+%)", "")                 -- drop "(question)"-style labels
    t = t:gsub("%s+", " "):gsub("^%s+", ""):gsub("%s+$", "")
    if #t > 200 or t:find("action is", 1, true) or t:find("with target", 1, true) then
        return "reply", "I did not understand that."
    end
    if t == "" then t = "I did not catch that." end
    return "reply", t
end

-- ask(text) -> {action, payload}. Fuzzy pre-route skips the LLM pass entirely
-- for a confident lexical match; on any failure the intent degrades to a spoken
-- "reply" rather than an error.
function M.ask(text)
    local routed = fuzzy.route(text)
    if routed then return routed end

    local url = rana.get_llm_url()
    if url == "" then
        return { action = "reply", payload = "LLM endpoint is not configured." }
    end

    local ok, body = rana.http_post(url, {
        model = rana.get_llm_model(),
        messages = {
            { role = "system", content = rana.get_system_prompt() },
            { role = "user",   content = text },
        },
        temperature = 0.2,
        max_tokens = 256,
    })
    if not ok then
        return { action = "reply", payload = "Sorry, I could not reach the language model." }
    end

    local j = rana.json_decode(body)
    local content = j and j.choices and j.choices[1] and j.choices[1].message
                   and j.choices[1].message.content or ""
    if content == "" then
        content = j and j.text or ""
    end

    local action, payload = M.parse_action(content)
    return { action = action, payload = payload }
end

return M
