-- Build script for the NsightPerf plugin; auto-discovered by LuminaDiscoverEnginePlugins.
-- Editor-only. The plugin IS the opt-in unit: it always builds and links its vendored SDK, and a
-- project enables/disables it per-project in its .lproject (no global build feature flag).

LuminaPlugin({
    Name = "NsightPerf",
})

LuminaPluginModule({
    Plugin = "NsightPerf",
    Name   = "NsightPerfEditor",
    Type   = "Editor",
    -- No reflected types here; skip the Reflector so it never parses the heavy NvPerf headers.
    Reflection = false,
    ModuleDependencies = { "Runtime" },
    Dependencies =
    {
        "ImGui", "RPMalloc", "EA", "Tracy", "Volk", "Entt",
    },
})

-- Engine modules prime the entt facade through their PCH (Runtime's pch.h carries entt), so world/
-- component headers can use entt::/FEntity without including the facade. This plugin has no engine
-- PCH, so force-include the facade into every TU the same way, ahead of any transitively-pulled
-- world header. Cleaner than sprinkling the include across each .cpp.
forceincludes { "World/Entity/EntityHandle.h" }

-- NVIDIA Nsight Perf SDK is vendored under Engine/Source/ThirdParty/NsightPerf and linked
-- unconditionally onto this module. The NvPerf headers include each other by bare name, so both
-- include roots must be on the path. The runtime DLL is copied next to the executable (the engine
-- Binaries dir) so the loader resolves it when this plugin DLL imports it.
do
    local NsightRoot   = LuminaConfig.EnginePath("Engine/Source/ThirdParty/NsightPerf")
    local NsightLibDir = path.join(NsightRoot, "lib")

    includedirs
    {
        path.join(NsightRoot, "include"),
        path.join(NsightRoot, "include/windows-desktop-x64"),
        path.join(NsightRoot, "NvPerfUtility/include"),
        -- NvPerfUtility's HUD/config parsing depends on rapidyaml (single-header amalgamation).
        path.join(NsightRoot, "imports/rapidyaml-0.4.0"),
    }
    libdirs { NsightLibDir }
    links { "nvperf_grfx_host" }
    postbuildcommands
    {
        LuminaConfig.CopyFileIgnoreErrors(
            path.join(NsightLibDir, "nvperf_grfx_host.dll"),
            LuminaConfig.GetTargetDirectory()),
    }
end
