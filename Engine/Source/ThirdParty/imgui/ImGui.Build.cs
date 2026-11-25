using LuminaBuildTool.Configuration;

public class ImGui : LuminaThirdPartyModuleRules
{
    public ImGui(TargetInfo Target)
        : base(Target)
    {
        // Included both as <imgui.h> and <imgui/misc/...>, so both roots are exposed.
        PublicIncludePaths.Add(".");
        PublicIncludePaths.Add("..");

        PrivateIncludePaths.Add("backends");

        PublicDependencyModuleNames.Add("GLFW");

        PublicDefinitions.Add("GLFW_INCLUDE_NONE");

        // ImGui vendors several optional back ends and add-ons; only the ones the engine uses
        // are built.
        bUseExplicitSourceList = true;

        // Without the developer UI nothing draws, so calls link to generated no-ops rather than the library.
        if (!LuminaFeatures.IsActive(Target, LuminaFeatures.DevUI))
        {
            ExtraSourceFiles.Add("stubs/imgui_stubs.cpp");
            return;
        }

        ExtraSourceFiles.AddRange(new[]
        {
            "imgui.cpp",
            "imgui_draw.cpp",
            "imgui_tables.cpp",
            "imgui_widgets.cpp",
            "imgui_demo.cpp",

            "implot.cpp",
            "implot_items.cpp",
            "implot_demo.cpp",

            "ImGuizmo.cpp",

            "backends/imgui_impl_glfw.cpp",
        });
    }
}
