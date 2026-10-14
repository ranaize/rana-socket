-- tests/check_scripts.lua
--
-- Load-check every command script and the startup init.lua under a stubbed
-- `rana` bridge. The daemon scans scripts/ at startup and (today) hard-fails the
-- whole process on one malformed script, so the cheapest guard is to load each
-- one here: every scripts/*.pluto must return a module table carrying a `meta`
-- table and an `execute` function.
--
-- Run from the repo root:   lua tests/check_scripts.lua

-- Resolve lib/ helpers the same way the bridge's restricted searcher does.
package.path = "scripts/?.lua;" .. package.path

local function stub_rana()
    local noop = function() return true end
    return {
        flags = { LONG_IDLE = 1 },
        log = function() end,
        audio = { decode_to_wav = function() return "" end },
        allow_binaries = noop,
        allow_endpoints = noop,
        spawn = function()
            return { ok = false, code = -1, out = "", err = "", reason = "not_allowed" }
        end,
        http_post = function() return false, "" end,
        json_decode = function() return nil end,
        get_system_prompt = function() return "" end,
        get_llm_url = function() return "" end,
        get_llm_model = function() return "" end,
        get_stt_url = function() return "" end,
        get_stt_model = function() return "" end,
        dispatch = function() return { status = "ok", message = "" } end,
        has_script = function() return false end,
        script_metas = function() return {} end,
        respond = function() end,
    }
end

local bridge = stub_rana()
_G.rana = bridge
package.loaded["rana"] = bridge

local failures = 0
local function fail(msg)
    failures = failures + 1
    io.stderr:write("FAIL: " .. msg .. "\n")
end

-- init.lua: must load and run (it only registers the binary/endpoint allowlists).
do
    local chunk, err = loadfile("scripts/init.lua")
    if not chunk then
        fail("scripts/init.lua: " .. tostring(err))
    else
        local ok, e = pcall(chunk)
        if not ok then fail("scripts/init.lua: " .. tostring(e)) end
    end
end

-- Command scripts: each must return { meta = {...}, execute = function }.
local function script_files()
    local out = {}
    local p = io.popen("ls scripts/*.pluto 2>/dev/null")
    if p then
        for line in p:lines() do out[#out + 1] = line end
        p:close()
    end
    return out
end

local count = 0
for _, path in ipairs(script_files()) do
    count = count + 1
    local chunk, err = loadfile(path)
    if not chunk then
        fail(path .. ": " .. tostring(err))
    else
        local ok, mod = pcall(chunk)
        if not ok then
            fail(path .. ": " .. tostring(mod))
        elseif type(mod) ~= "table" then
            fail(path .. ": did not return a module table")
        else
            if type(mod.meta) ~= "table" then
                fail(path .. ": missing meta table")
            elseif type(mod.meta.name) ~= "string" then
                fail(path .. ": meta.name is not a string")
            end
            if type(mod.execute) ~= "function" then
                fail(path .. ": missing execute function")
            end
        end
    end
end

if count == 0 then fail("no scripts/*.pluto found (run from the repo root)") end

if failures > 0 then
    io.stderr:write("check_scripts: " .. failures .. " check(s) failed\n")
    os.exit(1)
end
print("check_scripts: OK (" .. count .. " scripts)")
