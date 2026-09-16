-- rana-socket-client: installable CLI client for the daemon.
project "rana-socket-client"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    location (ROOT)
    targetdir (ROOT .. "/client/cpp/bin/%{cfg.buildcfg}")
    objdir  (ROOT .. "/client/cpp/obj/%{cfg.buildcfg}")

    files {
        ROOT .. "/client/cpp/src/**.h",
        ROOT .. "/client/cpp/src/**.cpp",
        client_gen_dir .. "/command_generated.h"
    }

    includedirs {
        client_gen_dir,
        ROOT .. "/client/cpp/src",
        fb_include
    }

    links { "pthread" }
    gen_schema_client()
