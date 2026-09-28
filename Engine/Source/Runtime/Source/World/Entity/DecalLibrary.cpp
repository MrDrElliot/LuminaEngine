#include "RuntimePCH.h"
#include "DecalLibrary.h"

#include "Components/DecalComponent.h"
#include "Core/Math/Matrix/MatrixMath.h"
#include "World/World.h"

namespace Lumina
{
    ECS::FEntity CDecalLibrary::SpawnDecal(CWorld* World, CMaterialInterface* Material, FVector3 Location, FVector3 Normal, FVector3 Size,
                                           float RotationDegrees, float Lifetime, float FadeOutDuration,
                                           int32 AtlasCell, int32 AtlasColumns, int32 AtlasRows)
    {
        if (World == nullptr || Material == nullptr)
        {
            return ECS::NullEntity;
        }

        const float NormalLength = Math::Length(Normal);
        const FVector3 Facing = NormalLength > 1e-4f ? Normal / NormalLength : FVector3(0.0f, 1.0f, 0.0f);
        const FQuat Spin = Math::AngleAxis(Math::Radians(RotationDegrees), FVector3(0.0f, 0.0f, 1.0f));

        FTransform Transform(Location);
        Transform.SetRotation(Math::RotationBetween(FVector3(0.0f, 0.0f, 1.0f), Facing) * Spin);

        const ECS::FEntity Entity = World->ConstructEntity("Decal", Transform);
        SDecalComponent& Decal = World->EmplaceComponent<SDecalComponent>(Entity);
        Decal.DecalMaterial   = Material;
        Decal.Size            = Size;
        Decal.Lifetime        = Math::Max(Lifetime, 0.0f);
        Decal.FadeOutDuration = Math::Max(FadeOutDuration, 0.0f);
        Decal.AtlasCell       = AtlasCell;
        Decal.AtlasColumns    = AtlasColumns;
        Decal.AtlasRows       = AtlasRows;
        Decal.SpawnTime       = (float)World->GetTimeSinceWorldCreation();

        if (Decal.Lifetime > 0.0f)
        {
            World->SetEntityLifetime(Entity, Decal.Lifetime + Decal.FadeOutDuration);
        }
        return Entity;
    }
}
