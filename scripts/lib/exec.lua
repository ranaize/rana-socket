-- scripts/lib/exec.lua
--
-- One ergonomic wrapper over rana.spawn, so command scripts never touch the raw
-- result table (and can never index a nil). The bridge guarantees a table —
-- { ok, code, out, err, reason } — but keeping a single choke point means the
-- scripts don't depend on that shape at all.
--
-- Errors are values, not exceptions (the pcall idiom):
--
--   local ok, res = exec.run("espeak", text)
--   if not ok then return result.error(res) end   -- res is a message string
--   ... res.out, res.code ...                     -- res is the spawn table
local rana = require("rana")

local M = {}

-- Plain-language gloss for each C-side failure reason, so callers get a
-- readable message without knowing the bridge's vocabulary.
local REASON_TEXT = {
    not_allowed  = "is not permitted",
    bad_args     = "got invalid arguments",
    spawn_failed = "could not be started",
    timeout      = "timed out",
    exit         = "failed",
}

local function trim(s)
    return (s or ""):gsub("^%s+", ""):gsub("%s+$", "")
end

-- run(name, ...) -> (true, result) on success, else (false, message).
-- `result` is the raw { ok, code, out, err, reason } table; `message` is a
-- ready-to-return error string naming the binary.
function M.run(name, ...)
    local r = rana.spawn(name, ...)
    if type(r) ~= "table" then
        -- Defensive: the bridge always returns a table, but if that contract
        -- ever regresses we degrade to an error value instead of crashing.
        return false, "internal error: no result from " .. tostring(name)
    end
    if r.ok then
        return true, r
    end
    local detail = trim(r.err)
    local what = REASON_TEXT[r.reason] or "failed"
    if detail == "" then
        return false, "`" .. tostring(name) .. "` " .. what
    end
    return false, "`" .. tostring(name) .. "` " .. what .. ": " .. detail
end

return M
