using LuminaBuildTool.Configuration;

public class RmlUi : LuminaThirdPartyModuleRules
{
    public RmlUi(TargetInfo Target)
        : base(Target)
    {
        // RmlUi's own error handling is exception based.
        bEnableExceptions = true;

        // Since 6.3 RmlUi's own RTTI is always on, so rmlui_dynamic_cast survives the engine's /GR-.

        PublicIncludePaths.Add("Include");
        PrivateIncludePaths.Add("Source/Core");
        PrivateIncludePaths.Add("..");

        PublicDependencyModuleNames.Add("FreeType");

        // RMLUI_STATIC_LIB is also a global definition; both sides must agree or consumers see
        // dllimport declarations against a static library.
        PublicDefinitions.Add("RMLUI_STATIC_LIB");
        PrivateDefinitions.Add("RMLUI_VERSION=\"6.3\"");
        PrivateDefinitions.Add("RMLUI_FONT_ENGINE_FREETYPE");

        // Vendored and trimmed to Core plus Debugger.
        SourceDirectories.Add("Source/Core");
        SourceDirectories.Add("Source/Debugger");

        // Source/Debugger/CommonSource.h defines non-inline variables such as common_rcss at
        // namespace scope, so two of its sources sharing a translation unit redefine them
        // (C2086/C2374). That is eight files. It used to disqualify the module, which meant the 185
        // files of Source/Core were compiled one at a time as collateral: a third of the engine's
        // whole clean-build cost, spent re-parsing RmlUi's own headers 185 times over.
        foreach (string Source in new[]
        {
            "Debugger.cpp",
            "DebuggerPlugin.cpp",
            "DebuggerSystemInterface.cpp",
            "ElementContextHook.cpp",
            "ElementDebugDocument.cpp",
            "ElementInfo.cpp",
            "ElementLog.cpp",
            "Geometry.cpp",

            // Source/Core files that name something at namespace scope another one also names.
            // Each is a file-local definition upstream never had to make unique, because upstream
            // compiles one file at a time. To re-derive this list after a version bump, drop these
            // entries, build, and read the C2084 and C2040 pairs the compiler reports.
            "StyleSheetFactory.cpp",         // static instance
            "StyleSheetSpecification.cpp",   // static instance
            "TemplateCache.cpp",             // static instance
            "StyleSheetSelector.cpp",        // IsTextElement, also in StyleSheetNode.cpp
            "StyleSheetParser.cpp",          // IsEscapedCharacter, also in StringUtilities.cpp
        })
        {
            ExcludeFromUnity.Add(Source);
        }
    }
}
