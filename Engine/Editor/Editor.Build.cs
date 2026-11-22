using LuminaBuildTool.Configuration;

public class Editor : LuminaModuleRules
{
    public Editor(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;

        // Editor sources reference APIs that WITH_EDITOR=0 strips, so this module cannot appear in a Game target.
        HostType = ModuleHostType.Editor;

        // A module's PCH is named after the module, since a generic name resolved the quoted include to the includer's own directory.
        PrecompiledHeader = new PrecompiledHeaderRules("EditorPCH.h", "Source/EditorPCH.cpp");

        PublicIncludePaths.Add(".");

        // A unity blob holding both material graphs carries two generated bodies for one include, and the later truncates the base class.
        ExcludeFromUnity.Add("MaterialNodeGraph.cpp");
        ExcludeFromUnity.Add("MaterialFunctionGraph.cpp");

        // Includes Windows.h, whose GetObject and friends would rename members in every file after it.
        ExcludeFromUnity.Add("VideoEncoder.cpp");

        PublicDependencyModuleNames.Add("Runtime");

        // The graph canvas tests run ImGui frames of their own.
        TestDependencyModuleNames.Add("ImGui");

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ImGui",
            "RPMalloc",
            "FreeType",

            // Model format parsers for the glTF importer and the texture cooker.
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
