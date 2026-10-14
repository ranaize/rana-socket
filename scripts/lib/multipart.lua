-- scripts/lib/multipart.lua
--
-- Build a multipart/form-data body. The C bridge's single-field
-- http_post_multipart was removed (it could not send the model + file pair the
-- OpenAI-compatible STT endpoint needs), so the body is assembled here and sent
-- with the generic rana.http_post.
local M = {}

-- build(parts, boundary) -> body, content_type
-- `parts` is an array of either
--   { name = "...", value = "..." }                       (plain field)
--   { name = "...", filename = "...", content_type = "...", value = "..." }
-- (both value/name are concatenated verbatim, so binary values are safe).
function M.build(parts, boundary)
    boundary = boundary or "----ranaBoundary7Q4k9x"
    local out = {}
    for _, p in ipairs(parts) do
        out[#out + 1] = "--" .. boundary .. "\r\n"
        if p.filename then
            out[#out + 1] = "Content-Disposition: form-data; name=\"" .. p.name ..
                            "\"; filename=\"" .. p.filename .. "\"\r\n"
            out[#out + 1] = "Content-Type: " .. (p.content_type or "application/octet-stream") ..
                            "\r\n\r\n"
        else
            out[#out + 1] = "Content-Disposition: form-data; name=\"" .. p.name .. "\"\r\n\r\n"
        end
        out[#out + 1] = p.value .. "\r\n"
    end
    out[#out + 1] = "--" .. boundary .. "--\r\n"
    return table.concat(out), "multipart/form-data; boundary=" .. boundary
end

return M
