-- scripts/lib/fuzzy.lua
--
-- The lexical pre-router (ported from the old executor.cpp pre-router). Routing
-- happens lexically first, then the LLM: a confident keyword hit never costs a
-- model call. Function words that carry no routing intent ("the", "and", "put",
-- "turn", ...) appear in nearly every description, so including them in the
-- overlap score produces false matches against unrelated queries — stripping
-- them is what keeps "open browser" and "please open the browser" from
-- collapsing every command together.
local rana = require("rana")

local M = {}

local STOPWORDS = {}
for w in ([[the a an of to on in for with and or is are be am my your our
their please can could would should will you i it this that these those what
who how why when where do does did have has some any into from at by as so if
then about me we they he she turn set make get got close just now]]):gmatch("%a+") do
    STOPWORDS[w] = true
end

-- Scripts the fuzzy router must never route TO (the ask chain itself).
local SKIP = { ask = true, speak = true, talk = true, stt = true }

local function tokenize(s)
    local out = {}
    for w in (s or ""):lower():gmatch("%a+") do out[#out + 1] = w end
    return out
end

local function drop_stopwords(s)
    local out = {}
    for _, w in ipairs(tokenize(s)) do
        if not STOPWORDS[w] then out[#out + 1] = w end
    end
    return out
end

-- Meaningful-token phrase, single-spaced (exact-match form).
local function norm_meaningful(s)
    return table.concat(drop_stopwords(s), " ")
end

local function overlap_count(haystack, needles)
    local h = {}
    for _, w in ipairs(haystack) do h[w] = true end
    local n = 0
    for _, w in ipairs(needles) do if h[w] then n = n + 1 end end
    return n
end

-- Best-matching script KEY for a transcript, or nil. `danger` scripts (power)
-- only match on an EXACT keyword phrase, so "shutdown the minecraft server" can
-- never collapse onto a machine-power command.
function M.best_command(text)
    local qt = drop_stopwords(text)
    if #qt == 0 then return nil end
    local qnorm = norm_meaningful(text)
    local metas = rana.script_metas() or {}

    -- Danger commands: exact phrase only.
    for key, m in pairs(metas) do
        if not SKIP[key] and m.danger then
            for _, kw in ipairs(m.keywords or {}) do
                if norm_meaningful(kw) == qnorm then return key end
            end
        end
    end

    -- Everything else: best meaningful-token overlap with description+keywords.
    -- Need at least one overlapping token, and roughly half the query's
    -- meaningful tokens, so a single common word can't false-route.
    local need = #qt <= 2 and 1 or math.floor((#qt + 1) / 2)
    local best, best_score
    for key, m in pairs(metas) do
        if not SKIP[key] and not m.danger then
            local corpus = drop_stopwords(m.description or "")
            for _, kw in ipairs(m.keywords or {}) do
                for _, tok in ipairs(drop_stopwords(kw)) do corpus[#corpus + 1] = tok end
            end
            if #corpus > 0 then
                local ov = overlap_count(corpus, qt)
                if ov >= need and ov > (best_score or 0) then
                    best, best_score = key, ov
                end
            end
        end
    end
    return best
end

function M.extract_url(text)
    local url = string.match(text, "https?://[%w%.%-%_~/:%?%%&=+#@]+")
    if url then return url end
    return string.match(text, "www%.[%w%.%-]+")
end

-- Fuzzy hit -> {action, payload} with the script-specific operand extracted from
-- the spoken words. Multi-verb scripts (power/mc_server/media/lights/volume) get
-- the raw text and infer the verb themselves; the single-verb ones need the URL
-- / name / title pulled out.
function M.route(text)
    local key = M.best_command(text)
    if not key then return nil end
    rana.log("fuzzy", "transcript '" .. text .. "' routed to " .. key .. " (no LLM)")
    local payload
    if key == "browser" then
        payload = M.extract_url(text) or ""
    elseif key == "play_movie" then
        payload = M.extract_url(text) or text
    else
        payload = text
    end
    return { action = key, payload = payload }
end

return M
