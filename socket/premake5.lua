-- rana-socketd: the FlatBuffers-framed command daemon.
project "rana-socketd"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    location (ROOT)
    targetdir (ROOT .. "/socket/bin/%{cfg.buildcfg}")
    objdir  (ROOT .. "/socket/obj/%{cfg.buildcfg}")

    files {
        ROOT .. "/socket/src/**.h",
        ROOT .. "/socket/src/**.cpp",
        gen_dir .. "/command_generated.h",
        gen_dir .. "/ask_reply_generated.h"
    }

    includedirs {
        ROOT .. "/socket/src",
        ROOT .. "/serializer/src",
        gen_dir,
        ROOT .. "/vendor/tomlplusplus",
        fb_include
    }

    links { "rana-serializer", "pthread" }
    gen_schema_daemon()

    filter "configurations:Release"
        -- Static build for portable single-binary deployment
        linkoptions { "-static", "-static-libgcc", "-static-libstdc++" }
        buildoptions { "-ffunction-sections", "-fdata-sections" }
        linkoptions { "-Wl,--gc-sections" }

    filter {}
