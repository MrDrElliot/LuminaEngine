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

        // Bits 3-5 are a 3-bit SHADING MODEL field holding EMaterialShadingModel, not one flag per model.
        // Unlit keeps its historical value for free: the enum value is 1 and the field starts at bit 3.
        Unlit       = 1 << 3,

        // Stored inverted so a material saved before the flag existed still reads as receiving decals.
        NoDecals    = 1 << 6,
    };

    ENUM_CLASS_FLAGS(EMaterialGPUFlags);

    // Mirrored by MATERIAL_SHADING_MODEL_SHIFT/MASK in Common.slang.
    constexpr uint32 kMaterialShadingModelShift = 3;
    constexpr uint32 kMaterialShadingModelMask  = 7;

    struct FMaterialUniforms
    {
        FVector4    Vectors[MAX_VECTORS];
        float       Scalars[MAX_SCALARS];
        uint32      Textures[MAX_TEXTURES];
        uint32      Flags;
        float       OpacityClipValue;

        /** Collection table slot per binding, resolved in PostLoad; 0 is the reserved zero collection. */
        uint32      CollectionIndices[MAX_MATERIAL_COLLECTIONS];
    };

    /** One collection's GPU block, shared by every material that binds it. */
    struct FMaterialCollectionUniforms
    {
        FVector4    Vectors[MAX_COLLECTION_VECTORS];
        float       Scalars[MAX_COLLECTION_SCALARS];
    };

    static_assert(sizeof(FMaterialCollectionUniforms) % 16 == 0, "FMaterialCollectionUniforms stride must stay 16-byte aligned");

    constexpr uint32 CollectionScalarFieldOffset(uint32 Index)
    {
        return (uint32)(offsetof(FMaterialCollectionUniforms, Scalars) + Index * sizeof(float));
    }

    constexpr uint32 CollectionVectorFieldOffset(uint32 Index)
    {
        return (uint32)(offsetof(FMaterialCollectionUniforms, Vectors) + Index * sizeof(FVector4));
    }

    // GetMaterialVec4 reaches Vectors with loadAligned<16>, which needs the element stride 16-aligned.
    static_assert(sizeof(FMaterialUniforms) % 16 == 0, "FMaterialUniforms stride must stay 16-byte aligned for loadAligned<16>");
    static_assert(offsetof(FMaterialUniforms, Vectors) % 16 == 0, "FMaterialUniforms::Vectors must stay 16-byte aligned");
    
    /** Byte offset of one parameter's field in the block, for targeted uploads. POD, so offsetof is exact. */
    constexpr uint32 ScalarFieldOffset(uint32 Index)
    {
        return (uint32)(offsetof(FMaterialUniforms, Scalars) + Index * sizeof(float));
    }

    constexpr uint32 VectorFieldOffset(uint32 Index)
    {
        return (uint32)(offsetof(FMaterialUniforms, Vectors) + Index * sizeof(FVector4));
    }

    constexpr uint32 TextureFieldOffset(uint32 Index)
    {
        return (uint32)(offsetof(FMaterialUniforms, Textures) + Index * sizeof(uint32));
    }

    constexpr uint32 CollectionIndexFieldOffset(uint32 Index)
    {
        return (uint32)(offsetof(FMaterialUniforms, CollectionIndices) + Index * sizeof(uint32));
    }

    // One bit per switch in a uint64 permutation key, so this is the cap the compiler enforces.
    constexpr uint32 kMaxStaticSwitches = 64;

    /** A compile-time branch an instance may flip, at the cost of compiling that instance its own shader. */
    REFLECT()
    struct RUNTIME_API FMaterialStaticSwitch
    {
        GENERATED_BODY()

        PROPERTY()
        FName ParameterName;

        PROPERTY()
        bool bDefaultValue = true;

        // Assigned by name order, so a graph edit that reorders nodes does not renumber the key.
        PROPERTY()
        uint8 BitIndex = 0;
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
