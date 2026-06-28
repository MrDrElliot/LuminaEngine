project "BuildScripts"
	kind "Utility"

	files
	{
		"**.lua",
		"**.bat",
		"../premake5.lua",
		"../Setup.bat",
		"../GenerateProjectFiles.bat",
	}

	vpaths
	{
		["Lua"]   = { "**.lua", "../premake5.lua" },
		["Batch"] = { "**.bat", "../*.bat" },
	}
