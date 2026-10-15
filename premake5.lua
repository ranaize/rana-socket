workspace "rana-socketd"
    configurations { "Release", "Debug" }
    platforms { "x86_64" }
    startproject "rana-socketd"

    filter "configurations:Debug"
        defines { "DEBUG" }
        symbols "On"
        optimize "Off"

    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "Speed"

    filter {}

-- ── shared paths & schema plumbing ─────────────────────────────────
-- ROOT is the directory of this premake5.lua (the repo root). Every
-- subproject premake file references it so all projects emit their build
-- artifacts and generated *.make files at the root, exactly where the
-- top-level Makefile expects them (rana-socketd.make, rana-socket-client.make).
ROOT = path.getdirectory(_SCRIPT)

schema_file     = ROOT .. "/schema/command.fbs"
gen_dir         = ROOT .. "/schema/generated"
client_gen_dir  = ROOT .. "/schema/generated/client"
-- The Python client package imports `.command_generated` (a relative import), so
-- the generated module must live *inside* the package. It is intentionally not
-- tracked in git (see client/py/.gitignore); the daemon build drops a copy there
-- so the installed package is never stale, and client/py/gen_command_generated.sh
-- regenerates it standalone for Python-only work.
py_client_pkg   = ROOT .. "/client/py/rana_socket"
-- FlatBuffers runtime headers. predep no longer vendors FlatBuffers (only
-- tomlplusplus + pluto); the headers come from the system/toolchain install that
-- also provides `flatc` (Dockerfile pins flatc 25.12.19 into /usr/local/include).
-- Override with FLATBUFFERS_INCLUDE when your headers live elsewhere.
fb_include      = os.getenv("FLATBUFFERS_INCLUDE") or "/usr/local/include"

-- Shared build helpers (library probing, …) reused by every subproject.
dofile(ROOT .. "/premake/helpers.lua")

-- Regenerate the daemon's C++/Python bindings whenever the schema changes.
-- Only the daemon writes into gen_dir; the CLI client regenerates its own
-- copy into client_gen_dir so the two projects never write the same header
-- concurrently (and race) under `make -j`.
function gen_schema_daemon()
    prebuildcommands {
        "mkdir -p " .. gen_dir,
        "flatc --cpp -o " .. gen_dir .. " " .. schema_file,
        "flatc --gen-onefile --python -o " .. gen_dir .. " " .. schema_file,
        -- Keep the importable Python copy in lockstep with the C++ schema.
        "mkdir -p " .. py_client_pkg,
        "cp " .. gen_dir .. "/command_generated.py " .. py_client_pkg .. "/command_generated.py"
    }
end

function gen_schema_client()
    prebuildcommands {
        "mkdir -p " .. client_gen_dir,
        "flatc --cpp -o " .. client_gen_dir .. " " .. schema_file
    }
end

-- ── subproject build definitions ───────────────────────────────────
include(ROOT .. "/socket/premake5.lua")
include(ROOT .. "/client/cpp/premake5.lua")
