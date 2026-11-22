#pragma once
#include "Core/Math/Vector/Vector.h"
#include "Core/Math/Matrix/Matrix.h"
// The Reflector parses headers PCH-less, so a transitively provided include mis-parses when that path changes.
#include "Core/Math/Math.h"
#include "Platform/Platform.h"
#include "Core/Object/ObjectMacros.h"
#include "AABB.generated.h"


namespace Lumina
{
    REFLECT()
    struct SAABB
    {
        GENERATED_BODY()
        
        PROPERTY(Editable)
        FVector3 Min;

        PROPERTY(Editable)
        FVector3 Max;
        
        constexpr SAABB()
            : Min(0.0f), Max(0.0f)
        {}

        constexpr SAABB(const FVector3& InMin, const FVector3& InMax)
            : Min(InMin), Max(InMax)
        {}

        // False for both a default-constructed box and the inverted min/max a failed build leaves behind.
        FORCEINLINE constexpr bool IsValid() const { return Max.x > Min.x || Max.y > Min.y || Max.z > Min.z; }

        FUNCTION()
        FORCEINLINE constexpr float MaxScale() const { return Math::Max(GetSize().x, Math::Max(GetSize().y, GetSize().z)); }
        
        FUNCTION()
        FORCEINLINE constexpr FVector3 GetSize() const { return Max - Min; }
        
        FUNCTION()
        FORCEINLINE constexpr FVector3 GetCenter() const { return Min + GetSize() * 0.5f; }
        
        NODISCARD constexpr SAABB ToWorld(const FMatrix4& World) const
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

            return SAABB(NewMin, NewMax);
        }
    };
}
