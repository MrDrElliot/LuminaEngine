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
            bool bFlipUVs           = false;
            bool bMergeMeshes       = false;
            float Scale             = 1.0f;
            /** Skip heavy finalization and user transforms; dialog defers them to commit time. */
            bool bSkipFinalization  = false;
        };

        struct FMeshImportImage : FImportSettings
        {
            FFixedString         RelativePath;
            RHI::FManagedTexture DisplayImage;
            TVector<uint8>       Bytes;

            /** Semantic role from the mesh importer; Auto defers to filename heuristic. */
            ETextureColorSpace IntendedColorSpace = ETextureColorSpace::Auto;

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

        /** Apply user transforms and run the heavy finalize passes on a parsed FMeshImportData.
         *  When Progress is set, advances a total of ProgressBudget across the finalize pass. */
        RUNTIME_API void FinalizeMeshImportData(FMeshImportData& Data, const FMeshImportOptions& Options, FScopedSlowTask* Progress = nullptr, float ProgressBudget = 1.0f);

        // Model-format parsers (ImportOBJ/FBX/GLTF) are editor-only (Editor's MeshFormatImport.h);
        // they pull tinyobjloader/OpenFBX/fastgltf, which don't ship in the Game runtime.
    }

}
