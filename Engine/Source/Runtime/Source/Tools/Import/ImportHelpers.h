#pragma once

#include <meshoptimizer.h>
#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Templates/Optional.h"
#include "Core/Utils/Expected.h"
#include "Memory/SmartPtr.h"
#include "Platform/Platform.h"
#include "Renderer/Format.h"
#include "Renderer/MeshDistanceField.h"
#include "Renderer/RHITexture.h"
#include "Assets/AssetTypes/Textures/Texture.h"

namespace Lumina
{
    struct FAnimationResource;
    struct FMeshResource;
    struct FSkeletonResource;
    struct FVertex;
    class FScopedSlowTask;
}

namespace Lumina::Import
{
    // Clean a raw source string (a file stem, a DCC material name, ...) into a tidy asset name: keep
    // [A-Za-z0-9], collapse every run of other characters (spaces, '.', '_', punctuation) into a single '_',
    // and trim leading/trailing separators. An empty result falls back to Fallback.
    inline FString SanitizeAssetName(FStringView In, FStringView Fallback = "Asset")
    {
        FString Out;
        Out.reserve(In.size());
        bool bSep = false;
        for (char C : In)
        {
            const bool bKeep = (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9');
            if (bKeep)
            {
                if (bSep && !Out.empty())
                {
                    Out.push_back('_');
                }
                Out.push_back(C);
                bSep = false;
            }
            else
            {
                bSep = true;
            }
        }
        if (Out.empty())
        {
            Out.assign(Fallback.data(), Fallback.size());
        }
        return Out;
    }

    // Sanitized name with Prefix (e.g. "T_") prepended, unless it already begins with Prefix (case-insensitive)
    // so a re-import or an already-conventionally-named source never becomes "T_T_Foo".
    inline FString MakeAssetName(FStringView Prefix, FStringView Raw, FStringView Fallback = "Asset")
    {
        FString Name = SanitizeAssetName(Raw, Fallback);

        bool bHasPrefix = Name.size() >= Prefix.size();
        for (size_t i = 0; bHasPrefix && i < Prefix.size(); ++i)
        {
            char A = Name[i];
            char B = Prefix[i];
            if (A >= 'a' && A <= 'z') { A -= 32; }
            if (B >= 'a' && B <= 'z') { B -= 32; }
            if (A != B) { bHasPrefix = false; }
        }
        if (bHasPrefix)
        {
            return Name;
        }

        FString Out(Prefix.data(), Prefix.size());
        Out.append(Name);
        return Out;
    }

    struct FImportSettings
    {
        virtual ~FImportSettings() = default;
        
        template<typename T>
        requires(eastl::is_base_of_v<FImportSettings, T> && !eastl::is_pointer_v<T>)
        const T& As() const
        {
            return *static_cast<const T*>(this);
        }
    };
    
    
    namespace Textures
    {
        struct FTextureImportResult
        {
            TVector<uint8>  Pixels;
            FUIntVector2      Dimensions;
            EFormat         Format;
        };
        
        /** Gets an image's raw pixel data */
        RUNTIME_API TOptional<FTextureImportResult> ImportTexture(FStringView RawFilePath, bool bFlipVertical = true, FUIntVector2 Size = {});
        RUNTIME_API TOptional<FTextureImportResult> ImportTexture(TSpan<const uint8> ImageData, bool bFlipVertical = true, FUIntVector2 Size = {});
    
        /** Decodes a file straight into the global texture heap; ImTextureID = ResourceID(). */
        NODISCARD RUNTIME_API RHI::FManagedTexture CreateTextureFromImport(FStringView RawFilePath, bool bFlipVerticalOnLoad = true, FUIntVector2 Size = {});
    }

    
    
    
    namespace Mesh
    {
        struct FMeshImportOptions
        {
            bool bOptimize          = true;
            bool bImportMaterials   = true;
            bool bImportTextures    = true;
            bool bImportMeshes      = true;
            bool bImportAnimations  = true;
            bool bImportSkeleton    = true;
            bool bFlipNormals       = false;
            bool bFlipUVs           = false;   // flips V (1 - V)
            bool bFlipU             = false;   // flips U (1 - U); for sources whose UVs are horizontally mirrored
            bool bMergeMeshes       = false;
            float Scale             = 1.0f;
            /** Skip heavy finalization and user transforms; dialog defers them to commit time. */
            bool bSkipFinalization  = false;

            /** Seeds CMesh::DistanceFieldSettings on every mesh the import creates, and drives the
             *  voxelisation pass in FinalizeMeshImportData. Off by default: the build is the single most
             *  expensive step of a mesh import, and most meshes never need a field. */
            SDistanceFieldBuildSettings DistanceField;
        };

        struct FMeshImportImage : FImportSettings
        {
            FFixedString         RelativePath;
            RHI::FManagedTexture DisplayImage;
            TVector<uint8>       Bytes;

            /** Semantic role from the mesh importer; Auto defers to filename heuristic. */
            ETextureColorSpace IntendedColorSpace = ETextureColorSpace::Auto;

            // Total basisu encode threads for this texture (includes the calling thread; 1 = single-threaded).
            // 0 = auto (use the worker count). A batch importer that cooks many textures in parallel sets this to
            // 1 so each texture doesn't also spawn a full encode pool and oversubscribe the cores. Not part of
            // identity (operator== / hasher ignore it).
            uint32 EncodeThreadBudget = 0;

            NODISCARD bool IsBytes() const { return !Bytes.empty(); }

            bool operator==(const FMeshImportImage& Other) const
            {
                return Other.RelativePath == RelativePath && Other.Bytes == Bytes;
            }
        };

        struct FMeshImportImageHasher
        {
            size_t operator()(const FMeshImportImage& Asset) const noexcept
            {
                size_t Seed = 0;
                Hash::HashCombine(Seed, Asset.RelativePath);
                Hash::HashCombine(Seed, Asset.Bytes.data());
                return Seed;
            }
        };
    
        struct FMeshImportImageEqual
        {
            bool operator()(const FMeshImportImage& A, const FMeshImportImage& B) const noexcept
            {
                return A.RelativePath == B.RelativePath && A.Bytes == B.Bytes;
            }
        };

        using FMeshImportTextureMap = THashSet<FMeshImportImage, FMeshImportImageHasher, FMeshImportImageEqual>;

        /** Alpha handling parsed from the source material; maps to EBlendMode at material-asset creation. */
        enum class EImportAlphaMode : uint8
        {
            Opaque,
            Mask,
            Blend,
        };

        /** One source-file material definition (PBR metallic-roughness). Indexed by the same value that
         *  FGeometrySurface::MaterialIndex references (for merge mode, see FMeshImportData::MergedMaterialSlotToSource).
         *  Texture slots store the source image's FMeshImportImage::RelativePath key so the committed CTexture
         *  asset can be resolved after import; an empty path means the channel has no texture. */
        struct FMeshImportMaterial
        {
            FString             Name;

            FVector4            BaseColorFactor   = FVector4(1.0f, 1.0f, 1.0f, 1.0f);
            float               MetallicFactor    = 1.0f;
            float               RoughnessFactor   = 1.0f;
            FVector3            EmissiveColor     = FVector3(0.0f, 0.0f, 0.0f);

            EImportAlphaMode    AlphaMode         = EImportAlphaMode::Opaque;
            float               AlphaCutoff       = 0.5f;
            bool                bTwoSided         = false;
            bool                bUnlit            = false;

            // RelativePath keys into FMeshImportData::Textures; empty = no texture for that channel.
            FFixedString        BaseColorTexture;
            FFixedString        MetallicRoughnessTexture;   // glTF packing: G = roughness, B = metallic.
            FFixedString        NormalTexture;
            FFixedString        EmissiveTexture;
            FFixedString        OcclusionTexture;
        };

        struct FMeshStatistics : INonCopyable
        {
            TVector<meshopt_OverdrawStatistics>         OverdrawStatics;
            TVector<meshopt_VertexFetchStatistics>      VertexFetchStatics;
        };

        struct FMeshImportData : FImportSettings
        {
            FMeshStatistics                             MeshStatistics;
            FMeshImportTextureMap                       Textures;
            TVector<TUniquePtr<FMeshResource>>          Resources;
            TVector<TUniquePtr<FAnimationResource>>     Animations;
            TVector<TUniquePtr<FSkeletonResource>>      Skeletons;

            /** Parsed source materials, indexed the same way FGeometrySurface::MaterialIndex references them
             *  (except in merge mode; see MergedMaterialSlotToSource). Empty when bImportMaterials is off. */
            TVector<FMeshImportMaterial>                Materials;

            /** Merge mode only: maps a merged mesh's material slot -> index into Materials. Empty otherwise
             *  (then a slot index is itself the Materials index). */
            TVector<int16>                              MergedMaterialSlotToSource;

            /** Populated by the dialog at commit; drives FinalizeMeshImportData and per-asset creation gates. */
            FMeshImportOptions                          CommitOptions;

            // Out-of-line (MeshImport.cpp): the dtor releases preview thumbnails, and all of
            // these need member TUniquePtrs' types complete, which they are not here.
            RUNTIME_API FMeshImportData();
            RUNTIME_API FMeshImportData(FMeshImportData&&) noexcept;
            RUNTIME_API FMeshImportData& operator=(FMeshImportData&&) noexcept;
            RUNTIME_API ~FMeshImportData() override;
        };

        RUNTIME_API void OptimizeNewlyImportedMesh(FMeshResource& MeshResource, FScopedSlowTask* Progress = nullptr);
        /** When Progress is set, advances StepPerSurface of progress for each surface meshletized. */
        RUNTIME_API void GenerateMeshlets(FMeshResource& MeshResource, FScopedSlowTask* Progress = nullptr, float StepPerSurface = 0.0f);
        RUNTIME_API void AnalyzeMeshStatistics(FMeshResource& MeshResource, FMeshStatistics& OutMeshStats);

        /** The screen threshold GenerateMeshlets bakes into LOD slot Index, so a tool offering
         *  "reset to default" resets to the same ramp the importer wrote instead of a second copy of the
         *  numbers. Returns 0 for LOD 0 (always active) and FLT_MAX past the last level.
         *
         *  Slot, not source level: a surface that skipped a level has its survivors compacted down, so a
         *  compacted slot was authored with a COARSER level's threshold than this returns. Reset therefore
         *  means "restore the standard ramp", not "undo back to the imported value". */
        RUNTIME_API float GetDefaultLODScreenThreshold(uint32 Index);

        /** Apply user transforms and run the heavy finalize passes on a parsed FMeshImportData.
         *  When Progress is set, advances a total of ProgressBudget across the finalize pass. */
        RUNTIME_API void FinalizeMeshImportData(FMeshImportData& Data, const FMeshImportOptions& Options, FScopedSlowTask* Progress = nullptr, float ProgressBudget = 1.0f);

        // Model-format parsers (ImportOBJ/FBX/GLTF) are editor-only (Editor's MeshFormatImport.h);
        // they pull tinyobjloader/OpenFBX/fastgltf, which don't ship in the Game runtime.
    }

}
