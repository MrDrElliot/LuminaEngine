-- Declares an external game project + workspace linking a pre-built engine install (shares LuminaWorkspaceSettings + third-party registry for identical preprocessor state).

assert(LuminaConfig, "GameProject.lua: include BuildScripts/Dependencies first")
include(path.join(_SCRIPT_DIR, "Workspace.lua"))
include(path.join(_SCRIPT_DIR, "PluginDiscovery.lua"))


-- Minimum third-party set a game TU links to satisfy engine template instantiations (engine headers expose more, header-only or absorbed into the DLLs).
local DefaultGameDependencies =
{
    "ImGui",
    "RPMalloc",
    "EA",
    "Tracy",
}

local Configurations = { "Debug", "Development", "Shipping" }
local Platforms      = { "Editor", "Game" }


local function Append(Dest, Src)
    for _, Value in ipairs(Src) do
        table.insert(Dest, Value)
    end
    return Dest
end


-- The engine suffixes every .lib per-config ("Runtime-Debug.lib"), so link names are emitted suffixed.
local function WithSuffix(Libs, Suffix)
    local Out = {}
    for _, Lib in ipairs(Libs) do
        table.insert(Out, Lib .. Suffix)
    end
    return Out
end


local function WithoutTracy(Libs)
    local Out = {}
    for _, Lib in ipairs(Libs) do
        if Lib ~= "Tracy" then
            table.insert(Out, Lib)
        end
    end
    return Out
end


local function SetupProject(Def)
    -- Not LuminaConfig.EngineDirectory: its "%{wks.location}" fallback resolves to the game, not the
    -- engine. The prebuild and the F5 launch both need the real env var, or to be skipped entirely.
    local EngineDir = os.getenv("LUMINA_DIR")

    project(Def.Name)
        kind "SharedLib"
        rtti "off"
        staticruntime "Off"
        vectorextensions "AVX" -- keep in sync with Workspace.lua/Jolt; AVX2 #UD-crashes on CPUs without it.
        linkoptions { "/NODEFAULTLIB:LIBCMT" }

        -- Force-include the API header first so ModuleAPI.h (RUNTIME_API/EDITOR_API) is defined.
        forceincludes { Def.Name .. "API.h" }

        -- Prebuild, in declaration order: engine libs, then reflection.
        -- "call" is required on both -- a .bat invoked without it transfers control and never returns,
        -- so whatever follows silently never runs.
        if EngineDir then
            -- Build matching-config engine libs first; avoids "Runtime-Debug.lib not found" on a first build against an unbuilt config.
            local EnsureBat = '"' .. path.translate(path.join(EngineDir, "BuildScripts", "EnsureEngineBuilt.bat"), "\\") .. '"'

            -- Can't pass $(Platform) to EnsureEngineBuilt: premake folds Editor/Game platforms into the config name, leaving $(Platform)="x64". Scope per (config, platform) and emit the literal token.
            for _, Cfg in ipairs(Configurations) do
                for _, Plat in ipairs(Platforms) do
                    filter { "configurations:" .. Cfg, "platforms:" .. Plat }
                        prebuildcommands { "call " .. EnsureBat .. " " .. Cfg .. " " .. Plat }
                end
            end
            filter {}
        end

        local FilePatterns =
        {
            "Source/**.h",
            "Source/**.cpp",
            "**.lua",
            "**.lproject",
            "**.json",
        }

        if Def.Reflection then
            enablereflection "true"
            -- Route generated C# bindings into the game script assembly (not LuminaSharp.dll), mirroring plugin routing.
            csharpbindingsdir(path.join(_MAIN_SCRIPT_DIR, "Game", "Scripts", "Generated"))
            prebuildcommands { "call \"%{wks.location}\\Tools\\ReflectionRunner.bat\"" }

            -- One generated source per unity shard.
            Append(FilePatterns, LuminaConfig.GetReflectionFiles())
        end

        Append(FilePatterns, Def.ExtraFiles or {})
        -- Each image needs one EASTLImpl.cpp copy so eastl::allocator gets the right dllimport decoration here.
        table.insert(FilePatterns, LuminaConfig.GetEASTLImplFile())
        files(FilePatterns)

        local Includes = { "Source", path.join("%{wks.location}", "Intermediates/Reflection", Def.Name) }
        Append(Includes, LuminaConfig.GetEngineRuntimeIncludes())
        Append(Includes, LuminaThirdParty.IncludesOf(LuminaThirdParty.RuntimePublicDeps))
        Append(Includes, LuminaThirdParty.IncludesOf(Def.Dependencies))
        includedirs(Includes)

        defines(Def.PrivateDefines or {})
        libdirs(Append({ LuminaConfig.GetTargetDirectory() }, Def.ExtraLibDirs or {}))

        local _, _, ThirdPartyLinks = LuminaThirdParty.Resolve(Def.Dependencies)

        local BaseLinks = { "Runtime" }
        Append(BaseLinks, Def.ModuleDependencies or {})
        Append(BaseLinks, ThirdPartyLinks)

        -- Editor platform additionally links the Editor module and exposes its headers.
        local EditorLinks = { "Editor" }
        Append(EditorLinks, Def.EditorModuleDependencies or {})

        for _, Cfg in ipairs(Configurations) do
            -- Link Tracy only where profiling is active; strip it elsewhere.
            local Links = LuminaOptions.IsActive("Tracy", Cfg) and BaseLinks or WithoutTracy(BaseLinks)

            filter { "configurations:" .. Cfg }
                links(WithSuffix(Links, "-" .. Cfg))
            filter { "configurations:" .. Cfg, "platforms:Editor" }
                links(WithSuffix(EditorLinks, "-" .. Cfg))
        end
        filter {}

        links(Def.ExtraLinks or {})

        filter "platforms:Editor"
            includedirs(Append(LuminaConfig.GetEngineEditorIncludes(),
                LuminaThirdParty.IncludesOf(LuminaThirdParty.EditorPublicDeps)))
        filter {}

        -- F5 launches the editor with this project pre-loaded; engine binaries resolved at generate time via LUMINA_DIR.
        if EngineDir then
            local EngineBin = path.join(EngineDir, "Binaries", "Windows64")

            debugdir(EngineBin)
            debugargs { "--Project=\"" .. path.join("%{wks.location}", Def.Name .. ".lproject") .. "\"" }

            for _, Cfg in ipairs(Configurations) do
                filter { "configurations:" .. Cfg }
                    debugcommand(path.join(EngineBin, "Lumina-" .. Cfg .. ".exe"))
            end
            filter {}
        end
end


---@param Def table Game project definition
function LuminaGameProject(Def)
    assert(Def.Name, "LuminaGameProject: Name is required")
    assert(LuminaConfig, "LuminaGameProject: BuildScripts/Dependencies must be included first")

    if Def.Reflection == nil then
        Def.Reflection = true
    end
    Def.Dependencies = Def.Dependencies or DefaultGameDependencies

    LuminaWorkspaceSettings({
        Name           = Def.Name,
        StartProject   = Def.Name,
        TargetDir      = path.join("%{wks.location}", "Binaries", LuminaConfig.OutputDirectory),
        ObjDir         = path.join("%{wks.location}", "Intermediates", "Obj", LuminaConfig.OutputDirectory, "%{prj.name}"),
        -- Default to Development; Debug is ~3x slower to compile and rarely needed for iteration.
        Configurations = { "Development", "Debug", "Shipping" },
    })

    SetupProject(Def)

    -- Only project-local plugins are added here; engine plugins ship pre-built in Lumina.slnx. Runtime loads both at startup.
    group "Plugins"
        LuminaDiscoverPlugins(path.join(_MAIN_SCRIPT_DIR, "Plugins"))
    group ""
end
