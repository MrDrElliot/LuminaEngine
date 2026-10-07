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

        // What the game window is titled, and the project name when left empty.
        PROPERTY(Editable, Category = "Project")
        FString GameDisplayName;

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

        // Content folders that always ship, for assets a game loads by names it builds at runtime, such as /Game/Content/NPCs.
        PROPERTY(Editable, Category = "Maps")
        TVector<FString> CookFolders;

        // Frames per second for a process with no window, such as a dedicated server or bot clients. -tickrate overrides it.
        PROPERTY(Editable, Category = "Server", ClampMin = 1, ClampMax = 1000)
        int32 HeadlessTickRate = 60;
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

        // Kept last, since the style refresh hashes the run of colors from Accent through this one.
        PROPERTY(Editable, Color, Category = "Surfaces", ToolTip = "Component and script header bars in the details panel. Hover and pressed shades derive from it.")
        FVector4 ComponentHeader = FVector4(0.175f, 0.190f, 0.235f, 1.00f);
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

    // How many taps the sun's soft shadow filter takes and how wide it reaches, matching Godot's soft shadow levels.
    REFLECT()
    enum class EShadowQuality : uint8
    {
        Low,
        Medium,
        High,
        Ultra,
    };

    // Low and Medium trace at half resolution, High and Ultra at full; each step up also walks further per ray.
    REFLECT()
    enum class ESSRQuality : uint8
    {
        Low,
        Medium,
        High,
        Ultra,
    };

    // S2x and 4x are absent because both add 2x MSAA, which a visibility-buffer deferred path does not run.
    REFLECT()
    enum class ESMAAMode : uint8
    {
        Off,
        SMAA1x,
        SMAAT2x,
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
        EPresentMode PresentMode = EPresentMode::Immediate;

        // Frames per second the engine paces itself to, where 0 runs uncapped.
        PROPERTY(Editable, Category = "Display", ClampMin = 0, ClampMax = 1000)
        int32 MaxFPS = 0;

        void PostInitSettings() override;

        // Pushes PresentMode to the RHI, rebuilding the primary swapchain if one already exists.
        void ApplyPresentMode() const;

        /** Volumetric fog froxel grid resolution multiplier (1.0 = 96x54x64). Higher is sharper but
            costs more GPU; takes effect on viewport resize or editor restart. */
        PROPERTY(Editable, Category = "Volumetric Fog", ClampMin = 0.25f, ClampMax = 2.0f, Delta = 0.05f)
        float FroxelResolutionScale = 1.0f;

        /** Supersample local (point/spot) light in-scatter 4x per froxel to reduce blockiness near lights. */
        PROPERTY(Editable, Category = "Volumetric Fog")
        bool bSupersampleVolumetricLights = true;

        /** Shadow the fog with the cloud layer overhead, so god rays weaken under an overcast sky. */
        PROPERTY(Editable, Category = "Volumetric Fog")
        bool bCloudShadows = true;

        /** Resolution of the square top-down cloud shadow map. */
        PROPERTY(Editable, Category = "Volumetric Fog", ClampMin = 128, ClampMax = 2048)
        int32 CloudShadowResolution = 512;

        /** World size the cloud shadow map covers around the camera. Larger trades detail for reach. */
        PROPERTY(Editable, Category = "Volumetric Fog", ClampMin = 100.0f, Delta = 100.0f, Units = "m")
        float CloudShadowExtent = 4000.0f;

        /** Steps taken through the cloud layer per shadow texel. */
        PROPERTY(Editable, Category = "Volumetric Fog", ClampMin = 1, ClampMax = 32)
        int32 CloudShadowSteps = 8;

        // Largest face a point light's cube shadow may claim. Higher is sharper, but every tap touches more cache lines.
        PROPERTY(Editable, Category = "Shadows", ClampMin = 128, ClampMax = 1024)
        int32 PointShadowResolution = 512;

        // Sun shadow filtering; a low sun stretches each shadow texel along the ground, which only more taps hide.
        PROPERTY(Editable, Category = "Shadows")
        EShadowQuality ShadowQuality = EShadowQuality::High;

        // Multiplies every mesh's imported LOD switch distances, so a close third-person camera can push them out.
        PROPERTY(Editable, Category = "Level of Detail", ClampMin = 0.25f, ClampMax = 16.0f, Delta = 0.25f)
        float LODDistanceScale = 1.0f;

        // Sharpens textures seen at a glancing angle, such as floors and roads, for a little bandwidth. 1 turns it off.
        PROPERTY(Editable, Category = "Textures", ClampMin = 1, ClampMax = 16)
        int32 MaxAnisotropy = 16;

        /** Trace reflections against the depth buffer, falling back to the prefiltered cube off-screen. */
        PROPERTY(Editable, Category = "Screen Space Reflections")
        bool bScreenSpaceReflections = false;

        // Full resolution removes the half-resolution edge on near-mirror surfaces and costs roughly four times the trace.
        PROPERTY(Editable, Category = "Screen Space Reflections")
        ESSRQuality SSRQuality = ESSRQuality::Medium;

        // How far behind a surface a ray may pass and still count as hitting it; too large reflects hidden surfaces.
        PROPERTY(Editable, Category = "Screen Space Reflections", ClampMin = 0.01f, Units = "m")
        float SSRDepthTolerance = 0.2f;

        // Exponent on the screen distance a ray covers, which eases contact reflections in. Zero disables it.
        PROPERTY(Editable, Category = "Screen Space Reflections", ClampMin = 0.0f, ClampMax = 8.0f)
        float SSRFadeIn = 0.15f;

        // Exponent on the screen distance left, which eases out rays that cross most of the screen. Zero disables it.
        PROPERTY(Editable, Category = "Screen Space Reflections", ClampMin = 0.0f, ClampMax = 8.0f)
        float SSRFadeOut = 2.0f;

        // Roughness above which the prefiltered environment takes over entirely; the hand-off starts at three quarters of it.
        PROPERTY(Editable, Category = "Screen Space Reflections", ClampMin = 0.0f, ClampMax = 1.0f)
        float SSRMaxRoughness = 0.6f;

        /** Overall strength of the traced reflection against the prefiltered fallback. */
        PROPERTY(Editable, Category = "Screen Space Reflections", ClampMin = 0.0f, ClampMax = 1.0f)
        float SSRIntensity = 1.0f;

        /** VRAM ceiling on the pre-skinned vertex buffer, which holds one skinned copy of every visible
            skeletal mesh so passes read it instead of re-blending bones. Past it, the surplus meshes
            blend inline in every pass they appear in. Zero scales the ceiling to the card. The buffer
            is sized from measured demand either way, so a high ceiling costs nothing until a frame
            asks for it. Takes effect on the next frame. */
        PROPERTY(Editable, Category = "Skinning", ClampMin = 0, ClampMax = 4096, Units = "MiB")
        int32 PreSkinnedVertexBudgetMiB = 0;

        // SMAA1x is morphological only; SMAAT2x adds a second jittered subsample resolved against a reprojected history.
        PROPERTY(Editable, Category = "Anti-Aliasing")
        ESMAAMode SMAAMode = ESMAAMode::SMAA1x;

        /** Edge-detection quality. Higher qualities detect more edges at higher GPU cost. */
        PROPERTY(Editable, Category = "Anti-Aliasing")
        ESMAAQuality SMAAQuality = ESMAAQuality::High;

        // T2x history contribution. 0.5 is the true two-sample average; lower trades aliasing for less ghosting.
        PROPERTY(Editable, Category = "Anti-Aliasing", ClampMin = 0.0f, ClampMax = 0.5f)
        float TemporalHistoryWeight = 0.5f;

        // Clamps the reprojected history into the local color range, which is what stops bad motion ghosting.
        PROPERTY(Editable, Category = "Anti-Aliasing")
        bool bTemporalNeighborhoodClamp = true;

        // Widens the clamp box in standard deviations. Larger keeps more history and ghosts more.
        PROPERTY(Editable, Category = "Anti-Aliasing", ClampMin = 0.25f, ClampMax = 4.0f)
        float TemporalClampGamma = 1.0f;

        // Scales the subpixel offset. 0 pins both subsamples to the pixel center, which isolates the resolve.
        PROPERTY(Editable, Category = "Anti-Aliasing", ClampMin = 0.0f, ClampMax = 1.0f)
        float TemporalJitterScale = 0.4f;

        // Screen fraction a pixel may reproject across before its history is refused outright.
        PROPERTY(Editable, Category = "Anti-Aliasing", ClampMin = 0.01f, ClampMax = 1.0f)
        float TemporalMaxReprojection = 0.25f;

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
        float GTAOPower = 2.2f;

        // Slices and steps per pixel, 0 low to 3 ultra. Below ultra the trace runs at half resolution and upsamples.
        PROPERTY(Editable, Category = "Ambient Occlusion", ClampMin = 0, ClampMax = 3)
        int32 GTAOQualityLevel = 2;

        /** Edge-aware denoise passes. 0 disabled, 1 sharp, 2 medium, 3 soft. */
        PROPERTY(Editable, Category = "Ambient Occlusion", ClampMin = 0, ClampMax = 3)
        int32 GTAODenoisePasses = 1;

        /** Scales the sampled radius against the ground-truth radius to counter screen-space bias. */
        PROPERTY(Editable, Category = "Ambient Occlusion|Heuristics", ClampMin = 0.3f, ClampMax = 3.0f)
        float GTAORadiusMultiplier = 1.457f;

        /** Fraction of the radius over which a sample's contribution fades to nothing. */
        PROPERTY(Editable, Category = "Ambient Occlusion|Heuristics", ClampMin = 0.0f, ClampMax = 1.0f)
        float GTAOFalloffRange = 0.615f;

        /** 1 spaces samples evenly along a slice, higher pulls them toward the center where crevices are. */
        PROPERTY(Editable, Category = "Ambient Occlusion|Heuristics", ClampMin = 1.0f, ClampMax = 3.0f)
        float GTAOSampleDistributionPower = 2.0f;

        /** Discards samples behind the center sooner, countering over-darkening behind thin geometry. */
        PROPERTY(Editable, Category = "Ambient Occlusion|Heuristics", ClampMin = 0.0f, ClampMax = 0.7f)
        float GTAOThinOccluderCompensation = 0.0f;

        /** Trades depth-pyramid bandwidth against thin-object accuracy and temporal stability. */
        PROPERTY(Editable, Category = "Ambient Occlusion|Heuristics", ClampMin = 0.0f, ClampMax = 30.0f)
        float GTAODepthMipSamplingOffset = 3.3f;
    };
}
