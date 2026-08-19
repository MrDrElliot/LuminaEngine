#pragma once
#include "Core/Math/Vector/Vector.h"
#include "Core/Math/Matrix/Matrix.h"
// Math::Min/Max are used below. The Reflector parses headers as one PCH-less amalgamation, so a
// header that leans on a transitively-provided include mis-parses the moment that path changes.
#include "Core/Math/Math.h"
#include "Platform/Platform.h"
#include "Core/Object/ObjectMacros.h"
#include "AABB.generated.h"


namespace Lumina
{
    REFLECT()
    struct FAABB
    {
        GENERATED_BODY()
        
        PROPERTY(Editable)
        FVector3 Min;

        PROPERTY(Editable)
        FVector3 Max;
        
        FAABB()
            : Min(0.0f), Max(0.0f)
        {}

        FAABB(const FVector3& InMin, const FVector3& InMax)
            : Min(InMin), Max(InMax)
        {}

        // False for both a default-constructed box and the inverted min/max a failed build leaves behind.
        FORCEINLINE bool IsValid() const { return Max.x > Min.x || Max.y > Min.y || Max.z > Min.z; }

        FUNCTION()
        FORCEINLINE float MaxScale() const { return Math::Max(GetSize().x, Math::Max(GetSize().y, GetSize().z)); }
        
        FUNCTION()
        FORCEINLINE FVector3 GetSize() const { return Max - Min; }
        
        FUNCTION()
        FORCEINLINE FVector3 GetCenter() const { return Min + GetSize() * 0.5f; }
        
        NODISCARD FAABB ToWorld(const FMatrix4& World) const
        {
            FVector3 NewMin = FVector3(World[3]);
            FVector3 NewMax = FVector3(World[3]);

            for (int i = 0; i < 3; i++)
            {
                FVector3 Axis = FVector3(World[i]);

                FVector3 MinContrib = Axis * Min[i];
                FVector3 MaxContrib = Axis * Max[i];

                NewMin += Math::Min(MinContrib, MaxContrib);
                NewMax += Math::Max(MinContrib, MaxContrib);
            }

            return FAABB(NewMin, NewMax);
        }
    };
}
