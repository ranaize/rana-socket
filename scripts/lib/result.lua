-- scripts/lib/result.lua
--
-- The single source of truth for what a script's execute() returns.
--
-- The C executor accepts a bare nil/true/string, or a table {status, message}
-- (see run_entrypoint's "Script return-value convention" in
-- socket/src/luafunctions.cpp). These helpers keep that shape in one place and
-- spell ONLY the status strings the bridge understands (status_from_string);
-- anything unrecognized degrades to EXECUTION_FAILED, so a typo can never
-- masquerade as success.
local M = {}

M.OK        = "ok"         -- success
M.ERROR     = "error"      -- EXECUTION_FAILED — the default for "something broke"
M.FORBIDDEN = "forbidden"  -- policy refused the command
M.INVALID   = "invalid"    -- malformed payload / params
M.SAFETY    = "safety"     -- SAFETY_GATE_FAILED — a destructive op lacked confirmation

local function make(status)
    return function(message)
        return { status = status, message = message or "" }
    end
end

M.ok        = make(M.OK)
M.error     = make(M.ERROR)
M.forbidden = make(M.FORBIDDEN)
M.invalid   = make(M.INVALID)
M.safety    = make(M.SAFETY)

return M
