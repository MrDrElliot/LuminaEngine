#pragma once
#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Assets/AssetTypes/Mesh/MeshCollision.h"
#include "Assets/AssetTypes/Mesh/MeshSocket.h"
#include "StaticMesh.generated.h"

namespace Lumina
{
    REFLECT()
    class RUNTIME_API CStaticMesh : public CMesh
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        const SMeshSocket* FindSocket(const FName& SocketName) const { return FindSocketByName(Sockets, SocketName); }

        // This mesh's collision when it builds anything, otherwise null so callers fall back to the render mesh.
        const SMeshCollision* GetCollision() const { return Collision.HasCollision() ? &Collision : nullptr; }

        // Named attach points relative to the mesh origin, whose BoneName goes unused.
        PROPERTY(Editable, Category = "Sockets")
        TVector<SMeshSocket> Sockets;

        // Simplified collision that mesh colliders, foliage and navigation use, edited in the mesh's Collision tab rather than the grid.
        PROPERTY(Hidden)
        SMeshCollision Collision;
    };
}
