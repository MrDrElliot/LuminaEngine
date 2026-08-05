#include "RuntimePCH.h"
#include "Math.h"

#include "Core/Math/Transform.h"
#include "Core/Serialization/Archiver.h"

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
