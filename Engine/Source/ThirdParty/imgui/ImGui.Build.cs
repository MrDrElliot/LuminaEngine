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
        PublicDependencyModuleNames.Add("Volk");

        PublicDefinitions.Add("GLFW_INCLUDE_NONE");

        // ImGui vendors several optional back ends and add-ons; only the ones the engine uses
        // are built.
        bUseExplicitSourceList = true;
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
            "backends/imgui_impl_vulkan.cpp",
        });
        // imgui_impl_vulkan.cpp expects to be the translation unit that pulls in the Vulkan
        // headers. Sharing one with a neighbour that has already included them under
        // VK_NO_PROTOTYPES leaves VK_SUCCESS and friends undeclared (C2065). Only that file, so
        // only that file is held back; the rest merge.
        ExcludeFromUnity.Add("imgui_impl_vulkan.cpp");
    }
}
