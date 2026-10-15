-- scripts/lib/route.lua
--
-- The shared routing tail for the ask chain (ask.pluto and talk.pluto both end
-- in it): turn an {action, payload} intent into either a local dispatch, a
-- relay to the peer daemon (rana.respond), or an error.
--
-- rana.dispatch uses EDGE semantics: only commands allowed on THIS daemon run
-- locally; a known-but-forbidden action is relayed to the peer; an unknown
-- action is an error and is never forwarded.
--
-- Inbound gate: when the intent came from an EXTERNAL (huggingface) provider
-- (intent.external), only a script that opted in with rana.flags.EXTERNAL_EGRESS
-- may be reached; everyone else is refused via lib/refuse.lua.
local rana = require("rana")
local result = require("lib.result")
local refuse = require("lib.refuse")

local M = {}

-- The INBOUND gate. A route produced by an EXTERNAL (huggingface) answer may only
-- touch a script that opted in with rana.flags.EXTERNAL_EGRESS (off by default).
-- `intent.external` is stamped by lib/llm.lua from the provider that ANSWERED —
-- never from the reply text — so the model cannot spoof it. This is what keeps a
-- remote model from reaching, say, power.
local function opted_in(key)
    local m = (rana.script_metas() or {})[key]
    return m ~= nil and m.external_egress == true
end

-- Walk the refusal ladder (lib/refuse.lua): speak a short reason if the TTS sink
-- itself opted in, else log and drop. Never dispatches, never relays to the peer.
local function refuse_route(target)
    local reason = refuse.reason(target)
    local level = refuse.level(opted_in("speak"))
    if level == "reply" then
        return rana.respond("speak", "speak", { target = reason })
    elseif level == "log" then
        rana.log("warning", reason .. " (refused; nothing run)")
        return nil
    end
    return result.safety(reason)
end

-- Default inner action for a routed script KEY. The LLM/fuzzy emits the KEY;
-- the script's execute() checks its verb (open/toggle/play/...). Scripts not
-- listed (power, mc_server, lights, media, volume) infer the verb from the
-- payload text instead.
M.DEFAULT_ACTION = {
    browser = "open",
    speak = "speak",
    play_movie = "play",
    lookup_machine = "lookup",
}

-- route(intent) -> whatever the script's execute() should return for the
-- executor: a {status, message} table, or nothing when a relay was stashed via
-- rana.respond (nil means OK — the FORWARD_TO_CLIENT response is built from the
-- pending respond).
function M.route(intent)
    -- Already a result table (a nested hop returned one): pass it through.
    if type(intent) == "table" and intent.status ~= nil then
        return intent
    end
    if type(intent) ~= "table" or not intent.action or intent.action == "" then
        return result.error("ask returned nothing")
    end

    local action = intent.action
    local payload = tostring(intent.payload or "")
    local external = intent.external == true

    if action == "reply" then
        if payload == "" then payload = "I do not have an answer for that." end
        -- The TTS sink is itself a script, so an external answer needs speak's
        -- own opt-in before it may be spoken.
        if external and not opted_in("speak") then
            return refuse_route("speak")
        end
        -- TTS'd by the peer daemon; nothing to return (nil → OK).
        return rana.respond("speak", "speak", { target = payload })
    end

    if not rana.has_script(action) then
        return result.forbidden("unknown action: " .. action)
    end

    -- External answers may only reach a script that opted in — checked before any
    -- local dispatch or peer relay.
    if external and not opted_in(action) then
        return refuse_route(action)
    end

    local inner = M.DEFAULT_ACTION[action] or ""
    local r = rana.dispatch(action, inner, { target = payload })
    if type(r) == "table" and r.status == "forbidden" then
        -- Known script, but this daemon may not run it -> the peer's job.
        return rana.respond(action, inner, { target = payload })
    end
    return r
end

return M
