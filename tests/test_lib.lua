-- tests/test_lib.lua
--
-- Unit tests for the shared scripts/lib/*.pluto helpers. Pure Pluto: a stubbed
-- `rana` bridge stands in for the C side, so this runs anywhere Pluto does.
--
-- Run from the repo root:   pluto tests/test_lib.lua

package.path = "scripts/?.pluto;" .. package.path

local calls = {}
local bridge = {
    flags = { LONG_IDLE = 1 },
    log = function() end,
    audio = { decode_to_wav = function() return "" end },
    allow_binaries = function() end,
    allow_endpoints = function() end,
    spawn = function()
        return { ok = false, code = -1, out = "", err = "", reason = "not_allowed" }
    end,
    http_post = function() return false, "" end,
    json_decode = function(s)
        -- Tiny flat-object decoder, enough for the parse_action fixtures below.
        if type(s) ~= "string" then return nil end
        local t = {}
        for k, v in s:gmatch('"([%w_]+)"%s*:%s*"([^"]*)"') do t[k] = v end
        if next(t) == nil then return nil end
        return t
    end,
    get_system_prompt = function() return "" end,
    providers = function() return {} end,
    provider_post = function() return false, "", 0 end,
    dispatch = function(...)
        calls[#calls + 1] = { "dispatch", ... }
        return { status = "ok", message = "" }
    end,
    has_script = function(k) return k == "browser" or k == "power" or k == "speak" end,
    script_metas = function() return {} end,
    respond = function(...) calls[#calls + 1] = { "respond", ... } end,
}
_G.rana = bridge
package.loaded["rana"] = bridge

local failures = 0
local function check(cond, msg)
    if not cond then
        failures = failures + 1
        io.stderr:write("FAIL: " .. msg .. "\n")
    end
end

-- ── verbs ─────────────────────────────────────────────────────────────────
local verbs = require("lib.verbs")
check(verbs.fold("Shut-Down  Now") == "shut down now", "verbs.fold lowercases and folds -/space")
check(verbs.fold(nil) == "", "verbs.fold tolerates nil")

local power_rules = {
    { verb = "shutdown", patterns = { "shut[%w%s]-down", "power[%w%s]-down", "shutdown", "power[%w%s]-off" } },
    { verb = "reboot",   patterns = { "reboot", "restart" } },
    { verb = "suspend",  patterns = { "suspend", "sleep" } },
}
check(verbs.infer("shut down the machine", power_rules, nil) == "shutdown", "verbs.infer -> shutdown")
check(verbs.infer("please reboot", power_rules, nil) == "reboot", "verbs.infer -> reboot")
check(verbs.infer("go to sleep", power_rules, nil) == "suspend", "verbs.infer -> suspend")
check(verbs.infer("make me a sandwich", power_rules, nil) == nil, "verbs.infer unmatched -> nil (fail closed)")
check(verbs.infer("turn it up", { { verb = "up", patterns = { "up" } } }, "get") == "up", "verbs.infer -> matched rule")
check(verbs.infer("what is it", { { verb = "up", patterns = { "up" } } }, "get") == "get", "verbs.infer -> default")
check(verbs.first_number("set it to 50 now") == "50", "verbs.first_number")
check(verbs.first_number("no digits") == nil, "verbs.first_number nil when absent")

-- ── multipart ─────────────────────────────────────────────────────────────
local multipart = require("lib.multipart")
local body, ctype = multipart.build({
    { name = "model", value = "whisper" },
    { name = "file", filename = "request.wav", content_type = "audio/wav", value = "RIFF" },
}, "BOUND")
local expected = "--BOUND\r\n"
    .. 'Content-Disposition: form-data; name="model"\r\n\r\n'
    .. "whisper\r\n"
    .. "--BOUND\r\n"
    .. 'Content-Disposition: form-data; name="file"; filename="request.wav"\r\n'
    .. "Content-Type: audio/wav\r\n\r\n"
    .. "RIFF\r\n"
    .. "--BOUND--\r\n"
check(body == expected, "multipart.build body bytes")
check(ctype == "multipart/form-data; boundary=BOUND", "multipart.build content-type")

-- ── result ────────────────────────────────────────────────────────────────
local result = require("lib.result")
check(result.ok("x").status == "ok", "result.ok")
check(result.error("x").status == "error", "result.error")
check(result.forbidden("x").status == "forbidden", "result.forbidden")
check(result.invalid("x").status == "invalid", "result.invalid")
check(result.safety("x").status == "safety", "result.safety")
check(result.error("x").message == "x", "result carries the message")

-- ── route ─────────────────────────────────────────────────────────────────
local route = require("lib.route")

-- reply -> respond("speak","speak",{target=...}); returns nothing (nil => OK).
calls = {}
local r1 = route.route({ action = "reply", payload = "hi" })
check(r1 == nil, "route.reply returns nil (respond stashed)")
check(calls[1] and calls[1][1] == "respond" and calls[1][2] == "speak" and calls[1][3] == "speak",
      "route.reply responds via speak")
check(calls[1] and type(calls[1][4]) == "table" and calls[1][4].target == "hi", "route.reply payload in target")

-- an existing result table passes straight through.
local passthru = { status = "ok", message = "m" }
check(route.route(passthru) == passthru, "route passes a result table through")

-- missing action -> execution failure.
check(route.route({}).status == "error", "route({}) -> error")

-- unknown action -> forbidden, never forwarded.
calls = {}
local r2 = route.route({ action = "nope" })
check(r2.status == "forbidden", "route.unknown -> forbidden")
check(#calls == 0, "route.unknown does not dispatch/respond")

-- known but dispatch-forbidden -> relay to the peer with the mapped default action.
calls = {}
bridge.dispatch = function(...)
    calls[#calls + 1] = { "dispatch", ... }
    return { status = "forbidden", message = "" }
end
local r3 = route.route({ action = "browser", payload = "https://example.com" })
check(r3 == nil, "route forbidden dispatch returns nil (responded)")
check(calls[1] and calls[1][1] == "dispatch" and calls[1][2] == "browser" and calls[1][3] == "open",
      "route dispatches with DEFAULT_ACTION open")
check(calls[2] and calls[2][1] == "respond" and calls[2][2] == "browser" and calls[2][3] == "open",
      "route relays the forbidden action to the peer")

-- ── route: inbound gate (intent.external) ─────────────────────────────────
-- An EXTERNAL (huggingface) answer may only reach a script that opted in with
-- rana.flags.EXTERNAL_EGRESS. Off by default.
bridge.dispatch = function(...)
    calls[#calls + 1] = { "dispatch", ... }
    return { status = "ok", message = "" }
end
bridge.script_metas = function()
    return {
        browser = { external_egress = true },
        power   = { external_egress = false },
        speak   = { external_egress = true },
    }
end

-- external + opted-in target -> dispatched normally.
calls = {}
local g1 = route.route({ action = "browser", payload = "https://x", external = true })
check(calls[1] and calls[1][1] == "dispatch" and calls[1][2] == "browser",
      "external answer reaches an opted-in script")
check(g1 and g1.status == "ok", "external/opted-in returns the dispatch result")

-- external + NOT opted-in target, but speak opted in -> spoken refusal, no dispatch.
calls = {}
local g2 = route.route({ action = "power", payload = "reboot", external = true })
check(#calls == 1 and calls[1][1] == "respond" and calls[1][2] == "speak",
      "external answer to a non-opted-in script refuses via speak")
check(g2 == nil, "spoken refusal returns nil (respond stashed)")

-- external reply with speak NOT opted in -> refused, not spoken.
bridge.script_metas = function() return { power = { external_egress = false } } end
calls = {}
local g4 = route.route({ action = "reply", payload = "hi", external = true })
check(#calls == 0, "external reply refuses when speak has not opted in")
check(g4 == nil, "refused external reply returns nil")

-- external + nothing opted in -> log and drop, no dispatch and no respond.
calls = {}
local g3 = route.route({ action = "power", payload = "reboot", external = true })
check(#calls == 0, "external/non-opted-in with no speak dispatches nothing")
check(g3 == nil, "log-level refusal returns nil")

-- external reply with speak opted in -> spoken.
bridge.script_metas = function() return { speak = { external_egress = true } } end
calls = {}
local g5 = route.route({ action = "reply", payload = "hi", external = true })
check(calls[1] and calls[1][1] == "respond" and calls[1][2] == "speak",
      "external reply speaks when speak opted in")
check(g5 == nil, "external reply returns nil")

-- a LOCAL answer (no external stamp) is never gated.
bridge.script_metas = function() return {} end
calls = {}
local g6 = route.route({ action = "browser", payload = "https://x" })
check(g6 and calls[1] and calls[1][1] == "dispatch", "local answer reaches a non-opted-in script")

-- ── fuzzy ─────────────────────────────────────────────────────────────────
local fuzzy = require("lib.fuzzy")
bridge.script_metas = function()
    return {
        browser = { description = "open a web page in the browser",
                    keywords = { "open browser", "website" }, danger = false },
        power   = { description = "control machine power",
                    keywords = { "shutdown", "reboot" }, danger = true },
        speak   = { description = "say something out loud", keywords = { "say" }, danger = false },
        ask     = { description = "ask a question", keywords = { "ask" }, danger = false },
    }
end

local f1 = fuzzy.route("open browser")
check(f1 and f1.action == "browser" and f1.payload == "", "fuzzy routes 'open browser' (browser payload is the URL, empty here)")
check(fuzzy.route("please open the browser").action == "browser", "fuzzy drops stopwords")
local f2 = fuzzy.route("shutdown")
check(f2 and f2.action == "power" and f2.payload == "shutdown", "fuzzy exact danger keyword -> power")
check(fuzzy.route("just some random words here") == nil, "fuzzy no confident match -> nil")
check(fuzzy.extract_url("see https://example.com/x?y=1 now") == "https://example.com/x?y=1",
      "fuzzy.extract_url full url")
check(fuzzy.extract_url("go to www.foo.org") == "www.foo.org", "fuzzy.extract_url www host")
check(fuzzy.extract_url("nothing here") == nil, "fuzzy.extract_url nil when absent")

-- ── llm ───────────────────────────────────────────────────────────────────
local llm = require("lib.llm")
-- parse_action accepts both schemas.
local a1, p1 = llm.parse_action('{"action":"browser","payload":"https://x"}')
check(a1 == "browser" and p1 == "https://x", "llm.parse_action {action,payload}")
local a2, p2 = llm.parse_action('{"command":"speak","text":"hello"}')
check(a2 == "speak" and p2 == "hello", "llm.parse_action {command,text}")
-- chatty non-JSON collapses to a short reply, never the rambling.
local a3, p3 = llm.parse_action("The action is open with target https://x and then blah blah")
check(a3 == "reply", "llm.parse_action chatty -> reply")
check(p3 == "I did not understand that.", "llm.parse_action chatty -> generic text")
-- empty -> generic reply.
local a4, p4 = llm.parse_action("")
check(a4 == "reply" and p4 == "I did not catch that.", "llm.parse_action empty -> generic reply")
-- ask() takes the fuzzy shortcut before touching the model.
local ask = llm.ask("open browser")
check(ask.action == "browser", "llm.ask fuzzy shortcut")
bridge.providers = function() return {} end
local ask2 = llm.ask("what is the meaning of life")
check(ask2.action == "reply", "llm.ask with no providers -> reply")
check(ask2.payload == "Sorry, I could not reach the language model.",
      "no providers -> could-not-reach reply")

-- ── llm provider chain ────────────────────────────────────────────────────
-- A recursive decoder for the nested response fixtures (the stub above only
-- parses flat string objects). Placed last so earlier fixtures are unaffected.
local function json_decode_full(s)
    if type(s) ~= "string" then return nil end
    local i, n = 1, #s
    local function skip() while i <= n and s:sub(i, i):match("%s") do i = i + 1 end end
    local parse_value
    local function parse_string()
        i = i + 1
        local out = {}
        while i <= n do
            local c = s:sub(i, i)
            if c == '"' then i = i + 1; return table.concat(out) end
            if c == "\\" then
                local e = s:sub(i + 1, i + 1)
                local map = { n = "\n", t = "\t", r = "\r", b = "\b", f = "\f",
                              ['"'] = '"', ["\\"] = "\\", ["/"] = "/" }
                out[#out + 1] = map[e] or e
                i = i + 2
            else
                out[#out + 1] = c
                i = i + 1
            end
        end
        return nil
    end
    parse_value = function()
        skip()
        local c = s:sub(i, i)
        if c == '"' then return parse_string(), true end
        if c == "{" then
            i = i + 1
            local obj = {}
            skip()
            if s:sub(i, i) == "}" then i = i + 1; return obj, true end
            while true do
                skip()
                if s:sub(i, i) ~= '"' then return nil, false end
                local k = parse_string()
                if k == nil then return nil, false end
                skip()
                if s:sub(i, i) ~= ":" then return nil, false end
                i = i + 1
                local v, ok = parse_value()
                if not ok then return nil, false end
                obj[k] = v
                skip()
                local d = s:sub(i, i)
                if d == "," then i = i + 1
                elseif d == "}" then i = i + 1; return obj, true
                else return nil, false end
            end
        end
        if c == "[" then
            i = i + 1
            local arr = {}
            skip()
            if s:sub(i, i) == "]" then i = i + 1; return arr, true end
            while true do
                local v, ok = parse_value()
                if not ok then return nil, false end
                arr[#arr + 1] = v
                skip()
                local d = s:sub(i, i)
                if d == "," then i = i + 1
                elseif d == "]" then i = i + 1; return arr, true
                else return nil, false end
            end
        end
        if c == "t" and s:sub(i, i + 3) == "true" then i = i + 4; return true, true end
        if c == "f" and s:sub(i, i + 4) == "false" then i = i + 5; return false, true end
        local num = s:sub(i):match("^%-?%d+%.?%d*[eE]?[%+%-]?%d*")
        if num and #num > 0 then i = i + #num; return tonumber(num), true end
        return nil, false
    end
    local v, ok = parse_value()
    if not ok then return nil end
    return v
end

bridge.json_decode = json_decode_full

-- One local provider that answers 200: the chain dials it exactly once.
do
    local seen = {}
    bridge.providers = function(kind)
        check(kind == "llm", "providers called with kind=llm")
        return { { type = "local", model = "command" } }
    end
    bridge.provider_post = function(kind, i, req)
        seen[#seen + 1] = i
        check(kind == "llm", "provider_post kind=llm")
        check(type(req) == "table" and req.model == "command", "body carries the model")
        return true, '{"choices":[{"message":{"content":"primary text"}}]}', 200
    end
    local r = llm.ask("what is the meaning of life")
    check(#seen == 1 and seen[1] == 1, "reachable provider dialed once")
    check(r.action == "reply" and r.payload == "primary text", "provider reply is parsed")
end

-- First provider unreachable -> advance to the next; the chain answers.
do
    local seen = {}
    bridge.providers = function()
        return {
            { type = "local", model = "command" },
            { type = "huggingface", model = "hf/model" },
        }
    end
    bridge.provider_post = function(_, i)
        seen[#seen + 1] = i
        if i == 1 then return false, "connect failed", 0 end
        return true, '{"choices":[{"message":{"content":"hello cloud"}}]}', 200
    end
    local r = llm.ask("what is the meaning of life")
    check(#seen == 2 and seen[1] == 1 and seen[2] == 2, "transport failure advances the chain")
    check(r.action == "reply" and r.payload == "hello cloud", "second provider's reply is parsed")
end

-- opts.cloud=false skips every huggingface provider for THIS call.
do
    local seen = {}
    bridge.providers = function()
        return {
            { type = "local", model = "command" },
            { type = "huggingface", model = "hf/model" },
        }
    end
    bridge.provider_post = function(_, i)
        seen[#seen + 1] = i
        return false, "connect failed", 0
    end
    local r = llm.ask("what is the meaning of life", { cloud = false })
    check(#seen == 1 and seen[1] == 1, "opts.cloud=false does not dial the huggingface provider")
    check(r.action == "reply", "opts.cloud=false still returns a reply")
end

-- A live 4xx/5xx is the upstream's own answer: the chain stops (no fallback).
do
    local seen = {}
    bridge.providers = function()
        return {
            { type = "local", model = "command" },
            { type = "huggingface", model = "hf/model" },
        }
    end
    bridge.provider_post = function(_, i)
        seen[#seen + 1] = i
        return false, "upstream exploded", 503
    end
    local r = llm.ask("what is the meaning of life")
    check(#seen == 1, "a live 5xx does not advance the chain")
    check(r.action == "reply", "a live 5xx still degrades to a reply")
end

-- Every provider unreachable -> the could-not-reach reply.
do
    bridge.providers = function()
        return {
            { type = "local", model = "command" },
            { type = "huggingface", model = "hf/model" },
        }
    end
    bridge.provider_post = function() return false, "connect failed", 0 end
    local r = llm.ask("what is the meaning of life")
    check(r.action == "reply" and r.payload == "Sorry, I could not reach the language model.",
          "all providers unreachable -> could-not-reach reply")
end

-- The ANSWERING provider's type is stamped on the intent: a local answer is not
-- external; a huggingface answer is (this is what the route gate keys on).
local browser_reply = '{"choices":[{"message":{"content":"{\\"action\\":\\"browser\\",\\"payload\\":\\"https://x\\"}"}}]}'
do
    bridge.providers = function() return { { type = "local", model = "command" } } end
    bridge.provider_post = function() return true, browser_reply, 200 end
    local r = llm.ask("what is the meaning of life")
    check(r.action == "browser" and r.external == false, "local answer is not external")
end
do
    bridge.providers = function() return { { type = "huggingface", model = "hf/model" } } end
    bridge.provider_post = function() return true, browser_reply, 200 end
    local r = llm.ask("what is the meaning of life")
    check(r.action == "browser" and r.external == true, "huggingface answer is stamped external")
end

if failures > 0 then
    io.stderr:write("test_lib: " .. failures .. " check(s) failed\n")
    os.exit(1)
end
print("test_lib: OK")
