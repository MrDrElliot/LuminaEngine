#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Platform/GenericPlatform.h"
#include "World/Entity/Components/BlockoutComponent.h"

namespace Lumina
{
    /** Streams a generated blockout primitive, in the shape's own local space. */
    struct FBlockoutMeshData
    {
        TVector<FVector3> Positions;
        TVector<FVector3> Normals;
        TVector<FVector2> UVs;
        TVector<uint32>   Indices;

        void Reset()
        {
            Positions.Reset();
            Normals.Reset();
            UVs.Reset();
            Indices.Reset();
        }

        NODISCARD bool IsEmpty() const { return Positions.IsEmpty() || Indices.IsEmpty(); }
    };

    namespace BlockoutMesh
    {
        /** Generates Shape into Out, replacing whatever it held. */
        RUNTIME_API void Build(const SBlockoutComponent& Shape, FBlockoutMeshData& Out);

        /** Covers every authored parameter and nothing transient, so it is the rebuild gate. */
        RUNTIME_API uint32 HashParameters(const SBlockoutComponent& Shape);

        /** Local-space box the generated mesh occupies, pivot applied. */
        RUNTIME_API void GetLocalBounds(const SBlockoutComponent& Shape, FVector3& OutMin, FVector3& OutMax);

        /** Menu label for one shape. */
        RUNTIME_API const char* GetShapeName(EBlockoutShape Shape);

        /** Number of entries in EBlockoutShape, for iterating the palette. */
        inline constexpr int32 ShapeCount = (int32)EBlockoutShape::Pipe + 1;
    }
}
