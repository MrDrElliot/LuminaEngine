#pragma once

#include "World/ECS/Registry.h"


#define USE_IMGUI_API
#include <imgui.h>
#include "Core/Math/Math.h"

#include "WorldEditorMode.h"
#include "World/Subsystems/TerrainSculptSystem.h"

namespace Lumina
{
    class CWorld;
    struct SCameraComponent;
    struct STerrainComponent;

    // Terrain sculpt and paint viewport mode, activated by the host mode registry.
    class EDITOR_API FTerrainEditMode final : public IWorldEditorMode
    {
    public:
        const char* GetDisplayName() const override { return "Terrain"; }
        const char* GetIcon() const override { return LE_ICON_TERRAIN; }
        const char* GetTooltip() const override
        {
            return "Sculpt, smooth, flatten, ramp, erode, terrace and paint layers on the active terrain.";
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

        // CreateDefaultTerrain wrapped in a mode creation transaction, so Ctrl+Z removes the entity.
        ECS::FEntity CreateTerrainTransacted(CWorld* World);

        /** Decode an image file (PNG/TGA/HDR/16-bit) and resample its luminance into the terrain's heightmap
         *  grid. Returns false with a human-readable reason in OutError; on success OutSrcW/H report the source
         *  image size. Marks the terrain fully dirty so the next frame re-uploads + re-chunks. */
        static bool ImportHeightmap(STerrainComponent& Terrain, const char* FilePath, FString& OutError, int32& OutSrcW, int32& OutSrcH);

    private:
        bool  bShowSettings = true;

        // Every tool's settings, which each tick stamps with the cursor and frame time to make the dab.
        FTerrainSculptDab Brush;

        // Holding Alt flips this for the stroke, and holding Shift swaps any sculpt tool for Smooth.
        bool  bInvertSculpt = false;

        // Flatten otherwise aims at the height under the cursor where each stroke starts.
        bool  bFixedFlattenHeight = false;

        // Ramp captures RampStart on mouse down and drags RampEnd until release.
        bool      bRampStarted = false;

        // Seconds left showing the size and strength beside the cursor after a shortcut changed them.
        float     ReadoutSeconds = 0.0f;

        FVector3 LastHit     = FVector3(0.0f);
        bool      bHitValid   = false;

        // Dabs interpolate from the previous hit so fast strokes stay gap-free, and it resets when the stroke breaks.
        FVector3 LastStrokeHit = FVector3(0.0f);
        bool      bHasStrokeHit = false;

        // Open from the first dab to mouse release so one Ctrl+Z reverts a whole stroke.
        bool      bTransactionOpen = false;

        // Undo name of the layer Name or UV Scale edit in flight, null when no such edit is open.
        const char* OpenLayerEditName = nullptr;

        // Reused so we act on whatever terrain entity is currently in the world.
        ECS::FEntity FindPreferredTerrain(CWorld* World) const;

        void TrackLayerEditTransaction(const char* Name);
        void CloseLayerEditTransaction();

        // Shift held over a sculpt tool, which then smooths for as long as it is down.
        bool IsMomentarySmooth() const;
        void DrawToolSettings(CWorld* World);
        void DrawLayerPanel(STerrainComponent& Terrain);
        static void EnsureLayerWeightStorage(STerrainComponent& Terrain);
        static void SwapLayers(STerrainComponent& Terrain, int32 A, int32 B);
        static void RemoveLayer(STerrainComponent& Terrain, int32 Index);
    };
}
