project "Reflector"
	kind "ConsoleApp"
	targetsuffix ""

	configmap
	{
		["Debug"] 		= "Development",
	}

	prebuildcommands 
	{
		LuminaConfig.MakeDirectory(LuminaConfig.GetTargetDirectory()),
		LuminaConfig.CopyFile(LuminaConfig.EnginePath("External/LLVM/bin/libclang.dll"), LuminaConfig.GetTargetDirectory())
	}

	libdirs
	{
		LuminaConfig.EnginePath("External/LLVM/Lib"),
		LuminaConfig.EnginePath("External/LLVM/bin"),
	}

	linkoptions
	{
		"/NODEFAULTLIB:MSVCRTD",
	}

	links
	{
        "XXHash",
		"NlohmannJson",
	  	"clangBasic",
	  	"clangLex",
	  	"clangAST",
	  	"libclang",
	  	"LLVMAnalysis",
	  	"LLVMBinaryFormat",
	  	"LLVMBitReader",
	  	"LLVMBitstreamReader",
	  	"LLVMDemangle",
	  	"LLVMFrontendOffloading",
	  	"LLVMFrontendOpenMP",
	  	"LLVMMC",
	  	"LLVMProfileData",
	  	"LLVMRemarks",
	  	"LLVMScalarOpts",
	  	"LLVMTargetParser",
	  	"LLVMTransformUtils",
	  	"LLVMCore",
        "LLVMSupport",
	}

	files
	{
		LuminaConfig.ThirdPartyPath("EA/**.h"),
		LuminaConfig.ThirdPartyPath("EA/**.cpp"),
		"Source/**.cpp",
		"Source/**.h",
		"**.lua",
	}

	includedirs
	{ 
		"Source",
		LuminaConfig.EnginePath("External/LLVM/include/"),
		LuminaConfig.ThirdPartyPath("xxhash"),
		LuminaConfig.ThirdPartyPath("json"),
		LuminaConfig.ThirdPartyPath("EA/EASTL/include"),
		LuminaConfig.ThirdPartyPath("EA/EABase/include/Common"),
	}

