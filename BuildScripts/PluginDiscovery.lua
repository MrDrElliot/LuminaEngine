-- Scans a Plugins root for <PluginName>/<PluginName>.lua build files and include()s each into the surrounding workspace.
-- Called from the engine root (Engine/Plugins/) and LuminaGameProject (<Project>/Plugins/) so plugins join the matching .slnx.

assert(LuminaConfig, "PluginDiscovery.lua: include BuildScripts/Dependencies first")
include(path.join(_SCRIPT_DIR, "Plugin.lua"))


---@param Root string Absolute path to a Plugins folder (or nil/empty no-op)
function LuminaDiscoverPlugins(Root)
    if not Root or Root == "" then
        return
    end
    if not os.isdir(Root) then
        return
    end

    -- No <PluginName>/<PluginName>.lua means a content-only plugin (skipped here; runtime still mounts its .lplugin).
    for _, Entry in ipairs(os.matchdirs(path.join(Root, "*"))) do
        local Name = path.getname(Entry)
        local BuildFile = path.join(Entry, Name .. ".lua")
        if os.isfile(BuildFile) then
            -- include() must be used (not dofile) so _SCRIPT_DIR scopes to the plugin and PluginRoot resolves correctly.
            include(BuildFile)
        end
    end
end


-- Discover the Engine/Plugins/ root; external game premake5.lua calls LuminaDiscoverPlugins with its own Plugins/ path instead.
function LuminaDiscoverEnginePlugins()
    local EnginePluginsRoot = LuminaConfig.EnginePath("Engine/Plugins")
    LuminaDiscoverPlugins(EnginePluginsRoot)
end
