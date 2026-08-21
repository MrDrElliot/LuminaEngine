#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Core/Object/SoftObjectPtr.h"
#include "Core/Object/SubclassOf.h"
#include "Core/Engine/GameInstance.h"
#include "Config/DeveloperSettings.h"
#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Input/Key.h"
#include "Renderer/PresentMode.h"
#include "World/World.h"
#include "EngineSettings.generated.h"

namespace Lumina
{
    // Per-project runtime settings; persists to the project's /Config/GameSettings.json.
    REFLECT(MinimalAPI, ConfigFile = "/Config/GameSettings.json", DisplayName = "Project", Category = "Project")
    class CProjectSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        /** CGameInstance subclass to instantiate at runtime */
        PROPERTY(Editable, Category = "Scripting")
        TSubclassOf<CGameInstance> GameInstanceClass;

        /** World loaded when the standalone game starts. */
        PROPERTY(Editable, Category = "Maps")
        TSoftObjectPtr<CWorld> GameStartupMap;

        /** World opened automatically when the editor finishes loading the project. */
        PROPERTY(Editable, Category = "Maps")
        TSoftObjectPtr<CWorld> EditorStartupMap;

        /** Worlds the cooker walks from to build the shipped PAK. */
        PROPERTY(Editable, Category = "Maps")
        TVector<TSoftObjectPtr<CWorld>> CookRoots;
    };

    // Texture streaming budget and policy. Project-scoped rather than per-user: the pool size a project
    // targets is a shipping decision, and it has to travel with the content it was tuned against.
    REFLECT(MinimalAPI, ConfigFile = "/Config/GameSettings.json", DisplayName = "Texture Streaming", Category = "Rendering")
    class CTextureStreamingSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        /** GPU budget for streamable texture mips, in MiB. Textures are trimmed toward their inline tail
         *  once the total exceeds this. Pinned textures (an open texture editor tab) are exempt and can
         *  push past it. */
        PROPERTY(Editable, Category = "Budget")
        int32 PoolSizeMB = 1024;

        // Off keeps every texture fully resident and stops trimming; their mips are still read from disk.
        PROPERTY(Editable, Category = "Budget")
        bool bTextureStreaming = true;

        /** Multiplier on the requested resident resolution. Above 1 keeps sharper mips than screen
         *  coverage implies; below 1 trades sharpness for memory. */
        PROPERTY(Editable, Category = "Quality")
        float ResolutionBias = 1.0f;

        // Cap on concurrent mip loads. Bounds IO queue depth; MaxLoadStagingMB bounds their memory.
        PROPERTY(Editable, Category = "Performance")
        int32 MaxLoadsInFlight = 8;

        // Ceiling on host bytes held by in-flight reads; one load is always admitted regardless.
        PROPERTY(Editable, Category = "Performance")
        int32 MaxLoadStagingMB = 128;

        // Host bytes staged per frame, enforced at band granularity. Loads that do not fit wait a frame.
        PROPERTY(Editable, Category = "Performance")
        int32 MaxUploadMBPerFrame = 32;

        // Promotions + demotions per frame. Separate from the upload budget: a residency change recreates
        // the GPU image and retires the old one, which a demotion pays while moving zero host bytes.
        PROPERTY(Editable, Category = "Performance")
        int32 MaxResidencyChangesPerFrame = 4;
    };

    // Editor-wide preferences + launch state. Lives in the runtime module so the runtime
    // ImGui renderer can read UIScale, while the editor edits it through the Settings panel.
    REFLECT(MinimalAPI, ConfigFile = "/Editor/Config/EditorPreferences.json", DisplayName = "General", Category = "Editor")
    class CEditorSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        /** Editor UI scale. 0 = auto (monitor DPI + resolution); otherwise an explicit factor (1.0 = 100%).
            Stepped (no click-drag) so the whole editor doesn't relayout continuously while adjusting. */
        PROPERTY(Editable, Category = "Appearance", ClampMin = 0.0f, ClampMax = 3.0f, NoDrag, Delta = 0.1f)
        float UIScale = 0.0f;
        
        /** Rate for anything the editor is not actively working in. */
        PROPERTY(Editable, Category = "Performance", ClampMin = 0, ClampMax = 240)
        int32 MaxBackgroundFPS = 5;

        /** Chord that recompiles + hot-reloads all C# scripts. */
        PROPERTY(Editable, Category = "Hotkeys")
        SKey ReloadScriptsHotkey = SKey(EKey::B, /*Ctrl*/ true, /*Shift*/ true);
        
        /** List of recently open projects. **/
        PROPERTY(Editable, Category = "Loading")
        TVector<FString> RecentProjects;
        
        /** Project to open at startup. **/
        PROPERTY(Editable, Category = "Loading")
        FString StartupProject;

        /** Pre-fills the email box in the crash reporter's send dialog, so it does not have to be
            retyped per crash. The dialog still asks before anything is sent, and the user can edit
            or clear the value there. Blank is fine; reports are still useful anonymous. */
        PROPERTY(Editable, Category = "Crash Reporting")
        FString CrashReportContactEmail;
    };

    // The editor's central color palette.
    REFLECT(MinimalAPI, ConfigFile = "/Editor/Config/EditorPreferences.json", DisplayName = "Editor Colors", Category = "Editor")
    class CEditorColorSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        //~ Accents: interactive + semantic state colors.

        /** Primary interactive accent: checkmarks, sliders, selection, focus, links. */
        PROPERTY(Editable, Color, Category = "Accents")
        FVector4 Accent = FVector4(0.26f, 0.59f, 0.98f, 1.00f);

        /** Secondary accent: folders, special highlights. */
        PROPERTY(Editable, Color, Category = "Accents")
        FVector4 AccentAlt = FVector4(1.00f, 0.78f, 0.40f, 1.00f);

        /** Success: enabled, loaded, confirmation. */
        PROPERTY(Editable, Color, Category = "Accents")
        FVector4 Success = FVector4(0.40f, 0.82f, 0.45f, 1.00f);

        /** Warning: pending, caution. */
        PROPERTY(Editable, Color, Category = "Accents")
        FVector4 Warning = FVector4(0.95f, 0.75f, 0.30f, 1.00f);

        /** Danger: delete, error. */
        PROPERTY(Editable, Color, Category = "Accents")
        FVector4 Danger = FVector4(0.96f, 0.36f, 0.38f, 1.00f);

        /** Section header labels (muted blue). */
        PROPERTY(Editable, Color, Category = "Accents")
        FVector4 SectionHeader = FVector4(0.50f, 0.58f, 0.72f, 1.00f);

        /** Outliner entity icon tint: a warm accent so entities stand out in the scene tree. */
        PROPERTY(Editable, Color, Category = "Accents")
        FVector4 EntityIcon = FVector4(0.90f, 0.44f, 0.36f, 1.00f);

        //~ Text: foreground hierarchy.

        /** Primary / bright text. */
        PROPERTY(Editable, Color, Category = "Text")
        FVector4 TextPrimary = FVector4(0.90f, 0.90f, 0.93f, 1.00f);

        /** Secondary / dim text. */
        PROPERTY(Editable, Color, Category = "Text")
        FVector4 TextDim = FVector4(0.55f, 0.56f, 0.62f, 1.00f);

        /** Tertiary / disabled text. */
        PROPERTY(Editable, Color, Category = "Text")
        FVector4 TextMuted = FVector4(0.42f, 0.42f, 0.47f, 1.00f);

        //~ Surfaces: window / frame / control backgrounds (hover + active variants are derived).

        /** Window / child / popup background. */
        PROPERTY(Editable, Color, Category = "Surfaces")
        FVector4 WindowBg = FVector4(0.13f, 0.14f, 0.15f, 1.00f);

        /** Input field (frame) background. */
        PROPERTY(Editable, Color, Category = "Surfaces")
        FVector4 FrameBg = FVector4(0.08f, 0.08f, 0.08f, 1.00f);

        /** Title bar / menu bar background. */
        PROPERTY(Editable, Color, Category = "Surfaces")
        FVector4 TitleBg = FVector4(0.08f, 0.08f, 0.09f, 1.00f);

        /** Button background. */
        PROPERTY(Editable, Color, Category = "Surfaces")
        FVector4 Button = FVector4(0.25f, 0.25f, 0.25f, 1.00f);

        /** Header / selectable / tree-node background. */
        PROPERTY(Editable, Color, Category = "Surfaces")
        FVector4 Header = FVector4(0.22f, 0.22f, 0.22f, 1.00f);

        /** Borders and separators. */
        PROPERTY(Editable, Color, Category = "Surfaces")
        FVector4 Border = FVector4(0.43f, 0.43f, 0.50f, 0.50f);

        /** Dark panel / card / table-row background. */
        PROPERTY(Editable, Color, Category = "Surfaces")
        FVector4 PanelBg = FVector4(0.10f, 0.11f, 0.13f, 1.00f);

        /** List row background (resting). */
        PROPERTY(Editable, Color, Category = "Surfaces")
        FVector4 RowBg = FVector4(0.135f, 0.140f, 0.165f, 1.00f);

        /** List row background (hovered). */
        PROPERTY(Editable, Color, Category = "Surfaces")
        FVector4 RowBgHovered = FVector4(0.190f, 0.205f, 0.245f, 1.00f);

        /** List row background (active / pressed). */
        PROPERTY(Editable, Color, Category = "Surfaces")
        FVector4 RowBgActive = FVector4(0.160f, 0.175f, 0.215f, 1.00f);
    };

    REFLECT()
    enum class ESMAAQuality : uint8
    {
        Off,
        Low,
        Medium,
        High,
        Ultra,
    };

    REFLECT()
    enum class EVariableRateShading : uint8
    {
        Off,        // 1x1 - full rate
        Rate2x2,    // quarter the fragment shader invocations
        Rate4x4,    // sixteenth (clamped to the GPU's max supported rate)
    };

    // Per-project renderer quality settings; persists to the project's /Config/RendererSettings.json.
    REFLECT(MinimalAPI, ConfigFile = "/Config/RendererSettings.json", DisplayName = "Rendering", Category = "Engine")
    class CRendererSettings : public CDeveloperSettings
    {
        GENERATED_BODY()
    public:

        // How finished frames reach the display. Falls back if the device lacks the requested mode.
        PROPERTY(Editable, Category = "Display")
        EPresentMode PresentMode = EPresentMode::FIFO;

        void PostInitSettings() override;

        // Pushes PresentMode to the RHI, rebuilding the primary swapchain if one already exists.
        void ApplyPresentMode() const;

        /** Volumetric fog froxel grid resolution multiplier (1.0 = 160x90x128). Higher is sharper but
            costs more GPU; takes effect on viewport resize or editor restart. */
        PROPERTY(Editable, Category = "Volumetric Fog", ClampMin = 0.25f, ClampMax = 2.0f, Delta = 0.05f)
        float FroxelResolutionScale = 1.0f;

        /** Supersample local (point/spot) light in-scatter 4x per froxel to reduce blockiness near lights. */
        PROPERTY(Editable, Category = "Volumetric Fog")
        bool bSupersampleVolumetricLights = true;

        /** Trace reflections against the depth buffer, falling back to the prefiltered cube off-screen. */
        PROPERTY(Editable, Category = "Screen Space Reflections")
        bool bScreenSpaceReflections = true;

        /** Ray-march steps per pixel; higher resolves thinner geometry but costs proportionally. */
        PROPERTY(Editable, Category = "Screen Space Reflections", ClampMin = 4, ClampMax = 128)
        int32 SSRMaxSteps = 32;

        /** How far a reflection ray travels before giving up and falling back to the cube. */
        PROPERTY(Editable, Category = "Screen Space Reflections", ClampMin = 1.0f, Units = "m")
        float SSRMaxDistance = 40.0f;

        /** Assumed depth of screen geometry; too small drops hits, too large reflects hidden surfaces. */
        PROPERTY(Editable, Category = "Screen Space Reflections", ClampMin = 0.01f, Units = "m")
        float SSRThickness = 0.5f;

        /** Roughness at which SSR has fully handed back to the cube. Mirror-only without temporal reuse. */
        PROPERTY(Editable, Category = "Screen Space Reflections", ClampMin = 0.0f, ClampMax = 1.0f)
        float SSRRoughnessFade = 0.4f;

        /** Overall strength of the traced reflection against the prefiltered fallback. */
        PROPERTY(Editable, Category = "Screen Space Reflections", ClampMin = 0.0f, ClampMax = 1.0f)
        float SSRIntensity = 1.0f;

        /** Antialiasing quality. Off disables SMAA; higher qualities detect more edges at higher GPU cost. */
        PROPERTY(Editable, Category = "Anti-Aliasing")
        ESMAAQuality SMAAQuality = ESMAAQuality::High;

        // VRS rate for opted-in passes; coarser is fewer PS invocations but softer, and a no-op without pipeline FSR.
        PROPERTY(ReadOnly, Category = "Variable Rate Shading")
        EVariableRateShading VariableRateShading = EVariableRateShading::Off;

        // Screen-space ambient occlusion, reconstructed from depth; visible only where there is ambient to darken.
        PROPERTY(Editable, Category = "Ambient Occlusion")
        bool bEnableGTAO = false;

        /** GTAO sample radius in world units. Larger = wider, softer occlusion. */
        PROPERTY(Editable, Category = "Ambient Occlusion", ClampMin = 0.01f)
        float GTAORadius = 0.5f;

        /** GTAO strength multiplier. 0 = none. */
        PROPERTY(Editable, Category = "Ambient Occlusion", ClampMin = 0.0f)
        float GTAOIntensity = 1.0f;

        /** GTAO contrast exponent applied to the AO factor. Higher = darker, tighter contact shadows. */
        PROPERTY(Editable, Category = "Ambient Occlusion", ClampMin = 0.1f)
        float GTAOPower = 2.0f;
    };
}
