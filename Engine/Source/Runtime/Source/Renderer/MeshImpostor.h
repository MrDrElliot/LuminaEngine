#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Math/Vector/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Serialization/Archiver.h"
#include "Core/Versioning/CoreVersion.h"
#include "Lumina.h"
#include "MeshImpostor.generated.h"

namespace Lumina
{
    // How the next bake captures a mesh's impostor and how soon its instances swap to it, kept on CMesh.
    REFLECT()
    struct RUNTIME_API SImpostorSettings
    {
        GENERATED_BODY()

        // Views along each side of the atlas, so 4 bakes 16 views where the default 8 bakes 64, enough for a rock seen only from afar.
        PROPERTY(Editable, Category = "Impostor", ClampMin = "3", ClampMax = "16")
        uint32 Views = 8;

        // Texels along each side of one view, rounded down to a power of two and shrunk until the atlas fits 4096.
        PROPERTY(Editable, Category = "Impostor", ClampMin = "16", ClampMax = "512")
        uint32 ViewResolution = 128;

        // Bakes views from below as well, for meshes seen from underneath such as hanging or floating props.
        PROPERTY(Editable, Category = "Impostor")
        bool bFullSphere = false;

        // Multiplies the screen size this mesh swaps under, so 2 swaps twice as near and 0.5 keeps the mesh until it is half as large.
        PROPERTY(Editable, Category = "Impostor", ClampMin = "0.25", ClampMax = "4.0")
        float TransitionScale = 1.0f;
    };

    // Views of a whole mesh from directions over its upper hemisphere or the whole sphere, which far instances draw in place of their meshlets.
    struct FMeshImpostor
    {
        // BC7 blocks of the smallest mip a compressed atlas keeps, which is one block per view.
        static constexpr uint32 kBlockSide = 4;

        // Views along each side of the atlas, 0 for no impostor.
        uint32         Frames    = 0;
        // Texels along each side of one view, a power of two.
        uint32         FrameSize = 0;
        // The mesh-local sphere every view frames.
        FVector3       Center    = FVector3(0.0f);
        float          Radius    = 0.0f;
        // BC7 blocks at a byte per texel rather than RGBA8 rows, a quarter of the memory.
        bool           bCompressed = false;
        // Views cover the whole octahedron rather than the hemisphere above the horizon.
        bool           bFullSphere = false;
        // Brightest emission the bake saw, which the emissive atlas stores fractions of.
        float          EmissiveScale = 0.0f;
        // The mesh's TransitionScale setting, copied in at load and on edit rather than serialized.
        float          TransitionMultiplier = 1.0f;
        // The whole atlas, mip 0 then every mip down to one texel per view, base color with coverage in alpha.
        TVector<uint8> BaseColor;
        // The same layout holding the mesh-local octahedral normal, the depth toward the view across the sphere, and ambient occlusion.
        TVector<uint8> NormalDepth;
        // The same layout holding the square root of emission over EmissiveScale, empty when the bake saw none.
        TVector<uint8> Emissive;

        uint32 GetAtlasSize() const { return Frames * FrameSize; }
        // A compressed atlas stops where a view is one block wide, since a smaller view would share blocks with its neighbors.
        uint32 GetMipCount() const
        {
            const uint32 Smallest = bCompressed ? kBlockSide : 1u;
            uint32 Mips = 0;
            for (uint32 Side = FrameSize; Side >= Smallest && Side > 0u; Side >>= 1u)
            {
                ++Mips;
            }
            return Mips;
        }

        uint32 GetBytesPerTexel() const { return bCompressed ? 1u : 4u; }

        uint64 GetMipOffset(uint32 Mip) const
        {
            uint64 Bytes = 0;
            for (uint32 Level = 0; Level < Mip; ++Level)
            {
                const uint64 Side = GetAtlasSize() >> Level;
                Bytes += Side * Side * GetBytesPerTexel();
            }
            return Bytes;
        }

        uint64 GetByteCount() const { return GetMipOffset(GetMipCount()); }

        bool IsValid() const
        {
            return Frames >= 2u && FrameSize > 0u && Radius > 0.0f
                && (uint64)BaseColor.Num() == GetByteCount() && (uint64)NormalDepth.Num() == GetByteCount()
                && (Emissive.IsEmpty() || ((uint64)Emissive.Num() == GetByteCount() && EmissiveScale > 0.0f));
        }

        bool HasEmissive() const { return !Emissive.IsEmpty(); }

        void Reset() { *this = FMeshImpostor(); }

        friend FArchive& operator<<(FArchive& Ar, FMeshImpostor& Data)
        {
            Ar << Data.Frames;
            Ar << Data.FrameSize;
            Ar << Data.Center;
            Ar << Data.Radius;
            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::MESH_IMPOSTOR_BC7)
            {
                Ar << Data.bCompressed;
            }
            else if (Ar.IsReading())
            {
                Data.bCompressed = false;
            }
            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::MESH_IMPOSTOR_SPHERE_EMISSIVE)
            {
                Ar << Data.bFullSphere;
                Ar << Data.EmissiveScale;
            }
            Ar << Data.BaseColor;
            Ar << Data.NormalDepth;
            if (Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::MESH_IMPOSTOR_SPHERE_EMISSIVE)
            {
                Ar << Data.Emissive;
            }
            return Ar;
        }
    };

    // Twin of Includes/Impostor.slang, so the bake and the draw agree on every view.
    namespace Impostor
    {
        // A mesh this dense swaps at exactly the renderer's impostor threshold.
        inline constexpr float kReferenceTriangles   = 500.0f;
        inline constexpr float kMinTransitionScale   = 0.5f;
        inline constexpr float kMaxTransitionScale   = 4.0f;
        // How far a view may be stretched on screen before a denser mesh stops swapping sooner, since past it the impostor blurs.
        inline constexpr float kMaxMagnification     = 1.5f;

        // Grows with the square root of the triangles, so every mesh swaps once it would put the same number of triangles on each pixel.
        FORCEINLINE float TransitionScale(uint32 Triangles)
        {
            return Math::Clamp(Math::Sqrt((float)Triangles / kReferenceTriangles), kMinTransitionScale, kMaxTransitionScale);
        }

        // Mirrored by ImpostorTransitionPixels in Impostor.slang, which the cull swaps against.
        FORCEINLINE float TransitionPixels(float BasePixels, float Scale, uint32 FrameSize)
        {
            const float Scaled = BasePixels * Scale;
            return Scale > 1.0f ? Math::Min(Scaled, Math::Max(BasePixels, (float)FrameSize * kMaxMagnification)) : Scaled;
        }

        // Turns an RGBA8 atlas into BC7 in place, dropping the mips smaller than a block per view.
        RUNTIME_API void Compress(FMeshImpostor& Impostor);

        // Y up. A hemisphere puts the horizon on the square's border, a full sphere folds the lower half into the corners.
        FORCEINLINE FVector3 DecodeDirection(const FVector2& GridUV, bool bFullSphere)
        {
            const FVector2 P = GridUV * 2.0f - FVector2(1.0f);
            if (!bFullSphere)
            {
                const float X = (P.x + P.y) * 0.5f;
                const float Z = (P.x - P.y) * 0.5f;
                return Math::Normalize(FVector3(X, Math::Max(1.0f - Math::Abs(X) - Math::Abs(Z), 0.0f), Z));
            }
            const float Y = 1.0f - Math::Abs(P.x) - Math::Abs(P.y);
            if (Y >= 0.0f)
            {
                return Math::Normalize(FVector3(P.x, Y, P.y));
            }
            return Math::Normalize(FVector3((1.0f - Math::Abs(P.y)) * (P.x >= 0.0f ? 1.0f : -1.0f), Y,
                                            (1.0f - Math::Abs(P.x)) * (P.y >= 0.0f ? 1.0f : -1.0f)));
        }

        // The direction of view (I, J), whose grid holds a view on every vertex.
        FORCEINLINE FVector3 FrameDirection(uint32 I, uint32 J, uint32 Frames, bool bFullSphere)
        {
            const float Step = 1.0f / (float)(Frames - 1u);
            return DecodeDirection(FVector2((float)I * Step, (float)J * Step), bFullSphere);
        }

        // The view's image axes, the shortest-arc turn from +Y onto Dir applied to +X and -Z, or from -Y with X mirrored below the horizon so each stays a rotation.
        FORCEINLINE void FrameBasis(const FVector3& Dir, FVector3& OutRight, FVector3& OutUp)
        {
            if (Dir.y >= 0.0f)
            {
                const float K = 1.0f / (1.0f + Dir.y);
                OutRight = FVector3(1.0f - K * Dir.x * Dir.x, -Dir.x, -K * Dir.x * Dir.z);
                OutUp    = FVector3(K * Dir.x * Dir.z, Dir.z, K * Dir.z * Dir.z - 1.0f);
                return;
            }
            const float K = 1.0f / (1.0f - Dir.y);
            OutRight = FVector3(K * Dir.x * Dir.x - 1.0f, -Dir.x, K * Dir.x * Dir.z);
            OutUp    = FVector3(K * Dir.x * Dir.z, -Dir.z, K * Dir.z * Dir.z - 1.0f);
        }
    }
}
