#pragma once

#define USE_IMGUI_API
#include <imgui.h>
#include <entt/entt.hpp>
#include "Core/Math/Math.h"

#include "UI/Properties/PropertyTable.h"
#include "WorldEditorMode.h"

namespace Lumina
{
    class CWorld;
    struct SCameraComponent;
    struct SFoliageComponent;
    struct STerrainComponent;

    // Foliage painting viewport mode: manage foliage types (static mesh + scatter/instancing settings), then
    // paint/erase instances onto the terrain (or ground plane). Instances follow the terrain when sculpted.
    class FFoliageEditMode final : public IWorldEditorMode
    {
    public:
        const char* GetDisplayName() const override { return "Foliage"; }
        const char* GetIcon() const override { return LE_ICON_FOREST; }
        const char* GetTooltip() const override
        {
            return "Paint instanced foliage (grass, trees, rocks). Instances follow the terrain when you sculpt it.";
        }

        void OnEnter(CWorld* World) override;
        void OnExit(CWorld* World) override;

        void DrawToolbar(CWorld* World, float ButtonSize) override;
        void DrawOverlay(CWorld* World, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera) override;
        void Tick(CWorld* World, const SCameraComponent& Camera, bool bViewportHovered, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize) override;

        // Painting captures viewport input (suppress select/gizmo while in the mode).
        bool ConsumesViewportInput() const override { return true; }

        /** Get the world's foliage component, creating the singleton foliage entity if needed.
         *  OutEntity receives the owning entity, which callers need to route render dirty marks. */
        static SFoliageComponent* FindOrCreateFoliage(CWorld* World, entt::entity* OutEntity = nullptr);

    private:
        enum class EFoliageBrushMode : int32 { Paint = 0, Erase = 1 };

        bool  bShowSettings = true;

        EFoliageBrushMode Mode = EFoliageBrushMode::Paint;
        float Radius   = 384.0f;
        float Falloff  = 0.5f;
        int32 ActiveType = 0;

        FVector3 LastHit   = FVector3(0.0f);
        bool      bHitValid = false;

        bool  bTransactionOpen = false;

        // Accumulates fractional paint budget across frames so low densities still place instances.
        float PaintAccumulator = 0.0f;

        // Cheap xorshift RNG for scatter jitter (editor-only; determinism not required).
        uint32 RngState = 0x9E3779B9u;
        float  RandFloat();

        SFoliageComponent* FindFoliage(CWorld* World) const;
        STerrainComponent* FindTerrain(CWorld* World, FVector3& OutOrigin) const;

        // Project a world XZ onto the paint surface (terrain if present, else ground plane). Always succeeds.
        bool ProjectToSurface(CWorld* World, STerrainComponent* Terrain, const FVector3& TerrainOrigin, float WorldX, float WorldZ, FVector3& OutPos, FVector3& OutNormal) const;

        void PaintDab(CWorld* World, SFoliageComponent& Foliage, STerrainComponent* Terrain, const FVector3& TerrainOrigin, const FVector3& Center, float DeltaSeconds);
        void EraseDab(SFoliageComponent& Foliage, const FVector3& Center);

        void DrawTypePanel(CWorld* World, SFoliageComponent& Foliage);

        /** Both foliage caches, together. See MarkFoliageChanged in FoliageComponent.h for why. */
        void MarkFoliageDirty(CWorld* World, SFoliageComponent& Foliage);

        /** Points TypeSettings at the active SFoliageType, rebuilding only when the binding actually moved.
         *  Types is a TVector, so adding one can reallocate and leave the table holding a dangling pointer;
         *  the bound address is tracked rather than just the index for exactly that reason. */
        void RebindTypeSettings(CWorld* World, SFoliageComponent& Foliage);

        // Draws the active type's reflected properties, which is what gives the mesh slot the standard
        // asset picker and every numeric its PROPERTY clamp -- all of it was hand-rolled before, so the
        // widgets and their limits drifted from the metadata that was already there.
        FPropertyTable TypeSettings;
        void*          BoundTypeData = nullptr;
    };
}
