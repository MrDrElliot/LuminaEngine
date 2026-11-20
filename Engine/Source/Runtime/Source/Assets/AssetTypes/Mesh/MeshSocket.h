#pragma once

#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Math/Transform.h"
#include "Core/Object/ObjectMacros.h"
#include "MeshSocket.generated.h"

namespace Lumina
{
    // A named attach point on a mesh asset, riding a bone on a skeletal mesh or the origin on a static one, which gameplay finds by name.
    REFLECT()
    struct RUNTIME_API SMeshSocket
    {
        GENERATED_BODY()

        // What gameplay looks the socket up by, such as WeaponR or Muzzle.
        PROPERTY(Editable, Category = "Socket")
        FName SocketName;

        // The bone the socket rides on, ignored on static meshes.
        PROPERTY(Editable, Category = "Socket", Picker = "Bone")
        FName BoneName;

        // Offset from the bone on a skeletal mesh, or from the origin on a static one.
        PROPERTY(Editable, Category = "Socket")
        FTransform RelativeTransform;
    };

    inline const SMeshSocket* FindSocketByName(const TVector<SMeshSocket>& Sockets, const FName& SocketName)
    {
        for (const SMeshSocket& Socket : Sockets)
        {
            if (Socket.SocketName == SocketName)
            {
                return &Socket;
            }
        }
        return nullptr;
    }
}
