using LuminaBuildTool.Configuration;

public class RmlUi : LuminaThirdPartyModuleRules
{
    public RmlUi(TargetInfo Target)
        : base(Target)
    {
        // RmlUi's own error handling is exception based.
        bEnableExceptions = true;

        // rmlui_dynamic_cast is a real dynamic_cast unless RMLUI_CUSTOM_RTTI is defined, and the engine
        // builds /GR- by default. Without this, Context's construction casts an Element to
        // ElementDocument against a vtable carrying no type information and crashes on startup.
        // Safe to scope to this module: every dynamic_cast target in RmlUi is an Element subclass
        // declared inside RmlUi, and no engine type derives from Element or instantiates the cast.
        bEnableRtti = true;

        PublicIncludePaths.Add("Include");
        PrivateIncludePaths.Add("Source/Core");
        PrivateIncludePaths.Add("..");

        PublicDependencyModuleNames.Add("FreeType");

        // RMLUI_STATIC_LIB is also a global definition; both sides must agree or consumers see
        // dllimport declarations against a static library.
        PublicDefinitions.Add("RMLUI_STATIC_LIB");
        PrivateDefinitions.Add("RMLUI_VERSION=\"6.0\"");
        PrivateDefinitions.Add("RMLUI_FONT_ENGINE_FREETYPE");

        // Vendored and trimmed to Core plus Debugger.
        SourceDirectories.Add("Source/Core");
        SourceDirectories.Add("Source/Debugger");
        // Not merged: Source/Debugger/CommonSource.h defines non-inline variables such as
        // common_rcss at namespace scope, so every source including it redefines them the moment
        // two share a translation unit (C2086/C2374).
        bUseUnityBuild = false;
    }
}
