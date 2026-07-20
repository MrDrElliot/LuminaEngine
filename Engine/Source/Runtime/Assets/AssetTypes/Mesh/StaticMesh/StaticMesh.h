#pragma once
#include "Assets/AssetTypes/Mesh/Mesh.h"
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

        const FMeshSocket* FindSocket(const FName& SocketName) const { return FindSocketByName(Sockets, SocketName); }

        /** Named attach points relative to the mesh origin (BoneName unused). */
        PROPERTY(Editable, Category = "Sockets")
        TVector<FMeshSocket> Sockets;
    };
}
