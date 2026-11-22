#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Math/Vector/Vector.h"
#include "Core/Serialization/Archiver.h"
#include "Core/Versioning/CoreVersion.h"

namespace Lumina
{
    // Views of a whole mesh from directions over its upper hemisphere, which far instances draw in place of their meshlets.
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
        // The whole atlas, mip 0 then every mip down to one texel per view, base color with coverage in alpha.
        TVector<uint8> BaseColor;
        // The same layout holding the mesh-local octahedral normal, the depth toward the view across the sphere, and ambient occlusion.
        TVector<uint8> NormalDepth;

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
                && (uint64)BaseColor.Num() == GetByteCount() && (uint64)NormalDepth.Num() == GetByteCount();
        }

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
            Ar << Data.BaseColor;
            Ar << Data.NormalDepth;
            return Ar;
        }
    };

    // Twin of Includes/Impostor.slang, so the bake and the draw agree on every view.
    namespace Impostor
    {
        // Turns an RGBA8 atlas into BC7 in place, dropping the mips smaller than a block per view.
        RUNTIME_API void Compress(FMeshImpostor& Impostor);

        // Hemi-octahedral, Y up, the square's border on the horizon and its center straight above.
        FORCEINLINE FVector3 DecodeDirection(const FVector2& GridUV)
        {
            const FVector2 P = GridUV * 2.0f - FVector2(1.0f);
            const float X = (P.x + P.y) * 0.5f;
            const float Z = (P.x - P.y) * 0.5f;
            return Math::Normalize(FVector3(X, Math::Max(1.0f - Math::Abs(X) - Math::Abs(Z), 0.0f), Z));
        }

        // The direction of view (I, J), whose grid holds a view on every vertex.
        FORCEINLINE FVector3 FrameDirection(uint32 I, uint32 J, uint32 Frames)
        {
            const float Step = 1.0f / (float)(Frames - 1u);
            return DecodeDirection(FVector2((float)I * Step, (float)J * Step));
        }

        // The view's image axes, the shortest-arc turn from +Y onto Dir applied to +X and -Z, smooth everywhere above the horizon.
        FORCEINLINE void FrameBasis(const FVector3& Dir, FVector3& OutRight, FVector3& OutUp)
        {
            const float K = 1.0f / (1.0f + Dir.y);
            OutRight = FVector3(1.0f - K * Dir.x * Dir.x, -Dir.x, -K * Dir.x * Dir.z);
            OutUp    = FVector3(K * Dir.x * Dir.z, Dir.z, K * Dir.z * Dir.z - 1.0f);
        }
    }
}
