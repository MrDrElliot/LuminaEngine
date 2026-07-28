-- Compiled library, NOT header-only. Header-only spdlog pulled fmt's format-inl.h and
-- spdlog's *-inl.h into every TU that reached Log.h (~380 of them, ~0.8s frontend each).
-- SPDLOG_COMPILED_LIB (public define, see BuildScripts/ThirdParty.lua) drops that to ~0.07s.
project "SPDLog"
	kind "StaticLib"
	warnings "off"

	defines { "SPDLOG_COMPILED_LIB" }

	includedirs { "include" }

	files
	{
		"include/**.h",
		"src/**.cpp",
		"**.lua",
	}

	filter "configurations:Debug"
		editandcontinue "Off"
	filter {}
