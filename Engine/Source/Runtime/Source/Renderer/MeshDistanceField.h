#pragma once

#include "Containers/Array.h"
#include "Core/Math/Vector/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Serialization/Archiver.h"
#include "Platform/Platform.h"
#include "MeshDistanceField.generated.h"

// Per-mesh signed distance field: a voxel volume, in MESH-LOCAL space, holding the signed distance
// from each voxel centre to the nearest surface. Negative is inside the mesh, positive outside.
//
// The field is built from the mesh's own baked LOD-0 meshlets (not the import scratch), which is what
// lets a rebuild run at any time from a loaded asset -- FMeshResource drops Positions/Indices once the
// GPU buffers exist, but MeshletData survives for the life of the mesh.
//
// The volume is stored as 8-bit UNORM over [-MaxDistance, +MaxDistance] and uploaded to a bindless
// Tex3D, so a material samples it with one filtered fetch. 8 bits over a narrow band is the standard
// tradeoff: the field is only accurate near the surface anyway (that is where the interesting values
// are), and quantising the band rather than the whole volume keeps the step below a tenth of a voxel.

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

        /** Master gate. Off means the mesh carries no field and every SDF material node reading it
         *  reports "not valid" rather than sampling garbage. */
        PROPERTY(Editable, Category = "Distance Field")
        bool bEnabled = false;

        /** Voxels along the volume's LONGEST axis; the other two are derived so voxels stay cubic.
         *  Build cost is cubic in this, memory is exactly Resolution^3 bytes at the worst aspect ratio. */
        PROPERTY(Editable, Category = "Distance Field", ClampMin = "4", ClampMax = "256")
        uint32 Resolution = 48;

        /** Width of the accurate band, as a fraction of the mesh's longest extent. Doubles as the
         *  margin the volume is grown past the mesh AABB, so the field always has room to reach
         *  +MaxDistance before it runs out of volume -- otherwise the outermost voxels clamp early and
         *  the gradient at the boundary points the wrong way. */
        PROPERTY(Editable, Category = "Distance Field", ClampMin = "0.01", ClampMax = "1.0")
        float NarrowBandScale = 0.2f;

        /** Thin/open geometry (foliage cards, cloth, anything not closed) has no meaningful inside, and
         *  the inside/outside vote on it produces speckle. This stores UNSIGNED distance instead: every
         *  value is >= 0 and the sign test is skipped entirely, which also makes the build much faster. */
        PROPERTY(Editable, Category = "Distance Field")
        bool bTwoSided = false;

        /** Which baked LOD supplies the triangles. A distance field is low-frequency by construction, so
         *  a coarser level usually voxelises identically for a fraction of the build cost. Levels 4-5 are
         *  the sloppy simplifier's and can contain holes, which the inside/outside vote reads as real
         *  openings, so this is clamped to the topology-preserving range. */
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

        // Local-space distance the encoded range [0,1] spans, signed: 0 encodes -MaxDistance, 1 encodes
        // +MaxDistance, 0.5 is the surface. Two-sided fields use [0, MaxDistance] instead (see bTwoSided).
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
        /** Sentinel written into FMeshletHeaderGPU::DistanceFieldIndex when a mesh has no field.
         *  Mirrored by kInvalidDistanceField in Includes/DistanceField.slang. */
        constexpr uint32 kInvalidIndex = 0xFFFFFFFFu;

        /** Voxelise Resource's baked meshlets into OutVolume. Returns false (leaving OutVolume cleared)
         *  when the mesh has no geometry at the requested LOD, or Settings.bEnabled is off.
         *
         *  Parallel internally (one task per Z slice), so callers must NOT wrap this in their own
         *  ParallelFor over meshes. Safe to call off the main thread; touches no GPU state. */
        RUNTIME_API bool Build(const FMeshResource& Resource,
                               const SDistanceFieldBuildSettings& Settings,
                               FDistanceFieldVolume& OutVolume,
                               FScopedSlowTask* Progress = nullptr);
    }
}
