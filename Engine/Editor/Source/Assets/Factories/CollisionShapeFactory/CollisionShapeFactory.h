#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Physics/CollisionShape.h"
#include "CollisionShapeFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CCollisionShapeFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Collision Shape"; }
        FStringView GetDefaultAssetCreationName() override { return "NewCollisionShape"; }
        FString GetAssetDescription() const override { return "Authored collision for a mesh: primitives, convex hulls, or a baked triangle mesh."; }
        CClass* GetAssetClass() const override { return CCollisionShape::StaticClass(); }
        FString GetCategory() const override { return "Physics"; }
    };
}
