#pragma once

#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Math/Transform.h"
#include "Core/Object/ObjectMacros.h"
#include "MeshSocket.generated.h"

namespace Lumina
{
    /** A named attach point on a mesh asset. Skeletal sockets ride a bone (BoneName); static mesh
     *  sockets sit relative to the mesh origin (BoneName unused). Gameplay resolves sockets by name. */
    REFLECT()
    struct RUNTIME_API FMeshSocket
    {
        GENERATED_BODY()

        /** Name gameplay looks the socket up by (e.g. "WeaponR", "Muzzle"). */
        PROPERTY(Editable, Category = "Socket")
        FName SocketName;

        /** Bone the socket rides on. Skeletal meshes only; ignored on static meshes. */
        PROPERTY(Editable, Category = "Socket", BonePicker)
        FName BoneName;

        /** Offset from the bone (skeletal) or the mesh origin (static). */
        PROPERTY(Editable, Category = "Socket")
        FTransform RelativeTransform;
    };

    inline const FMeshSocket* FindSocketByName(const TVector<FMeshSocket>& Sockets, const FName& SocketName)
    {
        for (const FMeshSocket& Socket : Sockets)
        {
            if (Socket.SocketName == SocketName)
            {
                return &Socket;
            }
        }
        return nullptr;
    }
}
