using LuminaBuildTool.Configuration;

public class Editor : LuminaModuleRules
{
    public Editor(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;

        // Editor sources reference APIs that WITH_EDITOR=0 strips, so this module cannot appear
        // in a Game target at all.
        HostType = ModuleHostType.Editor;

        // A module's PCH is named after the module. Every module's source root is on the include
        // path of everything downstream, so a generic name there is a name every dependent then
        // has to avoid: EditorPCH.h includes RuntimePCH.h, and when both were "pch.h" the quoted
        // include resolved to the includer's own directory and Runtime's never arrived.
        PrecompiledHeader = new PrecompiledHeaderRules("EditorPCH.h", "Source/EditorPCH.cpp");

        PublicIncludePaths.Add(".");

        PublicDependencyModuleNames.Add("Runtime");

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ImGui",
            "RPMalloc",
            "EA",
            "FreeType",

            // Model format parsers: MeshOptimizer for the glTF importer, BasicUniversal for the
            // texture cooker.
            "TinyOBJLoader",
            "UFBX",
            "CGLTF",
            "MeshOptimizer",
            "BasicUniversal",
        });

        if (LuminaFeatures.IsActive(Target, LuminaFeatures.Aftermath))
        {
            // Import library only; Runtime performs the DLL copy.
            PrivateDependencyModuleNames.Add("NvidiaAftermath");
        }
    }
}
