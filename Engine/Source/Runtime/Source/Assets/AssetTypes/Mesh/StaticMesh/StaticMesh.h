#pragma once
#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Assets/AssetTypes/Mesh/MeshSocket.h"
#include "StaticMesh.generated.h"

namespace Lumina
{
    class CCollisionShape;

    REFLECT()
    class RUNTIME_API CStaticMesh : public CMesh
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        const FMeshSocket* FindSocket(const FName& SocketName) const { return FindSocketByName(Sockets, SocketName); }

        // The default collision shape when it is loaded and holds collision, otherwise null.
        const CCollisionShape* GetDefaultCollisionShape() const;

        // A shape that is set but not loaded yet, which callers wait on rather than baking the render mesh instead.
        bool IsDefaultCollisionShapeLoading() const;

        /** Named attach points relative to the mesh origin (BoneName unused). */
        PROPERTY(Editable, Category = "Sockets")
        TVector<FMeshSocket> Sockets;

        // Simplified collision used by foliage, mesh colliders and navigation wherever they name no shape of their own.
        PROPERTY(Editable, Category = "Collision")
        TStrongObjectPtr<CCollisionShape> DefaultCollisionShape;
    };
}
