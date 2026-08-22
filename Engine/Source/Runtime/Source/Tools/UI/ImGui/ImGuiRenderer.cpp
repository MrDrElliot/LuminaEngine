#include "RuntimePCH.h"
#include "ImGuiRenderer.h"

#include "ImGuiDesignIcons.h"
#include "ImGuiFonts.h"
#include "Core/Engine/Engine.h"
#include "imgui/misc/freetype/imgui_freetype.h"

#include "TaskSystem/TaskSystem.h"
#include "Tools/UI/Notification/ImGuiNotifications.h"
#include <imgui.h>

#include "implot.h"
#include "Paths/Paths.h"
#include "Tools/UI/ImGui/ImGuiAllocator.h"
#include "Config/Config.h"
#include "Config/EngineSettings.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Math/Hash/Hash.h"
#include "Tools/UI/ImGui/EditorColors.h"
#include "Core/Windows/Window.h"
#include "Core/Windows/GLFWInclude.h"
#include "Core/Application/Application.h"
#include "Events/Event.h"
#include "Events/EventProcessor.h"
#include "Containers/HashTable.h"
#include "backends/imgui_impl_glfw.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        THashMap<GLFWwindow*, ImVec2> GSecondaryWindowLastMouse;

        void SecondaryKeyCallback(GLFWwindow* W, int Key, int Scancode, int Action, int Mods)
        {
            ImGui_ImplGlfw_KeyCallback(W, Key, Scancode, Action, Mods);
            if (Key == GLFW_KEY_UNKNOWN) { return; }
            const bool Ctrl = (Mods & GLFW_MOD_CONTROL) != 0, Shift = (Mods & GLFW_MOD_SHIFT) != 0;
            const bool Alt  = (Mods & GLFW_MOD_ALT) != 0,     Super = (Mods & GLFW_MOD_SUPER) != 0;
            FEventProcessor& EP = GApp->GetEventProcessor();
            switch (Action)
            {
            case GLFW_RELEASE: EP.Dispatch<FKeyReleasedEvent>(static_cast<EKey>(Key), Ctrl, Shift, Alt, Super); break;
            case GLFW_PRESS:   EP.Dispatch<FKeyPressedEvent>(static_cast<EKey>(Key), Ctrl, Shift, Alt, Super); break;
            case GLFW_REPEAT:  EP.Dispatch<FKeyPressedEvent>(static_cast<EKey>(Key), Ctrl, Shift, Alt, Super, true); break;
            }
        }

        void SecondaryMouseButtonCallback(GLFWwindow* W, int Button, int Action, int Mods)
        {
            ImGui_ImplGlfw_MouseButtonCallback(W, Button, Action, Mods);
            double X, Y;
            glfwGetCursorPos(W, &X, &Y);
            FEventProcessor& EP = GApp->GetEventProcessor();
            if (Action == GLFW_PRESS)        { EP.Dispatch<FMouseButtonPressedEvent>(static_cast<EMouseKey>(Button), X, Y); }
            else if (Action == GLFW_RELEASE) { EP.Dispatch<FMouseButtonReleasedEvent>(static_cast<EMouseKey>(Button), X, Y); }
        }

        void SecondaryCursorPosCallback(GLFWwindow* W, double X, double Y)
        {
            ImGui_ImplGlfw_CursorPosCallback(W, X, Y);
            ImVec2& Last = GSecondaryWindowLastMouse[W];
            const double DeltaX = X - Last.x;
            const double DeltaY = Y - Last.y;
            Last = ImVec2(static_cast<float>(X), static_cast<float>(Y));
            GApp->GetEventProcessor().Dispatch<FMouseMovedEvent>(X, Y, DeltaX, DeltaY);
        }

        void SecondaryScrollCallback(GLFWwindow* W, double XOffset, double YOffset)
        {
            ImGui_ImplGlfw_ScrollCallback(W, XOffset, YOffset);
            GApp->GetEventProcessor().Dispatch<FMouseScrolledEvent>(EMouseKey::Scroll, YOffset);
        }

        // Re-set each frame, cheap and idempotent, so new windows are covered without tracking creation.
        void ForwardSecondaryPlatformWindowInput()
        {
            ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
            GLFWwindow* MainWindow = static_cast<GLFWwindow*>(ImGui::GetMainViewport()->PlatformHandle);
            for (ImGuiViewport* VP : PlatformIO.Viewports)
            {
                GLFWwindow* W = static_cast<GLFWwindow*>(VP->PlatformHandle);
                if (W == nullptr || W == MainWindow)
                {
                    continue;
                }
                if (GSecondaryWindowLastMouse.find(W) == GSecondaryWindowLastMouse.end())
                {
                    double X, Y;
                    glfwGetCursorPos(W, &X, &Y);
                    GSecondaryWindowLastMouse[W] = ImVec2(static_cast<float>(X), static_cast<float>(Y));
                }
                glfwSetKeyCallback(W, SecondaryKeyCallback);
                glfwSetMouseButtonCallback(W, SecondaryMouseButtonCallback);
                glfwSetCursorPosCallback(W, SecondaryCursorPosCallback);
                glfwSetScrollCallback(W, SecondaryScrollCallback);
            }
        }
    }

    namespace
    {
        ImGuiStyle GBaseStyle;
        bool       GBaseStyleValid = false;
        float      GAppliedScale   = -1.0f;
    	
        constexpr float kAutoScaleBias = 0.85f;
    	
        float ResolutionFactor(float ScreenHeight)
        {
            constexpr float ReferenceHeight = 2160.0f;  // 4K, where the UI looks right
            constexpr float MinFactor = 0.9f;           // gentle low-res reduction (~0.95 at 1080p)
            const float T = Math::Max(0.0f, Math::Min(1.0f, ScreenHeight / ReferenceHeight));
            return MinFactor + (1.0f - MinFactor) * T;
        }

        float ResolveUIScale()
        {
            // Zero means auto from monitor DPI, and it is unset in game builds where the config is unmounted.
            const float Override = GetDefault<CEditorSettings>()->UIScale;
            if (Override > 0.0f)
            {
                return Override;
            }

            float Scale = 1.0f;
            if (FWindow* Window = Windowing::PrimaryWindow)
            {
                Scale = Window->GetContentScale();
                Scale *= ResolutionFactor(static_cast<float>(Window->GetMonitorResolution().y));
            }
            Scale = (Scale > 0.0f ? Scale : 1.0f) * kAutoScaleBias;
        	
        	GetMutableDefault<CEditorSettings>()->UIScale = Scale;
        	
            return Math::Max(Scale, 0.5f);
        }

        void ApplyUIScale()
        {
            if (!GBaseStyleValid)
            {
                return;
            }

            const float Scale = ResolveUIScale();
            ImGuiStyle& Style = ImGui::GetStyle();
            Style = GBaseStyle;             // reset to unscaled reference (keeps colors)
            Style.ScaleAllSizes(Scale);
            Style.FontScaleMain = Scale;    // 1.92 dynamic fonts rasterize at the scaled size
            GAppliedScale = Scale;

            ImGuiX::SetUIScale(Scale);
        }
    	
        void RefreshUIScaleIfNeeded()
        {
            if (GBaseStyleValid && ResolveUIScale() != GAppliedScale)
            {
                ApplyUIScale();
            }
        }

        uint64 GAppliedPaletteHash = 0;

        void ApplyPaletteColors(ImGuiStyle& Style)
        {
            using namespace EditorColors;
            const ImVec4 Acc   = Accent();
            const ImVec4 Win   = WindowBg();
            const ImVec4 Frame = FrameBg();
            const ImVec4 Title = TitleBg();
            const ImVec4 Btn   = Button();
            const ImVec4 Hdr   = Header();
            const ImVec4 Bord  = Border();

            Style.Colors[ImGuiCol_Text]             = TextPrimary();
            Style.Colors[ImGuiCol_TextDisabled]     = TextMuted();
            Style.Colors[ImGuiCol_WindowBg]         = Win;
            Style.Colors[ImGuiCol_ChildBg]          = Win;
            Style.Colors[ImGuiCol_PopupBg]          = Win;
            Style.Colors[ImGuiCol_Border]           = Bord;
            Style.Colors[ImGuiCol_FrameBg]          = Frame;
            Style.Colors[ImGuiCol_FrameBgHovered]   = Lighten(Frame, 0.08f);
            Style.Colors[ImGuiCol_FrameBgActive]    = Lighten(Frame, 0.14f);
            Style.Colors[ImGuiCol_TitleBg]          = Title;
            Style.Colors[ImGuiCol_TitleBgActive]    = Title;
            Style.Colors[ImGuiCol_TitleBgCollapsed] = WithAlpha(Title, 0.51f);
            Style.Colors[ImGuiCol_MenuBarBg]        = Lighten(Title, 0.06f);
            Style.Colors[ImGuiCol_CheckMark]        = Acc;
            Style.Colors[ImGuiCol_SliderGrab]       = Acc;
            Style.Colors[ImGuiCol_SliderGrabActive] = Lighten(Acc, 0.10f);
            Style.Colors[ImGuiCol_Button]           = Btn;
            Style.Colors[ImGuiCol_ButtonHovered]    = Lighten(Btn, 0.10f);
            Style.Colors[ImGuiCol_ButtonActive]     = Lighten(Btn, 0.18f);
            Style.Colors[ImGuiCol_Header]           = Hdr;
            Style.Colors[ImGuiCol_HeaderHovered]    = Lighten(Hdr, 0.08f);
            Style.Colors[ImGuiCol_HeaderActive]     = Lighten(Hdr, 0.14f);
            Style.Colors[ImGuiCol_Separator]        = Bord;
            Style.Colors[ImGuiCol_SeparatorHovered] = Lighten(Bord, 0.10f);
            Style.Colors[ImGuiCol_SeparatorActive]  = Acc;
            Style.Colors[ImGuiCol_ResizeGripHovered]= WithAlpha(Acc, 0.50f);
            Style.Colors[ImGuiCol_ResizeGripActive] = Acc;
            Style.Colors[ImGuiCol_Tab]              = Title;
            Style.Colors[ImGuiCol_TabHovered]       = Lighten(Win, 0.10f);
            Style.Colors[ImGuiCol_TabSelected]      = Lighten(Win, 0.06f);
            Style.Colors[ImGuiCol_TextSelectedBg]   = WithAlpha(Acc, 0.35f);
            Style.Colors[ImGuiCol_DragDropTarget]   = Acc;
            Style.Colors[ImGuiCol_NavCursor]        = Acc;
        }

        uint64 HashPalette()
        {
            // The palette is a contiguous run of FVector4 members; hashing the block detects any edit.
            const CEditorColorSettings& P = *GetDefault<CEditorColorSettings>();
            const char* Begin = reinterpret_cast<const char*>(&P.Accent);
            const char* End   = reinterpret_cast<const char*>(&P.RowBgActive) + sizeof(FVector4);
            return Hash::GetHash64(Begin, static_cast<size_t>(End - Begin));
        }

        // Cheap, one hash unless something changed.
        void RefreshStyleIfPaletteChanged()
        {
            if (!GBaseStyleValid)
            {
                return;
            }
            const uint64 Hash = HashPalette();
            if (Hash != GAppliedPaletteHash)
            {
                GAppliedPaletteHash = Hash;
                ApplyPaletteColors(GBaseStyle);  // update the unscaled reference colors
                ApplyUIScale();                  // re-derive the live (scaled) style, preserving the new colors
            }
        }
    }

    void IImGuiRenderer::Initialize()
    {
        IMGUI_CHECKVERSION();

        // Before CreateContext, so even ImGui's first internal allocation goes through our allocator.
        ImGuiX::InstallImGuiAllocator();
		
        Context = ImGui::CreateContext();
    	PlotContext = ImPlot::CreateContext();
		
		FString FontFile_Regular = Paths::GetEngineFontDirectory() + "/Lexend/Lexend-Regular.ttf";
		FString FontFile_Bold = Paths::GetEngineFontDirectory() + "/Lexend/Lexend-Bold.ttf";
		FString FontFile_Mono = Paths::GetEngineFontDirectory() + "/JetbrainsMono/JetBrainsMono-Regular.ttf";
		FString FontFile_MonoBold = Paths::GetEngineFontDirectory() + "/JetbrainsMono/JetBrainsMono-Bold.ttf";
		FString IconFontFile = Paths::GetEngineFontDirectory() + "/materialdesignicons-webfont.ttf";
		constexpr ImWchar IconRanges[] = { LE_ICONRANGE_MIN, LE_ICONRANGE_MAX, 0 };

    	ImFontConfig FontConfig;
    	FontConfig.FontDataOwnedByAtlas = false;

    	ImFontConfig IconFontConfig;
    	IconFontConfig.FontDataOwnedByAtlas = false;
    	IconFontConfig.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LoadColor | ImGuiFreeTypeLoaderFlags_Bitmap;
    	IconFontConfig.MergeMode = true;
    	IconFontConfig.PixelSnapH = true;
    	IconFontConfig.RasterizerMultiply = 1.5f;

		ImGuiIO& io = ImGui::GetIO();
    	auto CreateFontFromFile = [&] (FStringView Path, float FontSize, float IconFontSize, ImGuiX::Font::EFont FontID, const ImVec2& GlyphOffset)
    	{
    		ImFont* pFont = io.Fonts->AddFontFromFileTTF(Path.data(), FontSize, &FontConfig);
    		if (pFont == nullptr)
    		{
    			PANIC("Failed to load font '{}'. Engine resources could not be resolved -- "
    			      "LUMINA_DIR may be unset or pointing at the wrong directory. Run the engine's Setup.bat.",
    			      Path.data());
    		}
		    ImGuiX::Font::GFonts[static_cast<uint8>(FontID)] = pFont;

    		IconFontConfig.GlyphOffset = GlyphOffset;
    		IconFontConfig.GlyphMinAdvanceX = IconFontSize;
    		if (io.Fonts->AddFontFromFileTTF(IconFontFile.c_str(), IconFontSize, &IconFontConfig, IconRanges) == nullptr)
    		{
    			LOG_ERROR("Failed to load icon font '{}'; editor icons will be missing. Check engine resources / LUMINA_DIR.", IconFontFile.c_str());
    		}
    	};
		

    	constexpr float DPIScale = 1.0f;
    	const float size12 = std::floor(12 * DPIScale);
    	const float size14 = std::floor(14 * DPIScale);
    	const float size16 = std::floor(16 * DPIScale);
    	const float size18 = std::floor(18 * DPIScale);
    	const float size24 = std::floor(24 * DPIScale);
    	
    	CreateFontFromFile(FontFile_Regular, size12, size14, ImGuiX::Font::EFont::Tiny, ImVec2(0, 2));
    	CreateFontFromFile(FontFile_Bold, size12, size14, ImGuiX::Font::EFont::TinyBold, ImVec2(0, 2));

    	CreateFontFromFile(FontFile_Regular, size14, size16, ImGuiX::Font::EFont::Small, ImVec2(0, 2));
    	CreateFontFromFile(FontFile_Bold, size14, size16, ImGuiX::Font::EFont::SmallBold, ImVec2(0, 2));

    	CreateFontFromFile(FontFile_Regular, size16, size18, ImGuiX::Font::EFont::Medium, ImVec2(0, 2));
    	CreateFontFromFile(FontFile_Bold, size16, size18, ImGuiX::Font::EFont::MediumBold, ImVec2(0, 2));

    	CreateFontFromFile(FontFile_Regular, size24, size24, ImGuiX::Font::EFont::Large, ImVec2(0, 2));
    	CreateFontFromFile(FontFile_Bold, size24, size24, ImGuiX::Font::EFont::LargeBold, ImVec2(0, 2));

    	// Monospace pair for code editors (Lua / RML / shaders).
    	CreateFontFromFile(FontFile_Mono, size16, size18, ImGuiX::Font::EFont::Mono, ImVec2(0, 2));
    	CreateFontFromFile(FontFile_MonoBold, size16, size18, ImGuiX::Font::EFont::MonoBold, ImVec2(0, 2));

    	io.Fonts->TexMinWidth = 4096;

    	using namespace ImGuiX::Font;
    	io.FontDefault = GFonts[static_cast<uint8>(EFont::Medium)];
    	
        DEBUG_ASSERT(GFonts[(uint8)EFont::Small]->IsLoaded());
        DEBUG_ASSERT(GFonts[(uint8)EFont::SmallBold]->IsLoaded());
        DEBUG_ASSERT(GFonts[(uint8)EFont::Medium]->IsLoaded());
        DEBUG_ASSERT(GFonts[(uint8)EFont::MediumBold]->IsLoaded());
        DEBUG_ASSERT(GFonts[(uint8)EFont::Large]->IsLoaded());
        DEBUG_ASSERT(GFonts[(uint8)EFont::LargeBold]->IsLoaded());
        DEBUG_ASSERT(GFonts[(uint8)EFont::Mono]->IsLoaded());
        DEBUG_ASSERT(GFonts[(uint8)EFont::MonoBold]->IsLoaded());
		
    	
    	io.ConfigWindowsMoveFromTitleBarOnly = true;
    	io.ConfigViewportsNoDefaultParent = true;
    	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
    	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows
    	io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;

        ImGui::StyleColorsDark();
        ImGuiStyle& Style = ImGui::GetStyle();
    	
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			Style.WindowRounding = 0.0f;
			Style.Colors[ImGuiCol_WindowBg].w = 1.0f;
		}
    	
        Style.Alpha = 1.0f;

        // Structural colors not driven by the editor palette, such as scrollbars, plots and docking.
        Style.Colors[ImGuiCol_BorderShadow] =           ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        Style.Colors[ImGuiCol_ScrollbarBg] =            ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
        Style.Colors[ImGuiCol_ScrollbarGrab] =          ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
        Style.Colors[ImGuiCol_ScrollbarGrabHovered] =   ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
        Style.Colors[ImGuiCol_ScrollbarGrabActive] =    ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
        Style.Colors[ImGuiCol_ResizeGrip] =             ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        Style.Colors[ImGuiCol_TabDimmed] =              ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
        Style.Colors[ImGuiCol_TabDimmedSelected] =      ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
        Style.Colors[ImGuiCol_DockingPreview] =         ImVec4(0.26f, 0.49f, 0.28f, 0.70f);
        Style.Colors[ImGuiCol_DockingEmptyBg] =         ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        Style.Colors[ImGuiCol_PlotLines] =              ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
        Style.Colors[ImGuiCol_PlotLinesHovered] =       ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
        Style.Colors[ImGuiCol_PlotHistogram] =          ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
        Style.Colors[ImGuiCol_PlotHistogramHovered] =   ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
        Style.Colors[ImGuiCol_NavWindowingHighlight] =  ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
        Style.Colors[ImGuiCol_NavWindowingDimBg] =      ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
        Style.Colors[ImGuiCol_ModalWindowDimBg] =       ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

        // Everything else is themed from the editor palette and re-derived live when it changes.
        ApplyPaletteColors(Style);

    	
    	Style.FramePadding			= ImVec2(6, 4);
    	Style.WindowPadding			= ImVec2(4, 4);
    	Style.ItemSpacing			= ImVec2(4, 4);
    	Style.CellPadding			= ImVec2(4, 6);
        Style.GrabRounding			= 2.3f;
		Style.FrameRounding			= 2.3f;
		Style.DockingSeparatorSize	= 0.0f;
    	Style.ChildBorderSize		= 0.0f;
    	Style.TabBorderSize			= 1.0f;
    	Style.GrabRounding			= 0.0f;
    	Style.GrabMinSize			= 8.0f;
    	Style.WindowRounding		= 0.0f;
    	Style.WindowBorderSize		= 1.0f;
    	Style.FrameRounding			= 3.0f;
    	Style.IndentSpacing			= 8.0f;
    	Style.TabRounding			= 6.0f;
    	Style.ScrollbarSize			= 16.0f;
    	Style.ScrollbarRounding 	= 0.0f;

    	// Snapshot the fully-configured (unscaled) style, then apply DPI/UI scale.
    	GBaseStyle = Style;
    	GBaseStyleValid = true;
    	GAppliedPaletteHash = HashPalette();
    	ApplyUIScale();
    }

    void IImGuiRenderer::Deinitialize()
    {
    	ImGui::DestroyContext();
    }

    void IImGuiRenderer::StartFrame(const FUpdateContext& UpdateContext)
    {
    	RefreshUIScaleIfNeeded();
    	RefreshStyleIfPaletteChanged();
    	OnStartFrame(UpdateContext);
    }

    ImDrawData* IImGuiRenderer::BuildFrame()
    {
        LUMINA_PROFILE_SCOPE();

        ImGuiIO& Io = ImGui::GetIO();
        const FUIntVector2 ViewportSize = FEngine::GetEngineViewportSize();
        Io.DisplaySize.x = static_cast<float>(ViewportSize.x);
        Io.DisplaySize.y = static_cast<float>(ViewportSize.y);

        ImGuiX::Notifications::Render();
        ImGui::Render();

        ProcessTextureUpdates();

        if (Io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ForwardSecondaryPlatformWindowInput();
        }

        return ImGui::GetDrawData();
    }
}
