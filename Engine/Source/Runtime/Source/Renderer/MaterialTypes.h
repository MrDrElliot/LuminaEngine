#pragma once
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Math.h"
#include "Shared/SharedConstants.h"
#include "MaterialTypes.generated.h"

namespace Lumina
{
    // Must match EMaterialFlags in Common.slang.
    enum class EMaterialGPUFlags : uint32
    {
        None        = 0,
        Masked      = 1 << 0,
        Translucent = 1 << 1,
        Additive    = 1 << 2,
        Unlit       = 1 << 3,
    };

    ENUM_CLASS_FLAGS(EMaterialGPUFlags);

    struct FMaterialUniforms
    {
        FVector4    Vectors[MAX_VECTORS];
        float       Scalars[MAX_SCALARS];
        uint32      Textures[MAX_TEXTURES];
        uint32      Flags;
        float       OpacityClipValue;
        uint32      Padding[2];
    };
    
    REFLECT()
    enum class EMaterialParameterType : uint8
    {
        Scalar,
        Vector,
        Texture,
    };

    REFLECT()
    struct RUNTIME_API FMaterialParameter
    {
        GENERATED_BODY()

        PROPERTY()
        FName ParameterName;

        PROPERTY()
        EMaterialParameterType Type;

        PROPERTY()
        uint16 Index;

        // Replayed into MaterialUniforms in PostLoad (uniform block isn't serialized).
        PROPERTY()
        float ScalarDefault = 0.0f;

        PROPERTY()
        FVector4 VectorDefault = FVector4(0.0f);
    };
}
