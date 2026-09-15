-- rana-serializer: codec/serialization library.
--
-- A single static lib that owns every trust-boundary (de)serialization concern
-- the daemon and clients rely on, so the core stays free of codecs and JSON
-- parsing:
--   * rana::serializer::audio  — audio decode (pcm16 <-> wav)
--   * rana::serializer::json   — LLM JSON -> rana::AskReply FlatBuffer
--
-- The matching rana-serializer CLI (separate project below) wraps the json
-- serializer so scripts (e.g. rana-ask.sh) can produce AskReply buffers without
-- the daemon ever touching JSON.

project "rana-serializer"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location (ROOT)
    targetdir (ROOT .. "/serializer/lib/%{cfg.buildcfg}")
    objdir  (ROOT .. "/serializer/obj/%{cfg.buildcfg}")

    files {
        ROOT .. "/serializer/src/audio.cpp",
        ROOT .. "/serializer/src/audio.hpp",
        ROOT .. "/serializer/src/json.cpp",
        ROOT .. "/serializer/src/json.hpp"
    }

    includedirs {
        ROOT .. "/serializer/src",
        gen_dir,
        fb_include
    }

    prebuildcommands {
        "mkdir -p " .. gen_dir,
        "flatc --cpp -o " .. gen_dir .. " " .. ROOT .. "/schema/ask_reply.fbs"
    }

    filter "configurations:Release"
        -- Static build for portable single-binary deployment
        linkoptions { "-static", "-static-libgcc", "-static-libstdc++" }
        buildoptions { "-ffunction-sections", "-fdata-sections" }
        linkoptions { "-Wl,--gc-sections" }

    filter {}

-- rana-serializer CLI: serialization helper used by the ask hop.
-- Reads assistant content JSON on stdin, emits a binary rana::AskReply
-- FlatBuffer on stdout (replacing the old rana-ask-encode tool).
project "rana-serializer-cli"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    targetname "rana-serializer"
    location (ROOT)
    targetdir (ROOT .. "/serializer/bin/%{cfg.buildcfg}")
    objdir  (ROOT .. "/serializer/obj/%{cfg.buildcfg}")

    files {
        ROOT .. "/serializer/src/cli.cpp"
    }

    includedirs {
        ROOT .. "/serializer/src",
        gen_dir,
        fb_include
    }

    links { "rana-serializer", "pthread" }

    prebuildcommands {
        "mkdir -p " .. gen_dir,
        "flatc --cpp -o " .. gen_dir .. " " .. ROOT .. "/schema/ask_reply.fbs"
    }

    filter "configurations:Release"
        -- Static build for portable single-binary deployment
        linkoptions { "-static", "-static-libgcc", "-static-libstdc++" }
        buildoptions { "-ffunction-sections", "-fdata-sections" }
        linkoptions { "-Wl,--gc-sections" }

    filter {}
