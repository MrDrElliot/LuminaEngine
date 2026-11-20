#pragma once

#include "World/ECS/Registry.h"


#define USE_IMGUI_API
#include <imgui.h>
#include "Core/Math/Math.h"

#include "UI/Properties/PropertyTable.h"
#include "WorldEditorMode.h"

namespace Lumina
{
    class CWorld;
    struct SCameraComponent;
    struct SForestComponent;
    struct STerrainComponent;

    // Paints a forest's species in and out of the ground and places single plants, all streamed by the forest.
    class FFoliageEditMode final : public IWorldEditorMode
    {
    public:
        const char* GetDisplayName() const override { return "Foliage"; }
        const char* GetIcon() const override { return LE_ICON_FOREST; }
        const char* GetTooltip() const override
        {
            return "Paint forest species onto the ground, erase them, or place single plants. Everything follows the terrain.";
        }

        void OnExit(CWorld* World) override;

        void DrawToolbar(CWorld* World, float ButtonSize) override;
        void DrawOverlay(CWorld* World, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize, const SCameraComponent& Camera) override;
        void Tick(CWorld* World, const SCameraComponent& Camera, bool bViewportHovered, ImVec2 ViewportScreenOrigin, ImVec2 ViewportSize) override;

        bool ConsumesViewportInput() const override { return true; }

    private:
        enum class EBrushMode : int32 { Paint, Erase, Place };

        struct FGround
        {
            const STerrainComponent* Terrain = nullptr;
            FVector3                        Origin  = FVector3(0.0f);
            float                           Spacing = 0.0f;
        };

        bool  bShowSettings = true;

        EBrushMode Mode     = EBrushMode::Paint;
        float      Radius   = 32.0f;
        float      Falloff  = 0.5f;
        float      Strength = 0.5f;
        int32      ActiveSpecies = 0;

        FVector3 LastHit   = FVector3(0.0f);
        bool     bHitValid = false;

        bool  bTransactionOpen = false;

        // Fractional plants owed by the Place brush, carried across frames so sparse species still place.
        float PlaceAccumulator = 0.0f;

        // Brush jitter only, so it needs no determinism.
        uint32 RngState = 0x9E3779B9u;
        float  RandFloat();

        struct FTarget
        {
            SForestComponent* Forest = nullptr;
            FVector3          Center = FVector3(0.0f);
        };

        // Finest terrain first, the order the forest grounds its plants in.
        static TVector<FGround> GatherGround(CWorld* World);
        static bool ProjectToGround(const TVector<FGround>& Ground, float WorldX, float WorldZ, FVector3& OutPosition, FVector3& OutNormal);
        static bool RaycastGround(const TVector<FGround>& Ground, const FVector3& RayOrigin, const FVector3& RayDir, FVector3& OutHit);

        // The forest whose square holds the point, else the first one.
        static FTarget FindForest(CWorld* World, const FVector3& Near);
        static FTarget CreateForest(CWorld* World, const TVector<FGround>& Ground);
        static ECS::FEntity FindLegacyFoliage(CWorld* World);

        float BrushWeight(float Distance) const;
        void  PaintDab(const FTarget& Target, const FVector3& Hit, float DeltaSeconds, float Sign);
        void  PlaceDab(const FTarget& Target, const TVector<FGround>& Ground, const FVector3& Hit, float DeltaSeconds);

        void DrawSpeciesPanel(CWorld* World, const FTarget& Target);
        void ConvertLegacyFoliage(CWorld* World, ECS::FEntity Legacy);

        // Bound by address, since adding a species can reallocate the array under the table.
        void RebindSpeciesSettings(SForestComponent* Forest);
        FPropertyTable SpeciesSettings;
        void*          BoundSpeciesData = nullptr;
    };
}
