-- Shared premake5 helpers.
--
-- dofile()d once by the root premake5.lua so every subproject (and future
-- dependency build script) can reuse them. Keep this file free of any
-- project()/filter()/configuration state — it only defines plain functions.
--
-- Why not premake's built-in os.findlib()? That resolves *dynamic* names only
-- (lib%s.so / %s.so / .dylib / .dll). Several of our deps are linked from
-- static archives (libssl.a, libcrypto.a) to avoid shipping a runtime .so and
-- the trixie-build -> bookworm-run ABI mismatch, so we probe filenames
-- ourselves. The search-then-first-hit shape is the same as os.findlib() and
-- predep's cache://bin -> PATH binary resolver.

-- Directories probed, in order, when resolving a library by filename.
RANA_LIB_DIRS = {
	"/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib", "/usr/local/lib",
	"/usr/lib/aarch64-linux-gnu", "/usr/lib/arm-linux-gnueabihf",
}

-- Full path to the first occurrence of `name` (e.g. "libssl.a") in `dirs`
-- (defaults to RANA_LIB_DIRS), or nil. os.isfile is premake's file test.
function find_lib_file(name, dirs)
	dirs = dirs or RANA_LIB_DIRS
	for _, d in ipairs(dirs) do
		local p = d .. "/" .. name
		if os.isfile(p) then return p end
	end
	return nil
end
