-- Workspace-scoped build settings shared by engine and game workspaces.
-- ABI-affecting defines (LUA_*TAG limits, RMLUI_STATIC_LIB, NOMINMAX) must live here and nowhere else.

assert(LuminaConfig, "Workspace.lua must be included after BuildScripts/Dependencies")

function LuminaWorkspaceSettings(Opts)
    assert(Opts and Opts.Name, "LuminaWorkspaceSettings: Name is required")

    workspace(Opts.Name)
        language "C++"
        cppdialect "C++latest"
        architecture "x86_64"
        warnings "Default"
        targetdir (Opts.TargetDir or LuminaConfig.GetTargetDirectory())
        objdir (Opts.ObjDir or LuminaConfig.GetObjDirectory())
        enableunitybuild "Off"
        fastuptodate "On"
        multiprocessorcompile "On"
        -- AVX not AVX2 (AVX2 crashes on older CPUs); keep in sync with /arch, __AVX__, and GameProject/Module vectorextensions.
        vectorextensions "AVX"

        if Opts.StartProject then
            startproject(Opts.StartProject)
        end

        -- First-listed configuration is the .sln default.
        configurations(Opts.Configurations or { "Debug", "Development", "Shipping" })
        platforms { "Editor", "Game" }
        defaultplatform "Editor"

        filter "configurations:Debug or Development"
            defines
            {
                "JPH_FLOATING_POINT_EXCEPTIONS_ENABLED",
                "JPH_EXTERNAL_PROFILE",
                "JPH_ENABLE_ASSERTS",
                "LUAI_GCMETRICS",
            }
        filter {}

        defines
        {
            "JPH_OBJECT_LAYER_BITS=32",
            "JPH_DEBUG_RENDERER",
            "EASTL_USER_DEFINED_ALLOCATOR=1",
            "_CRT_SECURE_NO_WARNINGS",
            "_SILENCE_CXX23_ALIGNED_UNION_DEPRECATION_WARNING",
            "_SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING",
            "IMGUI_DEFINE_MATH_OPERATORS",
            "IMGUI_IMPL_VULKAN_USE_VOLK",

            "RMLUI_STATIC_LIB",

            "LUA_VECTOR_SIZE=4",
            "LUA_UTAG_LIMIT=2000",
            "LUA_LUTAG_LIMIT=2000",

            -- Tracy/Validation/Aftermath defines are applied per-config below, not here.

            'LUMINA_SYSTEM_NAME=\"%{LuminaConfig.GetSystem()}\"',
            'LUMINA_ARCH_NAME=\"%{LuminaConfig.GetArchitecture()}\"',
            'LUMINA_CONFIGURATION_NAME=\"%{cfg.buildcfg}\"',
            'LUMINA_PLATFORM_NAME=\"%{LuminaConfig.GetSystem()}%{LuminaConfig.GetArchitecture()}\"',
            'LUMINA_SHAREDLIB_EXT_NAME=\"%{LuminaConfig.GetSharedLibExtName()}\"',
        }

        disablewarnings
        {
            "4251", -- DLL-interface
            "4275", -- non-DLL-interface base class
            "4244", -- precision loss
            "4267", -- precision loss
        }

        filter "kind:SharedLib"
            defines { "%{prj.name:upper()}_EXPORTS" }
        filter {}

        filter "architecture:64"
            defines { "LUMINA_PLATFORM_CPU_X86_64" }

        filter "system:windows"
            systemversion "latest"
            conformancemode "On"
            defines
            {
                "LE_PLATFORM_WINDOWS",
                "DLL_EXPORT=__declspec(dllexport)",
                "DLL_IMPORT=__declspec(dllimport)",
                "__AVX__",
                "NOMINMAX",
                "WIN32_LEAN_AND_MEAN",
            }
            buildoptions
            {
                "/arch:AVX",
                "/Zc:preprocessor",
                "/Zc:inline",
                "/Zc:__cplusplus",
                "/bigobj",
            }
        filter {}

        filter "platforms:Game"
            defines { "WITH_EDITOR=0" }
        filter "platforms:Editor"
            defines { "WITH_EDITOR=1" }
        filter {}

        filter "configurations:Debug"
            targetsuffix "-Debug"
            linktimeoptimization "Off"
            incrementallink "On"
            optimize "Off"
            symbols "On"
            -- c7 (/Z7) embeds debug info in the .obj (no mspdbsrv serialization under /MP).
            debugformat "c7"
            editandcontinue "Off"
            runtime "Debug"
            defines { "LE_DEBUG", "LUMINA_DEBUG", "_DEBUG", "DEBUG" }

        filter "configurations:Development"
            targetsuffix "-Development"
            optimize "Speed"
            symbols "On"
            runtime "Release"
            -- Keep incremental link; LTO makes every relink a whole-program codegen pass.
            linktimeoptimization "Off"
            incrementallink "On"
            debugformat "c7"
            editandcontinue "Off"
            defines { "NDEBUG", "LE_DEVELOPMENT", "LUMINA_DEVELOPMENT" }

        filter { "configurations:Development", "kind:not StaticLib" }
            linkoptions { "/OPT:NOICF", "/OPT:NOREF" }

        filter "configurations:Shipping"
            targetsuffix "-Shipping"
            linktimeoptimization "On"
            optimize "Full"
            symbols "Off"
            runtime "Release"
            incrementallink "Off"
            -- /Gy: function-level COMDATs so /OPT:REF can strip unused functions.
            buildoptions { "/Gy" }
            -- LUMINA_MONOLITHIC strips API dllexport/import and makes IMPLEMENT_MODULE register into a static table; pairs with the SharedLib->StaticLib flip.
            defines { "NDEBUG", "LE_SHIPPING", "LUMINA_SHIPPING", "LUMINA_MONOLITHIC" }
            removedefines { "JPH_DEBUG_RENDERER" }

        -- /OPT:REF drops unreferenced code/data, /OPT:ICF folds identical COMDATs; scope off StaticLib to avoid lib.exe LNK4044.
        filter { "configurations:Shipping", "kind:not StaticLib" }
            linkoptions { "/OPT:REF", "/OPT:ICF" }

        filter {}

        -- Optional-feature defines, added only in configs where each is active (see BuildConfig.lua).
        ApplyLuminaFeatureDefines()
end


-- Apply each optional feature's defines only in the configs LuminaOptions marks active.
function ApplyLuminaFeatureDefines()
    local function Feature(name, defs)
        local fstr = LuminaOptions.FilterFor(name)
        if not fstr then return end
        filter(fstr)
            defines(defs)
        filter {}
    end

    -- Tracy profiler.
    Feature("Tracy",
    {
        "LUMINA_WITH_TRACY",
        "TRACY_ENABLE",
        "TRACY_CALLSTACK",
        "TRACY_ON_DEMAND",
        -- TRACY_FIBERS: scheduler brackets fiber switches with TracyFiberEnter/Leave or zones double-end.
        "TRACY_FIBERS",
        "TRACY_ALLOW_SHADOW_WARNING",
        "RMLUI_TRACY_PROFILING",
    })

    -- Vulkan validation/synchronization layers.
    Feature("Validation", { "LUMINA_WITH_VALIDATION" })

    -- NVIDIA Aftermath crash dumps; import lib + DLL copy wired by Options.LinkAftermath.
    Feature("Aftermath", { "WITH_AFTERMATH" })

    -- Verbose logging; when inactive LOG_TRACE/DEBUG/INFO compile to nothing (see Log.h).
    Feature("VerboseLogging", { "LUMINA_VERBOSE_LOGGING" })
end
