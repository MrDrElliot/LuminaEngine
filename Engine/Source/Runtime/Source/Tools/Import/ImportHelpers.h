#pragma once

#include <meshoptimizer.h>
#include "Containers/Span.h"
#include "Containers/Vector.h"
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
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    struct FAnimationResource;
    struct FMeshResource;
    struct FSkeletonResource;
    struct FSourceVertex;
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
        requires(std::is_base_of_v<FImportSettings, T> && !std::is_pointer_v<T>)
        const T& As() const
        {
            return *static_cast<const T*>(this);
        }
    };
    
    
    namespace Textures
    {
        struct FTextureImportResult
        {
            TVector<uint8>      Pixels;
            FUIntVector2        Dimensions;
            EFormat             Format;
        };
        
        /** One texture cook. Bytes win when present (mesh-embedded); otherwise the file at SourcePath is read. */
        struct FTextureCookRequest
        {
            TSpan<const uint8> EmbeddedBytes;
            FFixedString       SourcePath;
            ETextureColorSpace ColorSpace         = ETextureColorSpace::Auto;
            uint32             EncodeThreadBudget = 0;

            /**
             * Whether to also create the GPU image and upload the cooked mips. An importer wants this off:
             * the package only needs the CPU mip chain, CTexture::PostLoad creates the image on first real
             * use, and uploads queued for an image the import is about to destroy fault the copy engine.
             */
            bool bCreateGPUResource = true;
        };

        /** Resamples an imported image to TargetSize in place. No-op for an unsupported layout or a no-op size. */
        RUNTIME_API void ResizeImportResult(FTextureImportResult& Source, FUIntVector2 TargetSize);

        /** Reverses row order in place, for sources whose rows run bottom-up. */
        RUNTIME_API void FlipImportResultVertical(FTextureImportResult& Source);

        /** Reverses texel order within each row, in place. */
        RUNTIME_API void FlipImportResultHorizontal(FTextureImportResult& Source);

        /** Size with its longest edge capped at MaxDimension, aspect preserved. MaxDimension 0 means no cap. */
        RUNTIME_API FUIntVector2 ClampToMaxDimension(FUIntVector2 Size, uint32 MaxDimension);

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
             *  voxelization pass in FinalizeMeshImportData. Off by default: the build is the single most
             *  expensive step of a mesh import, and most meshes never need a field. */
            SDistanceFieldBuildSettings DistanceField;
        };

        /** One source image, deduplicated by Key during parse. Embedded payloads are a VIEW into the
         *  parser's own buffer (Bytes) so a GLB's textures are never copied; a parser that cannot keep its
         *  buffer alive fills OwnedBytes instead and points Bytes at it. */
        struct FSourceImage
        {
            FFixedString         Key;             // stable identity: source URI, or "<file>_Image_<n>"
            FFixedString         ResolvedPath;    // on-disk path; empty when the payload is embedded
            TVector<uint8>       OwnedBytes;
            TSpan<const uint8>   Bytes;
            RHI::FManagedTexture Thumbnail;

            /** Semantic role from the mesh importer; Auto defers to the filename heuristic. */
            ETextureColorSpace IntendedColorSpace = ETextureColorSpace::Auto;

            // Total basisu encode threads for this texture (includes the calling thread; 1 = single-threaded).
            // 0 = auto. A batch cooking many textures at once passes 1 so each doesn't spawn its own pool.
            uint32 EncodeThreadBudget = 0;

            NODISCARD bool IsEmbedded() const { return !Bytes.empty(); }

            void AdoptOwnedBytes() { Bytes = TSpan<const uint8>(OwnedBytes.data(), OwnedBytes.size()); }
        };

        /** Alpha handling parsed from the source material; maps to EBlendMode at material-asset creation. */
        enum class EImportAlphaMode : uint8
        {
            Opaque,
            Mask,
            Blend,
        };

        /** One source-file material definition (PBR metallic-roughness). Indexed by the same value that
         *  FGeometrySurface::MaterialIndex references (for merge mode, see FMeshImportData::MergedMaterialSlotToSource).
         *  Texture slots are indices into FMeshImportData::Images; INDEX_NONE means the channel has no texture. */
        /**
         * Filtering + address mode for one texture slot. Values mirror EMaterialSampler in the editor's
         * material graph and RHI::EStockSampler; kept as a plain uint8 here so the runtime import types do
         * not depend on the editor node headers.
         */
        enum class EImportSampler : uint8
        {
            LinearWrap   = 0,
            LinearClamp  = 1,
            LinearMirror = 2,
            PointWrap    = 3,
            PointClamp   = 4,
            AnisoWrap    = 5,
            AnisoClamp   = 6,
        };

        /** Per-texture-slot UV handling: which set to sample and the KHR_texture_transform applied to it. */
        struct FTextureUVTransform
        {
            /** TEXCOORD set index the slot samples. */
            uint32   TexCoordSet = 0;

            FVector2 Offset      = FVector2(0.0f, 0.0f);
            FVector2 Scale       = FVector2(1.0f, 1.0f);

            /** Radians, counter-clockwise about the UV origin. */
            float    Rotation    = 0.0f;

            NODISCARD bool IsIdentity() const
            {
                return TexCoordSet == 0
                    && Offset == FVector2(0.0f, 0.0f)
                    && Scale  == FVector2(1.0f, 1.0f)
                    && Rotation == 0.0f;
            }
        };

        /** Texture slots on FMeshImportMaterial, in the order UVTransforms indexes them. */
        enum class EMaterialTextureSlot : uint8
        {
            BaseColor = 0,
            MetallicRoughness,
            Normal,
            Emissive,
            Occlusion,

            /** FBX sources author metalness and roughness as two separate single-channel maps rather than
             *  glTF's packed ORM, so both have their own slot. Used only when MetallicRoughness is unset. */
            Metallic,
            Roughness,

            Count,
        };

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

            int32               BaseColorImage         = INDEX_NONE;
            int32               MetallicRoughnessImage = INDEX_NONE;   // glTF packing: G = roughness, B = metallic.
            int32               NormalImage            = INDEX_NONE;
            int32               EmissiveImage          = INDEX_NONE;
            int32               OcclusionImage         = INDEX_NONE;

            /** Separate single-channel maps, read from the R channel. Set only by sources that author
             *  metalness and roughness as two textures (FBX); ignored when MetallicRoughnessImage is set. */
            int32               MetallicImage          = INDEX_NONE;
            int32               RoughnessImage         = INDEX_NONE;

            /**
             * Refractive index (KHR_materials_ior). Drives the dielectric F0, which the engine expresses as
             * its Specular term. 1.5 is the glTF default and matches the engine's Specular = 0.5.
             */
            float               IOR               = 1.5f;

            /** KHR_materials_specular: scales the dielectric reflectance computed from IOR. */
            float               SpecularFactor    = 1.0f;

            /** normal_texture.scale -- multiplies the tangent-space normal's XY. */
            float               NormalScale       = 1.0f;

            /** occlusion_texture.strength -- lerp(1, AO, strength). */
            float               OcclusionStrength = 1.0f;

            /** KHR_materials_clearcoat. Non-zero strength selects the Clearcoat shading model. */
            float               ClearcoatFactor    = 0.0f;
            float               ClearcoatRoughness = 0.0f;

            /** Indexed by EMaterialTextureSlot. Identity unless the source authored a UV set or transform. */
            FTextureUVTransform UVTransforms[(size_t)EMaterialTextureSlot::Count];

            /** Indexed by EMaterialTextureSlot. */
            EImportSampler      Samplers[(size_t)EMaterialTextureSlot::Count] = {};

            NODISCARD const FTextureUVTransform& GetUVTransform(EMaterialTextureSlot Slot) const
            {
                return UVTransforms[(size_t)Slot];
            }
        };

        /** The resources one deduplicated source mesh produced. A source mesh whose primitives are a mix of
         *  skinned and unskinned yields both. */
        struct FSourceMeshSlot
        {
            int32 StaticResource  = INDEX_NONE;
            int32 SkinnedResource = INDEX_NONE;
        };

        /** One placement of a source mesh in the scene graph, keyed by FMeshImportData::MeshSlots. Kept flat
         *  and POD so a 200k-node scene costs a single contiguous array rather than an object per node. */
        struct FSourceMeshInstance
        {
            uint32   SlotIndex;
            uint32   NodeIndex;
            FMatrix4 WorldTransform;
        };

        /** What a scene node contributes. A node with none of these is still kept when a descendant needs
         *  its transform. */
        enum class ESourceNodeKind : uint8
        {
            Empty,
            Mesh,
            PointLight,
            SpotLight,
            DirectionalLight,
            Camera,
        };

        /** How a source expresses light intensity, which decides how it converts onto engine units. */
        enum class ESourceLightUnits : uint8
        {
            /** glTF's KHR_lights_punctual units: lux for directional, candela for point and spot. */
            Photometric,

            /** Intensity with no absolute meaning, so it anchors to the strongest light of its kind in the file. */
            Relative,
        };

        /** Source light parameters. Angles are radians from the cone axis, ranges are meters. */
        struct FSourceLight
        {
            FVector3          Color          = FVector3(1.0f);
            float             Intensity      = 1.0f;
            float             Range          = 0.0f;
            float             InnerConeAngle = 0.0f;
            float             OuterConeAngle = 0.0f;

            /** The direction the light emits in its own node's local space. glTF is -Z, FBX is usually -Y. */
            FVector3          LocalDirection = FVector3(0.0f, 0.0f, -1.0f);

            ESourceLightUnits Units          = ESourceLightUnits::Photometric;
        };

        /** Source camera parameters, in glTF units (radians, meters). */
        struct FSourceCamera
        {
            bool  bOrthographic = false;

            /** Vertical FOV. Default is 50 degrees, which is Blender's own default lens. */
            float YFov          = 0.872664626f;
            float ZNear         = 0.1f;

            /** 0 when the source left the far plane infinite. */
            float ZFar          = 0.0f;

            /** Orthographic mode only: the world-space width the viewport spans. */
            float OrthoWidth    = 0.0f;
        };

        /** One node of the source scene graph, flattened parents-before-children so a consumer can build a
         *  hierarchy in a single forward pass. Transforms stay local: the parent chain reproduces the world
         *  placement, which is what an entity hierarchy wants. */
        struct FSourceSceneNode
        {
            FName           Name;
            int32           ParentIndex = INDEX_NONE;

            FVector3        Translation = FVector3(0.0f);
            FQuat           Rotation    = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
            FVector3        Scale       = FVector3(1.0f);

            ESourceNodeKind Kind      = ESourceNodeKind::Empty;

            /** Index into FMeshImportData::MeshSlots when Kind is Mesh. */
            int32           MeshSlot  = INDEX_NONE;

            FSourceLight    Light;
            FSourceCamera   Camera;
        };

        struct FMeshStatistics : INonCopyable
        {
            TVector<meshopt_OverdrawStatistics>         OverdrawStatics;
            TVector<meshopt_VertexFetchStatistics>      VertexFetchStatics;
        };

        struct FMeshImportData : FImportSettings
        {
            FMeshStatistics                             MeshStatistics;

            /** Deduplicated source images. Materials reference these by index. */
            TVector<FSourceImage>                       Images;

            TVector<TUniquePtr<FMeshResource>>          Resources;
            TVector<TUniquePtr<FAnimationResource>>     Animations;
            TVector<TUniquePtr<FSkeletonResource>>      Skeletons;

            /** Parsed source materials, indexed the same way FGeometrySurface::MaterialIndex references them
             *  (except in merge mode; see MergedMaterialSlotToSource). Empty when bImportMaterials is off. */
            TVector<FMeshImportMaterial>                Materials;

            /** Every scene-graph placement of a mesh, in discovery order. Resources are the deduplicated
             *  geometry those placements share. Empty for sources with no scene graph. */
            TVector<FSourceMeshInstance>                MeshInstances;

            /** Deduplicated source meshes, in the order MeshInstances::SlotIndex references them. */
            TVector<FSourceMeshSlot>                    MeshSlots;

            /** The source scene graph, parents before children. Empty for formats with no hierarchy. */
            TVector<FSourceSceneNode>                   SceneNodes;

            /** Merge mode only: maps a merged mesh's material slot -> index into Materials. Empty otherwise
             *  (then a slot index is itself the Materials index). */
            TVector<int16>                              MergedMaterialSlotToSource;

            /** Populated by the dialog at commit; drives FinalizeMeshImportData and per-asset creation gates. */
            FMeshImportOptions                          CommitOptions;

            /** Source-node count before dedup, for the import report. */
            uint32                                      SourceNodeCount = 0;

            /**
             * True when at least one primitive in the source carried a vertex-color attribute.
             *
             * Drives whether generated materials sample vertex color at all. It has to be an ALL-OR-NOTHING
             * property of the import rather than of a material, because a material is shared across meshes
             * and cannot know which of them are colored -- but every importer fills opaque white where the
             * source has no attribute, so the multiply is a no-op on the meshes that lack one.
             *
             * Left false by sources that cannot carry the attribute, which keeps their masters node-for-node
             * what they were.
             */
            bool                                        bHasVertexColors = false;

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

        // Model-format parsers live in the editor's CImporter hierarchy; they pull
        // tinyobjloader/ufbx/cgltf, which don't ship in the Game runtime.
    }

}
