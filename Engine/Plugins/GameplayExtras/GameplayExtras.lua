LuminaPlugin({
    Name = "GameplayExtras",
})

LuminaPluginModule({
    Plugin = "GameplayExtras",
    Name   = "GameplayExtrasRuntime",
    Type   = "Runtime",
    ModuleDependencies = { "Runtime" },
    Dependencies =
    {
        "ImGui", "RPMalloc", "EA", "Tracy",
    },
})
