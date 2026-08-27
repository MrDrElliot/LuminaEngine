#pragma once

#include "World/ECS/Registry.h"


#define USE_IMGUI_API
#include <imgui.h>
#include "Core/Math/Math.h"

#include "WorldEditorMode.h"
#include "World/Entity/Components/TerrainBrushComponent.h"

namespace Lumina
{
    class CWorld;
    struct SCameraComponent;
    struct STerrainComponent;

    // Terrain sculpt/paint viewport mode: brush settings window, cursor ring overlay,
    // per-frame stroke integration. Activation owned by the host mode registry.
    class FTerrainEditMode final : public IWorldEditorMode
    {
    public:
        const char* GetDisplayName() const override { return "Terrain"; }
        const char* GetIcon() const override { return LE_ICON_TERRAIN; }
        const char* GetTooltip() const override
        {
            return "Sculpt, smooth, ramp, noise, and paint layers on the active terrain.";
        }

        void OnEnter(CWorld* World) override;

        /** Closes any in-flight sculpt transaction if the user leaves the mode mid-stroke. */
        void OnExit(CWorld* World) override;

        /** Mode-specific brush toolbar shown beneath the mode-selector bar. */
        void DrawToolbar(CWorld* World, float ButtonSize) override;

        /** Brush cursor ring + ramp preview. */
        void DrawOverlay(CWorld* World, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera) override;

        /** Cursor raycast, key shortcuts, stroke dispatch. */
        void Tick(CWorld* World, const SCameraComponent& Camera, bool bViewportHovered, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize) override;

        /** Drop a fresh flat terrain entity at the world origin. */
        static ECS::FEntity CreateDefaultTerrain(CWorld* World);

        /** Decode an image file (PNG/TGA/HDR/16-bit) and resample its luminance into the terrain's heightmap
         *  grid. Returns false with a human-readable reason in OutError; on success OutSrcW/H report the source
         *  image size. Marks the terrain fully dirty so the next frame re-uploads + re-chunks. */
        static bool ImportHeightmap(STerrainComponent& Terrain, const char* FilePath, FString& OutError, int32& OutSrcW, int32& OutSrcH);

    private:
        bool  bShowSettings = true;

        ETerrainBrushMode Mode = ETerrainBrushMode::Sculpt;
        float Radius          = 512.0f;
        // World units per second of height change at the brush center.
        float Strength        = 128.0f;
        float Falloff         = 0.5f;
        float FlattenHeight   = 0.0f;
        int32 ActiveLayer     = 0;

        // Sculpt direction sign; held key (LAlt) momentarily flips it during a stroke.
        // Replaces the older Shift-modifier so Shift remains free for other shortcuts.
        bool  bInvertSculpt   = false;

        // Noise brush controls.
        float NoiseFrequency  = 1.0f / 512.0f;
        int32 NoiseOctaves    = 4;

        // Ramp brush state. Stroke captures Start on mousedown and updates End each
        // frame until release; dabs apply the current Start->End line.
        bool      bRampStarted   = false;
        FVector3 RampStart      = FVector3(0.0f);
        FVector3 RampEnd        = FVector3(0.0f);
        float     RampHalfWidth  = 256.0f;

        // Optional fixed ramp endpoint heights instead of sampling the terrain.
        bool      bRampExplicitHeights = false;
        float     RampStartHeight      = 0.0f;
        float     RampEndHeight        = 0.0f;

        FVector3 LastHit     = FVector3(0.0f);
        bool      bHitValid   = false;

        // Footprint brushes interpolate dabs from the previous frame's hit to this
        // frame's so fast strokes stay gap-free. Reset whenever the stroke breaks.
        FVector3 LastStrokeHit = FVector3(0.0f);
        bool      bHasStrokeHit = false;

        // True while an undo transaction is open for the current stroke (begun on the
        // first dab, committed on mouse release). Lets one Ctrl+Z revert a whole stroke.
        bool      bTransactionOpen = false;

        // Reused so we act on whatever terrain entity is currently in the world.
        ECS::FEntity FindPreferredTerrain(CWorld* World) const;

        void DrawLayerPanel(STerrainComponent& Terrain);
        static void EnsureLayerWeightStorage(STerrainComponent& Terrain);
        static void SwapLayers(STerrainComponent& Terrain, int32 A, int32 B);
        static void RemoveLayer(STerrainComponent& Terrain, int32 Index);
    };
}
