#pragma once

#include "Containers/Array.h"
#include "Core/Math/Vector/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Serialization/Archiver.h"
#include "Platform/Platform.h"
#include "MeshDistanceField.generated.h"

namespace Lumina
{
    struct FMeshResource;
    class FScopedSlowTask;
}

namespace Lumina
{
    /** Build inputs for a mesh distance field. Lives on CMesh so a rebuild reproduces the import result. */
    REFLECT()
    struct RUNTIME_API SDistanceFieldBuildSettings
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Distance Field")
        bool bEnabled = false;

        PROPERTY(Editable, Category = "Distance Field", ClampMin = "4", ClampMax = "256")
        uint32 Resolution = 48;

        PROPERTY(Editable, Category = "Distance Field", ClampMin = "0.01", ClampMax = "1.0")
        float NarrowBandScale = 0.2f;

        PROPERTY(Editable, Category = "Distance Field")
        bool bTwoSided = false;

        PROPERTY(Editable, Category = "Distance Field", ClampMin = "0", ClampMax = "3")
        uint32 SourceLOD = 0;
    };

    /** A built field. Not reflected: it is bulk payload, serialized by hand alongside FMeshResource. */
    struct FDistanceFieldVolume
    {
        // Voxel counts. Zero on any axis means "no field".
        FUIntVector3    Dimensions = FUIntVector3(0, 0, 0);

        // Mesh-local AABB the volume spans (the mesh bounds grown by the narrow band).
        FVector3        VolumeMin  = FVector3(0.0f);
        FVector3        VolumeSize = FVector3(0.0f);

        float           MaxDistance = 0.0f;

        // Unsigned build: values are distances only, encoded over [0, MaxDistance].
        bool            bTwoSided = false;

        // Dimensions.x * y * z bytes, X-major then Y then Z (matching the Tex3D upload order).
        TVector<uint8>  Distances;

        FORCEINLINE bool IsValid() const
        {
            return Dimensions.x > 0 && Dimensions.y > 0 && Dimensions.z > 0
                && Distances.size() == (size_t)Dimensions.x * Dimensions.y * Dimensions.z;
        }

        FORCEINLINE size_t GetSizeBytes() const { return Distances.size(); }

        friend FArchive& operator << (FArchive& Ar, FDistanceFieldVolume& Data)
        {
            Ar << Data.Dimensions;
            Ar << Data.VolumeMin;
            Ar << Data.VolumeSize;
            Ar << Data.MaxDistance;
            Ar << Data.bTwoSided;
            Ar << Data.Distances;
            return Ar;
        }
    };

    namespace DistanceField
    {
        constexpr uint32 kInvalidIndex = 0xFFFFFFFFu;

        RUNTIME_API bool Build(const FMeshResource& Resource,
                               const SDistanceFieldBuildSettings& Settings,
                               FDistanceFieldVolume& OutVolume,
                               FScopedSlowTask* Progress = nullptr);
    }
}
