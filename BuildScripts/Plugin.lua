-- Each plugin module is its own SharedLib. The .lplugin descriptor is the runtime source of truth; keep it in sync with the LuminaPluginModule() calls here.

assert(LuminaConfig, "Plugin.lua: include BuildScripts/Dependencies first")

-- Include Module.lua so LuminaPluginModule works in external game workspaces too.
include(path.join(_SCRIPT_DIR, "Module.lua"))

LuminaPlugins = LuminaPlugins or {}


---@param Def table Plugin metadata
function LuminaPlugin(Def)
    assert(Def.Name, "LuminaPlugin: Name is required")

    Def.SharedDependencies       = Def.SharedDependencies       or {}
    Def.SharedModuleDependencies = Def.SharedModuleDependencies or {}
    Def.Modules                  = {} -- filled in by LuminaPluginModule

    -- _SCRIPT_DIR is this plugin's <Name>.lua dir, i.e. the plugin root.
    Def.PluginRoot = _SCRIPT_DIR

    LuminaPlugins[Def.Name] = Def
end


---@param Def table Plugin module definition
function LuminaPluginModule(Def)
    assert(Def.Plugin, "LuminaPluginModule: Plugin is required")
    assert(Def.Name,   "LuminaPluginModule: Name is required")
    assert(Def.Type,   "LuminaPluginModule: Type is required (Runtime/Editor/Developer)")

    local Plugin = LuminaPlugins[Def.Plugin]
    assert(Plugin, "LuminaPluginModule: plugin '" .. Def.Plugin .. "' not declared (call LuminaPlugin first)")

    Def.ModuleDependencies       = Def.ModuleDependencies       or {}
    Def.EditorModuleDependencies = Def.EditorModuleDependencies or {}
    Def.Dependencies             = Def.Dependencies             or {}
    Def.PublicDefines            = Def.PublicDefines            or {}
    Def.PrivateDefines           = Def.PrivateDefines           or {}
    Def.PublicIncludeDirs        = Def.PublicIncludeDirs        or {}
    Def.ExtraFiles               = Def.ExtraFiles               or {}

    -- Reflection on by default; Reflector is a no-op when there's nothing to find.
    if Def.Reflection == nil then
        Def.Reflection = true
    end

    -- Effective deps = plugin shared + module-specific.
    local AllDeps = {}
    for _, D in ipairs(Plugin.SharedDependencies) do          table.insert(AllDeps, D) end
    for _, D in ipairs(Def.Dependencies)        do            table.insert(AllDeps, D) end

    local AllModuleDeps = {}
    for _, D in ipairs(Plugin.SharedModuleDependencies) do    table.insert(AllModuleDeps, D) end
    for _, D in ipairs(Def.ModuleDependencies)         do     table.insert(AllModuleDeps, D) end

    Def.AllDependencies       = AllDeps
    Def.AllModuleDependencies = AllModuleDeps

    table.insert(Plugin.Modules, Def)

    -- Editor-typed modules link Editor automatically.
    if Def.Type == "Editor" then
        local HasEditorLink = false
        for _, M in ipairs(Def.EditorModuleDependencies) do
            if M == "Editor" then HasEditorLink = true break end
        end
        if not HasEditorLink then
            table.insert(Def.EditorModuleDependencies, "Editor")
        end
    end

    -- Delegate to LuminaModule for force-includes/reflection/EASTL/linking, then steer output and source dirs into the plugin tree.
    -- Editor-typed modules drop the Game platform; pass it THROUGH the Def so it lands on the LuminaModules
    -- record, not just as a loose removeplatforms() directive. The monolithic-Shipping WHOLEARCHIVE loops in
    -- the Lumina app premake key off Mod.RemovePlatforms to exclude editor modules from the Game exe; setting
    -- it only via a bare removeplatforms() left those loops force-linking a lib that was never built for Game.
    LuminaModule({
        Name                     = Def.Name,
        Kind                     = "SharedLib",
        Reflection               = Def.Reflection,
        ModuleDependencies       = AllModuleDeps,
        EditorModuleDependencies = Def.EditorModuleDependencies,
        Dependencies             = AllDeps,
        PublicIncludeDirs        = Def.PublicIncludeDirs,
        PrivateIncludeDirs       = Def.PrivateIncludeDirs,
        PublicDefines            = Def.PublicDefines,
        PrivateDefines           = Def.PrivateDefines,
        ExtraFiles               = Def.ExtraFiles,
        RemovePlatforms          = (Def.Type == "Editor") and { "Game" } or nil,
    })

    -- Multi-module plugins share Source/, so drop LuminaModule's blanket Source/** glob first, then add only this module's slice.
    location(Plugin.PluginRoot)
    removefiles { "Source/**.h", "Source/**.cpp" }
    files {
        path.join(Plugin.PluginRoot, "Source", Def.Name, "**.h"),
        path.join(Plugin.PluginRoot, "Source", Def.Name, "**.cpp"),
    }
    includedirs { path.join(Plugin.PluginRoot, "Source", Def.Name) }

    -- Generated bindings go to the plugin's Scripts/Generated so they compile into the plugin's OWN assembly, not LuminaSharp.dll.
    csharpbindingsdir(path.join(Plugin.PluginRoot, "Scripts", "Generated"))

    -- DLL goes under the plugin's Binaries/<Platform>/, matching FPlugin::ResolveModuleBinaryPath.
    targetdir(path.join(Plugin.PluginRoot, "Binaries", LuminaConfig.OutputDirectory))
    objdir   (path.join(Plugin.PluginRoot, "Intermediates", "Obj", LuminaConfig.OutputDirectory, "%{prj.name}"))

    -- Monolithic Shipping: plugin StaticLibs go into the engine Binaries dir so bare-name /WHOLEARCHIVE finds them on the existing LIBPATH.
    filter "configurations:Shipping"
        targetdir(LuminaConfig.GetTargetDirectory())
    filter {}
    -- (Editor modules' removeplatforms{"Game"} is now passed through the LuminaModule Def above.)
end
