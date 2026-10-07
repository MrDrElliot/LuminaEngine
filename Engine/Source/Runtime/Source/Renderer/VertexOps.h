#pragma once

#include "Core/Math/Vector/VectorTypes.h"
#include "Platform/GenericPlatform.h"

// Eight-wide forms of the vertex pack helpers in Vertex.h, bit-identical to calling them one element at a time.
namespace Lumina::VertexOps
{
    struct FSNorm16Normal
    {
        int16 X;
        int16 Y;
        int16 Z;
    };

    // UnpackNormal per element.
    RUNTIME_API void UnpackNormals(const uint32* Packed, FVector3* Out, size_t Count);

    // FloatToSNorm16 of each component of UnpackNormal, the form the meshlet vertex stores.
    RUNTIME_API void UnpackNormalsToSNorm16(const uint32* Packed, FSNorm16Normal* Out, size_t Count);

    // PackNormal per element.
    RUNTIME_API void PackNormals(const FVector3* Normals, uint32* Out, size_t Count);

    // PackTangent per element, reading xyz plus the handedness sign from four floats each.
    RUNTIME_API void PackTangents(const float* TangentsXYZW, uint32* Out, size_t Count);

    // Math::UnpackHalf2x16 per element.
    RUNTIME_API void UnpackHalf2x16s(const uint32* Packed, FVector2* Out, size_t Count);
}
