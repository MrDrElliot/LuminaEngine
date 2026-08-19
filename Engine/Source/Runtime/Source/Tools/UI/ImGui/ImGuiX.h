#pragma once

#include <format>
#include "imgui.h"
#include "ImGuizmo.h"
#include "imgui_internal.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Core/LuminaMacros.h"
#include "Core/Math/Math.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Platform/GenericPlatform.h"
#include "Renderer/RHITexture.h"

struct ImGuiWindow;

namespace Lumina
{
    class CClass;
    class CStruct;
}

namespace Lumina::ImGuiX
{
    struct RUNTIME_API FImGuiImageInfo
    {
        FORCEINLINE bool IsValid() const { return ID != 0; }
        
        ImTextureID     ID = 0;
        ImVec2          Size = ImVec2(0, 0);
    };
    
    // Editor UI scale (monitor DPI * bias); 1.0 = none. Fonts/style track it automatically;
    // multiply hardcoded pixel sizes by this so custom layouts stay aligned.
    RUNTIME_API float GetUIScale();

    // Set by the ImGui renderer whenever the scale is (re)resolved.
    RUNTIME_API void SetUIScale(float Scale);

    RUNTIME_API void TextTooltip_Internal(FStringView String);
    RUNTIME_API void TextColoredUnformatted(const ImVec4& Color, const FFixedString& String);

    template<typename... TArgs>
    void TextTooltip(std::format_string<TArgs...> Fmt, TArgs&&... Args)
    {
        FFixedString Buffer;
        std::format_to(std::back_inserter(Buffer), Fmt, std::forward<TArgs>(Args)...);
        TextTooltip_Internal(Buffer);
    }

    // Wrapped tooltip for help text longer than a few words; auto-wraps at ~35em.
    RUNTIME_API void WrappedTooltip_Internal(FStringView String);

    template<typename... TArgs>
    void WrappedTooltip(std::format_string<TArgs...> Fmt, TArgs&&... Args)
    {
        FFixedString Buffer;
        std::format_to(std::back_inserter(Buffer), Fmt, std::forward<TArgs>(Args)...);
        WrappedTooltip_Internal(Buffer);
    }

    // Inline `(?)` icon that shows a wrapped tooltip on hover. Place after a label or control.
    RUNTIME_API void HelpMarker(FStringView Help);

    // Same as HelpMarker but with a custom leading icon (e.g. LE_ICON_INFORMATION_OUTLINE).
    RUNTIME_API void HelpMarkerIcon(const char* Icon, FStringView Help);

    template <typename... TArgs>
    void Text(std::format_string<TArgs...> Fmt, TArgs&&... Args)
    {
        FFixedString Buffer;
        std::format_to(std::back_inserter(Buffer), Fmt, std::forward<TArgs>(Args)...);
        ImGui::TextUnformatted(Buffer.c_str());
    }
    
    RUNTIME_API void TextUnformatted(FStringView String);
    
    template <typename... TArgs>
    void TextColored(const ImVec4& Color, std::format_string<TArgs...> fmt, TArgs&&... Args)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Color);
        ImGui::TextUnformatted(std::format(fmt, std::forward<TArgs>(Args)...).c_str());
        ImGui::PopStyleColor();
    }
    
    template <typename... TArgs>
    void TextWrapped(std::format_string<TArgs...> fmt, TArgs&&... Args)
    {
        ImGuiContext& g = *GImGui;
        const bool bNeedBackup = (g.CurrentWindow->DC.TextWrapPos < 0.0f);
        if (bNeedBackup)
        {
            ImGui::PushTextWrapPos(0.0f);
        }
        
        Text(std::forward<decltype(fmt)>(fmt), std::forward<TArgs>(Args)...);
        
        if (bNeedBackup)
        {
            ImGui::PopTextWrapPos();
        }
    }
    
    RUNTIME_API FStringView ImGuizmoOpToString(ImGuizmo::OPERATION Op);

    RUNTIME_API bool ButtonEx(char const* pIcon, char const* pLabel, ImVec2 const& size = ImVec2( 0, 0 ), const ImColor& backgroundColor = ImGui::ColorConvertFloat4ToU32( ImGui::GetStyle().Colors[ImGuiCol_Button] ), const ImColor& iconColor = ImGui::ColorConvertFloat4ToU32( ImGui::GetStyle().Colors[ImGuiCol_Text] ), const ImColor& foregroundColor = ImGui::ColorConvertFloat4ToU32( ImGui::GetStyle().Colors[ImGuiCol_Text] ), bool shouldCenterContents = false );

    RUNTIME_API inline bool FlatButton( char const* pLabel, ImVec2 const& size = ImVec2( 0, 0 ), const ImColor& foregroundColor = ImGui::ColorConvertFloat4ToU32( ImGui::GetStyle().Colors[ImGuiCol_Text] ) )
    {
        return ButtonEx( nullptr, pLabel, size, ImColor(0), ImColor(0), ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_Text]));
    }

    RUNTIME_API inline bool IconButton(char const* pIcon, char const* pLabel, const ImColor& iconColor = ImGui::ColorConvertFloat4ToU32( ImGui::GetStyle().Colors[ImGuiCol_Text] ), ImVec2 const& size = ImVec2(0, 0), bool shouldCenterContents = false )
    {
        return ButtonEx(pIcon, pLabel, size, ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_Button]), iconColor, ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_Text]), shouldCenterContents);
    }

    enum class ESliderFlags : uint32
    {
        None         = 0,
        FillGradient = 1 << 0,   // sweep the filled track between FillColor and FillColorEnd
        ValueOnHover = 1 << 1,   // show the formatted value in a tooltip while hovered or active
        AlwaysValue  = 1 << 2,   // always draw the formatted value, right-aligned over the track
        Glow         = 1 << 3,   // soft halo behind the knob
        ReadOnly     = 1 << 4,   // render normally but ignore input
    };

    ENUM_CLASS_FLAGS(ESliderFlags);

    // Visual overrides; any color left at 0 is derived from the active theme.
    struct FSliderStyle
    {
        float TrackHeight      = 6.0f;   // unscaled; multiplied by GetUIScale()
        float KnobRadius       = 9.0f;   // unscaled; multiplied by GetUIScale()
        ImU32 TrackColor       = 0;
        ImU32 FillColor        = 0;
        ImU32 FillColorEnd     = 0;      // gradient end, used when FillGradient is set
        ImU32 KnobColor        = 0;
        ImU32 KnobColorHovered = 0;
    };

    RUNTIME_API bool SliderFloat(const char* Label, float* Value, float Min, float Max, ESliderFlags Flags = ESliderFlags::None, const char* Format = "%.2f", const FSliderStyle* Style = nullptr);
    RUNTIME_API bool SliderInt(const char* Label, int32* Value, int32 Min, int32 Max, ESliderFlags Flags = ESliderFlags::None, const char* Format = "%d", const FSliderStyle* Style = nullptr);
    
    // Filter input + trailing clear button, sized so the button cannot clip against the panel edge.
    // Hint is drawn inside the empty box (ImGuiTextFilter::Draw takes no hint of its own). Returns true
    // when the filter text changed this frame, including via the clear button.
    RUNTIME_API bool SearchBar(const char* StrId, ImGuiTextFilter& Filter, const char* Hint = nullptr);

    // ImGuiTextFilter only splits on commas, so "pine tree" is matched as one contiguous run and finds
    // nothing. Treats spaces as separators too and passes only text containing every word.
    RUNTIME_API bool PassSearchFilter(FStringView Query, FStringView Text);
    RUNTIME_API bool PassSearchFilter(const ImGuiTextFilter& Filter, FStringView Text);

    // A searchable single-select dropdown.
    RUNTIME_API int32 SearchableCombo(const char* StrId, const char* Preview, int32 ItemCount, int32 CurrentIndex, const TFunction<FFixedString(int32)>& GetItemLabel, const char* ItemIcon = nullptr);

    // Searchable combo for picking an asset of (or deriving from) FilterClass from the registry.
    // Writes the chosen asset's GUID into InOutGUID and returns true when it changes.
    RUNTIME_API bool AssetReferenceCombo(const char* StrId, CClass* FilterClass, FGuid& InOutGUID, const char* ItemIcon = nullptr);

    // Searchable combo over every loaded class that is-a BaseClass (BaseClass itself included). Writes
    // the pick into InOutClass and returns true when it changes. bAllowNone adds a leading "None" entry
    // that clears the selection; without it the combo always holds a class once one is set.
    RUNTIME_API bool ClassCombo(const char* StrId, CClass* BaseClass, CClass*& InOutClass, bool bAllowNone = true, const char* ItemIcon = nullptr);

    // Same, over structs that is-a BaseStruct. BaseStruct itself is excluded, being never a concrete pick.
    RUNTIME_API bool StructCombo(const char* StrId, CStruct* BaseStruct, CStruct*& InOutStruct, bool bAllowNone = true, const char* ItemIcon = nullptr);
    
    RUNTIME_API ImTextureRef ToImTextureRef(FStringView Path);
    // Direct new-heap ResourceID (scene display targets sample the global heap). ~0u -> placeholder.
    RUNTIME_API ImTextureRef ToImTextureRef(uint32 ResourceID);
    RUNTIME_API ImTextureRef ToImTextureRef(const RHI::FManagedTexture& Texture);

    RUNTIME_API FString FormatSize(size_t Bytes);

    /**
     * Switches subsequent images in this draw list into HDR-preview mode: the sampled texel is treated
     * as LINEAR radiance and run through exposure -> ACES -> display encode, the same transform the
     * scene viewport applies (both share Includes/Tonemap.slang).
     *
     * Use it for float-format sources -- environment maps, scene captures, anything cooked as RGBA16F.
     * Do NOT use it for ordinary color textures: those are already display-encoded, and running the
     * transform again would wash them out. Without it the reverse happens, which is why a linear HDR
     * panorama blitted directly reads far darker in a preview than the same data does in-world.
     *
     * ExposureStops is a photographic EV bias; 0 = the texture's authored values.
     *
     * Always pair with EndHDRPreview -- the mode persists for the rest of the draw list otherwise.
     */
    RUNTIME_API void BeginHDRPreview(ImDrawList* DrawList, float ExposureStops);
    RUNTIME_API void EndHDRPreview(ImDrawList* DrawList);

    /**
     * Draws the following images as one slice of a Texture2DArray.
     *
     * Required, not cosmetic: an array texture's heap slot holds a VIEW_TYPE_2D_ARRAY view, and the
     * ImGui pixel shader's default path reads gTextures2D[]. Sampling a 2D_ARRAY view through a
     * Texture2D descriptor is a type mismatch, so an array drawn without this resolves to the null
     * slot (the purple placeholder) rather than merely looking wrong.
     *
     * The ResourceID handed to AddImage is the same either way -- the bindless arrays are aliased
     * views of one descriptor array; only the descriptor type the shader reads it through changes.
     *
     * Does NOT compose with BeginHDRPreview: each writes the whole display state, so the later call
     * wins. Not currently a limitation, because array layers cook through Basis and are always LDR.
     *
     * Always pair with EndArrayPreview -- the mode persists for the rest of the draw list otherwise.
     */
    RUNTIME_API void BeginArrayPreview(ImDrawList* DrawList, uint32 Slice);
    RUNTIME_API void EndArrayPreview(ImDrawList* DrawList);

    namespace Detail
    {
        // Mirrors FImGuiArgs' display fields in Includes/ImGuiCommon.slang.
        struct FImGuiDisplayState
        {
            uint32 DisplayMode = 0;     // IMGUI_DISPLAY_*
            float  Exposure    = 1.0f;  // linear multiplier
            uint32 ArraySlice  = 0;     // layer to sample, bIsArray only
            uint32 bIsArray    = 0;     // 0 = Texture2D, 1 = Texture2DArray
        };

        // Marker only; never invoked. The renderer backend compares ImDrawCmd::UserCallback against
        // this and reads ImDrawCmd::UserCallbackData as an FImGuiDisplayState. Going through the
        // draw list rather than a global keeps the state ordered with the draws it applies to, which
        // is what makes it correct across docked windows and secondary viewports.
        RUNTIME_API ImDrawCallback GetDisplayStateCallback();
    }

    RUNTIME_API void RenderWindowOuterBorders(ImGuiWindow* Window);
    RUNTIME_API bool UpdateWindowManualResize(ImGuiWindow* Window, ImVec2& NewSize, ImVec2& NewPosition);
    
    namespace Notifications
    {
        enum class EType
        {
            None,
            Success,
            Warning,
            Error,
            Info,
        };


        RUNTIME_API void NotifyInternal(EType Type, FStringView Msg);

        // Extra pixels to lift the notification stack above the work-area bottom edge.
        // The editor sets this to the open footer drawer's height so toasts aren't covered.
        RUNTIME_API void SetBottomInset(float Pixels);

        template <typename... TArgs>
        void NotifyInfo(std::format_string<TArgs...> fmt, TArgs&&... Args)
        {
            FFixedString MessageStr;
            std::format_to(std::back_inserter(MessageStr), fmt, Forward<TArgs>(Args)...);
            NotifyInternal(EType::Info, MessageStr);
        }

        template <typename... TArgs>
        void NotifySuccess(std::format_string<TArgs...> fmt, TArgs&&... Args)
        {
            FFixedString MessageStr;
            std::format_to(std::back_inserter(MessageStr), fmt, Forward<TArgs>(Args)...);
            NotifyInternal(EType::Success, MessageStr);
        }

        template <typename... TArgs>
        void NotifyWarning(std::format_string<TArgs...> fmt, TArgs&&... Args)
        {
            FFixedString MessageStr;
            std::format_to(std::back_inserter(MessageStr), fmt, Forward<TArgs>(Args)...);
            NotifyInternal(EType::Warning, MessageStr);
        }

        template <typename... TArgs>
        void NotifyError(std::format_string<TArgs...> fmt, TArgs&&... Args)
        {
            FFixedString MessageStr;
            std::format_to(std::back_inserter(MessageStr), fmt, Forward<TArgs>(Args)...);
            NotifyInternal(EType::Error, MessageStr);
        }
    }
    
    // Stand-in for the OS title bar, drawn as the main viewport's top side bar. Laid out left to right as
    // [menu section][info section][window controls]. Every pixel not covered by a widget is reported to the
    // platform window as caption, so the whole bar drags without any section having to reserve a gap.
    class RUNTIME_API FApplicationTitleBar
    {
    public:

        // Draws the bar for this frame. Only the info section is measured by the caller; the menu section
        // takes every pixel that is left, so long content (a project name) is bounded by the window rather
        // than clipped against a fixed section width.
        void Draw(TFunction<void()>&& MenuSectionDrawFunction = TFunction<void()>(),
                  TFunction<void()>&& InfoSectionDrawFunction = TFunction<void()>(),
                  float InfoSectionWidth = 0.0f);

        // Screen space rectangle (X, Y, Width, Height) of the bar, as of the last Draw.
        const FVector4& GetScreenRectangle() const { return Rect; }

        // Scaled bar height. Valid before Draw, so callers can reserve space up front.
        static float GetHeight();

        // Scaled width of the minimize/maximize/close cluster.
        static float GetWindowControlsWidth();

        // Height of the single row the sections draw on. Content taller than a line of text (icons,
        // images) must be offset up by half the difference to stay centered on the bar.
        static float GetContentRowHeight();

    private:

        static void DrawWindowControls();

        // Design metrics in unscaled pixels; multiply by the UI scale before use.
        static constexpr float UnscaledButtonWidth     = 45.0f;
        static constexpr float UnscaledBarHeight       = 40.0f;
        static constexpr float UnscaledSectionPadding  = 8.0f;

        FVector4 Rect = FVector4(0.0f);
    };
    
}
