-- rana-socketd: the FlatBuffers-framed command daemon.
--
-- Pluto (a Lua 5.4+ fork) is vendored and built as a shared library by the
-- separate "pluto" predep make stage (libpluto.so at vendor/pluto/src). The
-- daemon links that prebuilt .so — it does NOT compile Pluto in-tree. The
-- scripts/ directory is the only place the daemon runs code from; the C++ side
-- only bridges to it via luafunctions. See docs/security.md for the runtime
-- hardening applied on top of the compile-time flags.

project("rana-socketd")
kind("ConsoleApp")
language("C++")
cppdialect("C++17")
location(ROOT)
targetdir(ROOT .. "/socket/bin/%{cfg.buildcfg}")
objdir(ROOT .. "/socket/obj/%{cfg.buildcfg}")

files({
	ROOT .. "/socket/src/**.h",
	ROOT .. "/socket/src/**.cpp",
	gen_dir .. "/command_generated.h",
	gen_dir .. "/ask_reply_generated.h",
})

includedirs({
	ROOT .. "/socket/src",
	ROOT .. "/vendor/pluto/src",
	gen_dir,
	ROOT .. "/vendor/tomlplusplus",
	fb_include,
})

-- Pluto is built by the separate "pluto" predep make stage into a shared
-- library at vendor/pluto/src/libpluto.so; link that, do not compile it.
-- Define PLUTO_ETL_ENABLE on the daemon side too so lstate.h exposes Pluto's
-- ETL 'deadline' field (gated on that macro in the shared headers); the daemon
-- then pins each script's deadline to its meta.timeout_ms in new_state().
buildoptions({ "-DPLUTO_ETL_ENABLE" })
libdirs({ ROOT .. "/vendor/pluto/src" })
links({ "pluto", "pthread", "m", "dl" })
-- Locate libpluto.so ONLY next to the binary ($ORIGIN), so the daemon keeps
-- resolving the .so regardless of where the repo is cloned/moved. The pluto
-- stage also builds a static archive (libplutostatic.a); stage BOTH next to
-- the freshly-built daemon so each build self-contains its pluto artifacts.
-- A baked-in absolute rpath would silently break on a clone.
linkoptions({ "-Wl,-rpath,$$ORIGIN" })
linkoptions({ "-Wl,-rpath,'$$ORIGIN'" })
-- Run before every daemon link; the pluto predep stage must have built the
-- artifacts first (the link below would fail if it hadn't, so ordering is
-- already guaranteed). {COPYFILE} = cp -f (cross-platform), %{cfg.targetdir}
-- expands to socket/bin/<buildcfg>.
prebuildcommands({
	"{COPYFILE} " .. ROOT .. "/vendor/pluto/src/libpluto.so %{cfg.targetdir}/libpluto.so",
	"{COPYFILE} " .. ROOT .. "/vendor/pluto/src/libplutostatic.a %{cfg.targetdir}/libplutostatic.a",
})
-- Post-build hook: ldd the freshly-linked daemon and fail the build if
-- libpluto.so would not resolve at runtime from the binary's own directory
-- (catches the "cannot open shared object file" failure at build time).
gen_schema_daemon()

filter("configurations:Release")
-- libpluto.so is linked dynamically, so we can no longer do a fully
-- static link; keep libgcc/libstdc++ static for portability.
linkoptions({ "-static-libgcc", "-static-libstdc++" })
buildoptions({ "-ffunction-sections", "-fdata-sections" })
linkoptions({ "-Wl,--gc-sections" })

filter({})
