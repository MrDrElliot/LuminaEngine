#pragma once

#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/ObjectMacros.h"
#include "PrimitiveManager.generated.h"

namespace Lumina
{
    REFLECT()
    class RUNTIME_API CPrimitiveManager : public CObject
    {
        GENERATED_BODY()
    public:

        CPrimitiveManager();

        void Initialize();

        static CPrimitiveManager& Get();

        /** Unit cube mesh. */
        PROPERTY(NotSerialized)
        TObjectPtr<CStaticMesh> CubeMesh;

        /** Unit sphere mesh. */
        PROPERTY(NotSerialized)
        TObjectPtr<CStaticMesh> SphereMesh;

        /** Unit plane mesh. */
        PROPERTY(NotSerialized)
        TObjectPtr<CStaticMesh> PlaneMesh;

        /** Unit cylinder mesh. */
        PROPERTY(NotSerialized)
        TObjectPtr<CStaticMesh> CylinderMesh;

        /** Unit cone mesh. */
        PROPERTY(NotSerialized)
        TObjectPtr<CStaticMesh> ConeMesh;

        /** Unit capsule mesh. */
        PROPERTY(NotSerialized)
        TObjectPtr<CStaticMesh> CapsuleMesh;
    };
}
