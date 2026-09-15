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
-- top-level Makefile expects them (rana-socketd.make, rana-socket-client.make,
-- rana-serializer.make, rana-serializer-cli.make).
ROOT = path.getdirectory(_SCRIPT)

schema_file     = ROOT .. "/schema/command.fbs"
gen_dir         = ROOT .. "/schema/generated"
client_gen_dir  = ROOT .. "/schema/generated/client"
fb_include      = ROOT .. "/vendor/flatbuffers/flatbuffers-24.3.25/include"

-- Regenerate the daemon's C++/Python bindings whenever the schema changes.
-- Only the daemon writes into gen_dir; the CLI client regenerates its own
-- copy into client_gen_dir so the two projects never write the same header
-- concurrently (and race) under `make -j`.
function gen_schema_daemon()
    prebuildcommands {
        "mkdir -p " .. gen_dir,
        "flatc --cpp -o " .. gen_dir .. " " .. schema_file,
        "flatc --gen-onefile --python -o " .. gen_dir .. " " .. schema_file,
        "flatc --cpp -o " .. gen_dir .. " " .. ROOT .. "/schema/ask_reply.fbs"
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
include(ROOT .. "/serializer/premake5.lua")
