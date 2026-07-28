LuminaModule({
    Name = "Editor",
    Kind = "SharedLib",
    Reflection = true,
    -- PCH header name must be unique (not pch.h) or the Reflector's libclang pass resolves Runtime's pch.h to this and loops.
    PCH = { Header = "EditorPCH.h", Source = "Source/EditorPCH.cpp" },
    PublicIncludeDirs = { "." },
    ModuleDependencies = { "Runtime" },
    Dependencies =
    {
        "ImGui", "RPMalloc", "EA", "Tracy", "FreeType",
        -- Model-format parsers: MeshOptimizer used by the GLTF importer, BasicUniversal by the texture cooker.
        "TinyOBJLoader", "OpenFBX", "FastGLTF", "MeshOptimizer", "BasicUniversal",
    },
    -- No Game platform: Editor sources reference WITH_EDITOR-only APIs stripped under WITH_EDITOR=0.
    RemovePlatforms = { "Game" },
})

-- NVIDIA Aftermath import lib (no DLL copy here, Runtime handles the copy).
LuminaOptions.LinkAftermath({ Copy = false })
