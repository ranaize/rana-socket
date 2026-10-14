-- tests/test_lib.lua
--
-- Unit tests for the shared scripts/lib/*.lua helpers. Pure Lua: a stubbed
-- `rana` bridge stands in for the C side, so this runs anywhere Lua does.
--
-- Run from the repo root:   lua tests/test_lib.lua

package.path = "scripts/?.lua;" .. package.path

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
    get_llm_url = function() return "" end,
    get_llm_model = function() return "" end,
    get_stt_url = function() return "" end,
    get_stt_model = function() return "" end,
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
bridge.get_llm_url = function() return "" end
local ask2 = llm.ask("what is the meaning of life")
check(ask2.action == "reply", "llm.ask unconfigured endpoint -> reply")

if failures > 0 then
    io.stderr:write("test_lib: " .. failures .. " check(s) failed\n")
    os.exit(1)
end
print("test_lib: OK")
