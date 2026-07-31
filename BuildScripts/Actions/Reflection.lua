
-- Fall back to _MAIN_SCRIPT_DIR so fresh clones work before Setup.bat sets LUMINA_DIR.
local LuminaDir = os.getenv("LUMINA_DIR") or _MAIN_SCRIPT_DIR

include (path.join(LuminaDir, "BuildScripts/Logger"))

function Capitalize(str)
    if not str or str == "" then
        return ""
    end
    return str:sub(1, 1):upper() .. str:sub(2)
end

ArchBits =
{
    ["x86"]     = "32",
    ["x86_64"]  = "64",
    ["ARM64"]   = "ARM64"
}

premake.modules.lua = {}
local m = premake.modules.lua

local p = premake

local json = require("json")

local ProjectFiles = {}
local Workspace = {}

newaction 
{
	trigger = "Reflection",
	description = "Builds necessary reflection info and packs into a json file.",

	onStart = function()
        ProjectFiles = {}
	end,

	onWorkspace = function(wks)
        Workspace = wks
	end,

	onProject = function(prj)

        if not prj.enablereflection then
            return
        end

        ProjectFiles[prj.name] = {
            Path = prj.basedir,
            Files = {},
            IncludeDirs = {},
            CSharpBindingsDir = prj.csharpbindingsdir or ""
        }
        
        for Config in p.project.eachconfig(prj) do
            for _, IncludePath in ipairs(Config.includedirs) do
                local Path = path.getabsolute(IncludePath)
                
                if not table.contains(ProjectFiles[prj.name].IncludeDirs, Path) then
                    table.insert(ProjectFiles[prj.name].IncludeDirs, Path)
                end
            end
        end

        local Tree = p.project.getsourcetree(prj)
        local function TraverseTree(node)

            if node.abspath then
                local Ext = path.getextension(node.abspath)
                if Ext == ".h" then
                    table.insert(ProjectFiles[prj.name].Files, node.abspath)
                end
            end

            if node.children then
                for _, child in ipairs(node.children) do
                    TraverseTree(child)
                end
            end

        end

        TraverseTree(Tree)
	end,
    
    execute = function()

        local Data = {
            WorkspaceName = Workspace.name,
            WorkspacePath = _MAIN_SCRIPT_DIR,
            Projects = {}
        }
    
        for Name, ProjectData in pairs(ProjectFiles) do
            table.insert(Data.Projects, {
                Name             = Name,
                IncludeDirs      = ProjectData.IncludeDirs,
                Files            = ProjectData.Files,
                Path             = ProjectData.Path,
                CSharpBindingsDir = ProjectData.CSharpBindingsDir
            })
        end

        -- The reflector only knows about types it parses. A game or plugin workspace contains just its
        -- own module, so without the engine's headers it cannot tell that an engine base class or an
        -- engine struct property is reflected: the base is silently dropped (null SuperStruct, so every
        -- IsChildOf against it fails at runtime) and the property emits a Construct_ call it never
        -- declared. The engine publishes a manifest of its reflected modules; every other workspace
        -- pulls it in as reference-only input, parsed for type discovery but never generated for.
        local function NormalizePath(P)
            return (path.getabsolute(P):gsub("\\", "/"):lower())
        end

        local bIsEngineWorkspace = NormalizePath(_MAIN_SCRIPT_DIR) == NormalizePath(LuminaDir)

        -- Lua, not JSON: premake's bundled json module encodes but does not decode, and a manifest
        -- nobody can read back is no manifest at all. loadfile gives us the table directly.
        local ManifestPath = path.join(LuminaDir, "Intermediates", "Reflection", "EngineModules.lua")

        if bIsEngineWorkspace then
            -- %q quotes and escapes for Lua, so paths with backslashes survive the round trip.
            local Parts = { "return {\n  Projects = {\n" }
            for _, Project in ipairs(Data.Projects) do
                table.insert(Parts, "    {\n")
                table.insert(Parts, string.format("      Name = %q,\n", Project.Name))
                table.insert(Parts, string.format("      Path = %q,\n", Project.Path))

                table.insert(Parts, "      IncludeDirs = {\n")
                for _, Dir in ipairs(Project.IncludeDirs) do
                    table.insert(Parts, string.format("        %q,\n", Dir))
                end
                table.insert(Parts, "      },\n")

                table.insert(Parts, "      Files = {\n")
                for _, F in ipairs(Project.Files) do
                    table.insert(Parts, string.format("        %q,\n", F))
                end
                table.insert(Parts, "      },\n")
                table.insert(Parts, "    },\n")
            end
            table.insert(Parts, "  },\n}\n")

            local Encoded = table.concat(Parts)

            -- Rewrite only on a real change: downstream workspaces key their up-to-date check on this
            -- file's timestamp, and touching it every generate would re-reflect every game needlessly.
            local Existing = nil
            local Read = io.open(ManifestPath, "r")
            if Read then
                Existing = Read:read("*a")
                Read:close()
            end

            if Existing ~= Encoded then
                os.mkdir(path.getdirectory(ManifestPath))
                local Out = io.open(ManifestPath, "w")
                if Out then
                    Out:write(Encoded)
                    Out:close()
                end
            end
        else
            local Chunk, LoadError = loadfile(ManifestPath)
            if not Chunk then
                Logger.Error("Could not read the engine reflection manifest at " .. ManifestPath)
                Logger.Error(tostring(LoadError))
                Logger.Error("Generate the engine's projects once before this one - the manifest is what lets your module derive from and reference engine reflected types.")
                os.exit(1)
            end

            local bOk, Manifest = pcall(Chunk)
            if not bOk or type(Manifest) ~= "table" or type(Manifest.Projects) ~= "table" then
                Logger.Error("Engine reflection manifest at " .. ManifestPath .. " is malformed. Regenerate the engine's projects to rewrite it.")
                os.exit(1)
            end

            for _, EngineProject in ipairs(Manifest.Projects) do
                EngineProject.ReferenceOnly = true
                -- Its bindings belong to the engine's own build, not this one's.
                EngineProject.CSharpBindingsDir = ""
                table.insert(Data.Projects, EngineProject)
            end
        end

        local File = io.open("Reflection_Files.json", "w")
        if File then
            File:write(json.encode(Data))
            File:close()
        end

        local SystemName = Capitalize(os.host())

        local Extension = ""
        if SystemName == "Windows" then
            Extension = ".exe"
        end

        local ReflectionDirectory = path.join(LuminaDir, "Binaries", SystemName .. "64", "Reflector" .. Extension)
        local CmdLine = ReflectionDirectory .. " " .. path.getabsolute("Reflection_Files.json")


        if SystemName == "Windows" then
            CmdLine = CmdLine:gsub("/", "\\")
        end

        -- Skip the libclang parse when no reflected input is newer than the stamp.
        -- Workspace-local: a game project generates into its own Intermediates, so keying off the engine's
        -- stamp made every game look up-to-date the moment the engine had reflected, and its outputs were
        -- never produced. _MAIN_SCRIPT_DIR is the engine root for Lumina.slnx, so that side is unchanged.
        local StampFile = path.join(_MAIN_SCRIPT_DIR, "Intermediates", "Reflection", ".stamp")
        local function FileTime(P)
            local Stat = os.stat(P)
            return (Stat and Stat.mtime) or 0
        end

        local StampTime = FileTime(StampFile)
        local LatestInput = FileTime(ReflectionDirectory) -- rebuilding the Reflector invalidates outputs

        -- A downstream workspace's output depends on the engine's types too: an engine base gaining a
        -- field changes what this module generates, even though none of its own headers moved. The
        -- engine's own stamp is the precise signal -- it is touched exactly when the engine re-reflects
        -- -- and costs one stat, where re-scanning every engine header would cost thousands.
        if not bIsEngineWorkspace then
            local EngineStamp = path.join(LuminaDir, "Intermediates", "Reflection", ".stamp")
            for _, Upstream in ipairs({ EngineStamp, ManifestPath }) do
                local T = FileTime(Upstream)
                if T > LatestInput then
                    LatestInput = T
                end
            end
        end
        if LatestInput == 0 then
            Logger.Error("Reflector binary not found at " .. ReflectionDirectory)
            Logger.Error("This means the Reflector project FAILED to build - scroll up to the 'Reflector.vcxproj -- FAILED' errors above; the reflection step itself is fine.")
            Logger.Error("Fresh clone? Run Setup.bat (downloads External/LLVM) and verify LUMINA_DIR points at this engine root.")
            os.exit(1)
        end

        if StampTime > 0 then
            for _, ProjectData in pairs(ProjectFiles) do
                for _, F in ipairs(ProjectData.Files) do
                    local T = FileTime(F)
                    if T > LatestInput then LatestInput = T end
                    if LatestInput > StampTime then break end
                end
                if LatestInput > StampTime then break end
            end
        end

        -- A fresh stamp only means no input changed; it says nothing about the outputs still being on disk.
        -- Wiping Intermediates leaves the stamp intact elsewhere, so verify what we'd skip regenerating
        -- actually exists -- otherwise the build fails later on a missing unity shard. Every shard the
        -- vcxproj lists has to be there, not just the first: premake bakes the full fixed list in.
        local ShardCount = (LuminaConfig and LuminaConfig.ReflectionUnityShardCount) or 8
        local bOutputsPresent = true
        for Name, _ in pairs(ProjectFiles) do
            for Shard = 0, ShardCount - 1 do
                local Unity = path.join(_MAIN_SCRIPT_DIR, "Intermediates", "Reflection", Name,
                    "ReflectionUnity_" .. Shard .. ".gen.cpp")
                -- A zero-byte shard is a half-finished write from an aborted Reflector run; treat it as absent.
                local Stat = os.stat(Unity)
                if not Stat or (Stat.size or 0) == 0 then
                    bOutputsPresent = false
                    break
                end
            end

            if not bOutputsPresent then
                break
            end
        end

        if StampTime > 0 and LatestInput > 0 and LatestInput <= StampTime and bOutputsPresent then
            Logger.Success("Reflection up-to-date - skipping Reflector exec.")
            os.remove("Reflection_Files.json")
            return
        end

        Logger.Info("Executing Command Line " .. CmdLine)
        local Result = os.execute(CmdLine)

        -- os.execute returns an int exit code or a bool (older Lua); handle both.
        local bOk = (Result == 0) or (Result == true)

        if bOk then
            Logger.Success("Reflection completed successfully!")
            os.remove("Reflection_Files.json")
            local StampDir = path.getdirectory(StampFile)
            os.mkdir(StampDir)
            local Touch = io.open(StampFile, "w")
            if Touch then
                Touch:write(os.date())
                Touch:close()
            end
        else
            -- Must exit non-zero so ReflectionRunner.bat forwards it and the build halts.
            Logger.Error("Reflection failed - keeping Reflection_Files.json for debugging")
            os.exit(1)
        end
    end,

	onEnd = function()
	end
}

return m