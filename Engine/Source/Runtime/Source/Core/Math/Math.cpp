#include "RuntimePCH.h"
#include "Math.h"

#include "Core/Math/Transform.h"
#include "Core/Serialization/Archiver.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    // Out of line so Transform.h does not have to pull in Archiver.h (which already depends on the
    // vector types); the declaration is all Concepts::THasSerialize needs to see.
    bool VTransform::Serialize(FArchive& Ar)
    {
        FVector3 L = GetLocation();
        FQuat    R = GetRotation();
        FVector3 S = GetScale();

        Ar << L;
        Ar << R;
        Ar << S;

        if (Ar.IsReading())
        {
            // Through the setters, so the pad lanes land at their invariants (Location.w 0, Scale.w 1)
            // rather than inheriting whatever the default-constructed value had.
            SetLocation(L);
            SetRotation(R);
            SetScale(S);
        }
        return true;
    }
}

namespace Lumina::Math
{
}

namespace Lumina
{
    void FormatArgument(Fmt::FFormatBuffer& Out, const FTransform& Transform, const Fmt::FFormatSpec&)
    {
        const FVector3 Location = Transform.GetLocation();
        const FQuat    Rotation = Transform.GetRotation();
        const FVector3 Scale    = Transform.GetScale();

        AppendFormat(Out,
            "Location: ({:.2f}, {:.2f}, {:.2f}) | Rotation: ({:.2f}, {:.2f}, {:.2f}, {:.2f}) | Scale: ({:.2f}, {:.2f}, {:.2f})",
            Location.x, Location.y, Location.z,
            Rotation.w, Rotation.x, Rotation.y, Rotation.z,
            Scale.x, Scale.y, Scale.z);
    }
}
