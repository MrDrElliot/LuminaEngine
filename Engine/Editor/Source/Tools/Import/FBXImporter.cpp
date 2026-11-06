#include "EditorPCH.h"
#include "FBXImporter.h"

#include <ufbx/ufbx.h>

#include "ImportDedup.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Core/Math/Math.h"
#include "Core/Progress/SlowTask.h"
#include "Core/Threading/Atomic.h"
#include "FileSystem/FileSystem.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkeletonResource.h"
#include "Renderer/Vertex.h"
#include "TaskSystem/TaskSystem.h"
#include "Log/Log.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    using namespace Import::Mesh;
    using namespace Import;

    namespace
    {
        //~ ufbx plumbing -------------------------------------------------------------------------------

        void* UfbxAlloc(void*, size_t Size)
        {
            LUMINA_MEMORY_SCOPE("Import");
            return Memory::Malloc(Size);
        }

        void UfbxFree(void*, void* Ptr, size_t)
        {
            if (Ptr != nullptr)
            {
                Memory::Free(Ptr);
            }
        }

        ufbx_allocator MakeTrackedAllocator()
        {
            ufbx_allocator Allocator = {};
            Allocator.alloc_fn = &UfbxAlloc;
            Allocator.free_fn  = &UfbxFree;
            return Allocator;
        }

        FORCEINLINE FStringView ToView(const ufbx_string& S)
        {
            return (S.data != nullptr) ? FStringView(S.data, IntCastChecked<int32>(S.length)) : FStringView();
        }

        FORCEINLINE FFixedString ToFixed(const ufbx_string& S)
        {
            return (S.data != nullptr && S.length > 0) ? FFixedString(S.data, IntCastChecked<int32>(S.length)) : FFixedString();
        }

        FORCEINLINE FVector2 ToVector2(const ufbx_vec2& V) { return FVector2((float)V.x, (float)V.y); }
        FORCEINLINE FVector3 ToVector3(const ufbx_vec3& V) { return FVector3((float)V.x, (float)V.y, (float)V.z); }
        FORCEINLINE FVector4 ToVector4(const ufbx_vec4& V) { return FVector4((float)V.x, (float)V.y, (float)V.z, (float)V.w); }
        FORCEINLINE FQuat    ToQuat(const ufbx_quat& Q)    { return FQuat((float)Q.w, (float)Q.x, (float)Q.y, (float)Q.z); }

        // ufbx_matrix is a column-major 4x3 affine; the missing row is (0,0,0,1).
        FMatrix4 ToMatrix4(const ufbx_matrix& M)
        {
            return FMatrix4(
                (float)M.m00, (float)M.m10, (float)M.m20, 0.0f,
                (float)M.m01, (float)M.m11, (float)M.m21, 0.0f,
                (float)M.m02, (float)M.m12, (float)M.m22, 0.0f,
                (float)M.m03, (float)M.m13, (float)M.m23, 1.0f);
        }

        FFixedString FormatUfbxError(const ufbx_error& Error)
        {
            char Buffer[512];
            ufbx_format_error(Buffer, sizeof(Buffer), &Error);
            return FFixedString(Buffer);
        }

        //~ Material helpers ----------------------------------------------------------------------------

        FORCEINLINE float MapReal(const ufbx_material_map& Map, float Default)
        {
            return Map.has_value ? (float)Map.value_real : Default;
        }

        FORCEINLINE FVector3 MapVec3(const ufbx_material_map& Map, const FVector3& Default)
        {
            return Map.has_value ? ToVector3(Map.value_vec3) : Default;
        }

        // 3ds Max wraps normal maps in a bump node, and file_textures is ufbx's flattened view of the files.
        const ufbx_texture* FindFileTexture(const ufbx_texture* Texture)
        {
            if (Texture == nullptr)
            {
                return nullptr;
            }
            if (Texture->has_file)
            {
                return Texture;
            }
            for (const ufbx_texture* Sub : Texture->file_textures)
            {
                if (Sub != nullptr && Sub->has_file)
                {
                    return Sub;
                }
            }
            return nullptr;
        }

        FORCEINLINE FQuat InverseAdjustRotation(const ufbx_node& Node)
        {
            return Node.has_adjust_transform ? Math::Inverse(ToQuat(Node.adjust_pre_rotation)) : FQuat(1.0f, 0.0f, 0.0f, 0.0f);
        }

        struct FEngineBasis
        {
            FQuat    Rotation        = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
            FQuat    InverseRotation = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
            FMatrix4 Matrix          = FMatrix4(1.0f);
            FMatrix4 InverseMatrix   = FMatrix4(1.0f);
            FMatrix3 Matrix3         = FMatrix3(1.0f);

            // Undoes ufbx's axis rotation on a world-space matrix, where it sits at the top-level ancestor.
            FMatrix4 InverseRootAdjust = FMatrix4(1.0f);

            explicit FEngineBasis(const ufbx_scene& Scene)
            {
                Rotation          = ToQuat(Scene.metadata.root_rotation);
                InverseRotation   = Math::Inverse(Rotation);
                Matrix            = Math::ToMatrix4(Rotation);
                InverseMatrix     = Math::ToMatrix4(InverseRotation);
                Matrix3           = Math::ToMatrix3(Rotation);
                InverseRootAdjust = InverseMatrix;
            }

            FMatrix4 ToEngine(const FMatrix4& M) const { return Matrix * M * InverseMatrix; }
            FMatrix4 WorldToEngine(const ufbx_matrix& M) const { return ToEngine(InverseRootAdjust * ToMatrix4(M)); }

            // Bone transforms are parent-relative and shared across files, so only root-level nodes take the basis.
            FMatrix4 BoneToEngine(const ufbx_node& Node, const FMatrix4& Local) const { return IsRootLevel(Node) ? Matrix * Local : Local; }
            FMatrix4 InvBindToEngine(const FMatrix4& InvBind) const { return InvBind * InverseMatrix; }
            FVector3 KeyTranslation(const ufbx_node& Node, const FVector3& T) const { return IsRootLevel(Node) ? Rotation * (InverseAdjustRotation(Node) * T) : T; }
            FQuat    KeyRotation(const ufbx_node& Node, const FQuat& Q) const { return IsRootLevel(Node) ? Rotation * InverseAdjustRotation(Node) * Q : Q; }

            static bool IsRootLevel(const ufbx_node& Node) { return Node.parent == nullptr || Node.parent->is_root; }

            FVector3 Direction(const FVector3& V) const { return Rotation * V; }
            FVector3 Translation(const ufbx_node& Node, const FVector3& T) const { return Rotation * (InverseAdjustRotation(Node) * T); }
            FQuat    Orientation(const ufbx_node& Node, const FQuat& Q) const { return Rotation * InverseAdjustRotation(Node) * Q * InverseRotation; }
            FVector3 Scale(const FVector3& S) const { return Math::Abs(Matrix3 * S); }
        };

        FFixedString PathLeaf(FStringView Path)
        {
            FFixedString Normalized(Path.GetData(), Path.Len());
            for (FFixedString::value_type& C : Normalized)
            {
                if (C == '\\') { C = '/'; }
            }
            const FStringView Leaf = VFS::FileName(FStringView(*Normalized, Normalized.Len()), false);
            return FFixedString(Leaf.GetData(), Leaf.Len());
        }
    }

    void CFBXImporter::ReleaseSourceData()
    {
        // Source data first, since its image spans point into the scene ufbx owns.
        CMeshImporter::ReleaseSourceData();

        if (Scene != nullptr)
        {
            ufbx_free_scene(Scene);
            Scene = nullptr;
        }

        SourceBlob.Reset();
        SourceBlob.Shrink();
    }

    bool CFBXImporter::ParseMeshSource(const FImportRequest& Request,
                                       const FMeshImportOptions& Options,
                                       FMeshImportData& OutData,
                                       FString& OutError,
                                       FScopedSlowTask* Progress)
    {
        if (Scene != nullptr)
        {
            ufbx_free_scene(Scene);
            Scene = nullptr;
        }

        const FFixedString SourcePath = Request.SourcePath;
        const FStringView  SourceDir  = VFS::Parent(SourcePath);
        const FStringView  SourceName = VFS::FileName(SourcePath, true);

        const bool bWantTextures = Options.bImportTextures || Options.bImportMaterials;

        //~ Stage 1, source parsing.

        if (Progress)
        {
            Progress->UpdateMessage("Loading source file...");
        }

        if (!FileHelper::LoadFileToArray(SourceBlob, FStringView(*SourcePath, SourcePath.Len())))
        {
            OutError = FString(*Format("Failed to read '{0}'.", *SourcePath));
            return false;
        }

        if (Progress)
        {
            Progress->UpdateMessage("Parsing FBX...");
        }

        ufbx_load_opts LoadOptions = {};
        LoadOptions.temp_allocator.allocator   = MakeTrackedAllocator();
        LoadOptions.result_allocator.allocator = MakeTrackedAllocator();

        // Lets ufbx resolve texture paths and any external file the scene references relative to the FBX.
        LoadOptions.filename.data   = *SourcePath;
        LoadOptions.filename.length = SourcePath.Len();

        // These axes preserve the X mirror used by the GLTF importer.
        LoadOptions.target_axes                = {
            UFBX_COORDINATE_AXIS_NEGATIVE_X,
            UFBX_COORDINATE_AXIS_POSITIVE_Y,
            UFBX_COORDINATE_AXIS_POSITIVE_Z
        };
        LoadOptions.handedness_conversion_axis = UFBX_MIRROR_AXIS_X;
        LoadOptions.space_conversion           = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;

        // The user's Scale is deliberately NOT applied here, since FinalizeMeshImportData already applies it.
        LoadOptions.target_unit_meters = 1.0f;

        // Helper nodes keep instancing intact, unlike baking a geometry transform into the geometry.
        LoadOptions.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
        LoadOptions.inherit_mode_handling       = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;

        LoadOptions.generate_missing_normals = true;
        LoadOptions.normalize_normals        = true;
        LoadOptions.clean_skin_weights       = true;

        // Blender writes PBR as legacy Phong reversibly, and without this every Blender FBX loses its maps.
        LoadOptions.use_blender_pbr_material = true;

        LoadOptions.load_external_files            = bWantTextures;
        LoadOptions.ignore_missing_external_files  = true;
        LoadOptions.ignore_embedded                = !bWantTextures;

        // A broken index reads as zero, and aborting would reject files that load fine everywhere else.
        LoadOptions.index_error_handling = UFBX_INDEX_ERROR_HANDLING_NO_INDEX;
        LoadOptions.connect_broken_elements = true;

        ufbx_error LoadError;
        Scene = ufbx_load_memory(SourceBlob.GetData(), SourceBlob.Num(), &LoadOptions, &LoadError);
        if (Scene == nullptr)
        {
            OutError = FString(*Format("Failed to parse '{0}': {1}",
                *SourcePath, *FormatUfbxError(LoadError)));
            SourceBlob.Reset();
            SourceBlob.Shrink();
            return false;
        }

        for (const ufbx_warning& Warning : Scene->metadata.warnings)
        {
            LOG_WARN("[FBX] '{}': {} (x{})", SourceName.GetData(), ToView(Warning.description).GetData(), (uint32)Warning.count);
        }

        //~ Embedded payloads stay VIEWS into the scene ufbx owns, and ReleaseSourceData is what frees them.

        TVector<int32> FileToImage(IntCastChecked<int32>(Scene->texture_files.count), Constants::kIndexNone);

        if (bWantTextures && Scene->texture_files.count > 0)
        {
            // In decreasing order of authority, recovering absolute, flattened and .fbm sidecar layouts.
            auto ResolveOnDisk = [&](const ufbx_texture_file& File, FFixedString& OutTried) -> FFixedString
            {
                const FStringView Relative = ToView(File.relative_filename);
                const FStringView Absolute = ToView(File.absolute_filename);
                const FStringView Resolved = ToView(File.filename);

                const FFixedString Leaf = PathLeaf(!Relative.IsEmpty() ? Relative
                                                 : (!Resolved.IsEmpty() ? Resolved : Absolute));

                FFixedString Fbm(SourceName.GetData(), SourceName.Len());
                Fbm.Append(".fbm");

                FFixedString Candidates[6];
                uint32 Count = 0;
                if (!Resolved.IsEmpty()) { Candidates[Count++] = FFixedString(Resolved.GetData(), Resolved.Len()); }
                if (!Relative.IsEmpty()) { Candidates[Count++] = Paths::Combine(SourceDir, Relative); }
                if (!Absolute.IsEmpty()) { Candidates[Count++] = FFixedString(Absolute.GetData(), Absolute.Len()); }
                if (!Leaf.IsEmpty())
                {
                    Candidates[Count++] = Paths::Combine(SourceDir, Leaf);
                    Candidates[Count++] = Paths::Combine(SourceDir, Fbm, Leaf);
                    Candidates[Count++] = Paths::Combine(SourceDir, "textures", Leaf);
                }

                for (uint32 i = 0; i < Count; ++i)
                {
                    if (Paths::Exists(FStringView(*Candidates[i], Candidates[i].Len())))
                    {
                        return Candidates[i];
                    }
                    if (!OutTried.IsEmpty()) { OutTried.Append(", "); }
                    OutTried.Append(Candidates[i]);
                }

                return FFixedString();
            };

            OutData.Images.Reserve(IntCastChecked<int32>(Scene->texture_files.count));

            for (int32 i = 0, Limit = IntCastChecked<int32>(Scene->texture_files.count); i < Limit; ++i)
            {
                const ufbx_texture_file& File = Scene->texture_files.data[i];

                FSourceImage Image;

                // The key drives the cook's filename color-space heuristic, so it wants to look like a filename.
                Image.Key = PathLeaf(!ToView(File.relative_filename).IsEmpty() ? ToView(File.relative_filename)
                                                                            : ToView(File.filename));
                if (Image.Key.IsEmpty())
                {
                    Image.Key = FormatAs<FFixedString>("{}_Image_{}",
                                                       FStringView(SourceName.GetData(), SourceName.Len()), (uint32)i);
                }

                if (File.content.size > 0 && File.content.data != nullptr)
                {
                    Image.Bytes = TSpan<const uint8>((const uint8*)File.content.data, IntCastChecked<int32>(File.content.size));
                }
                else
                {
                    FFixedString Tried;
                    Image.ResolvedPath = ResolveOnDisk(File, Tried);
                    if (Image.ResolvedPath.IsEmpty())
                    {
                        // Named loudly with the paths tried, since a default material looks like an untextured file.
                        LOG_WARN("[FBX] Texture '{}' is neither embedded nor on disk; the materials using it "
                                 "will import with the neutral default. Looked in: {}",
                                 Image.Key, Tried.IsEmpty() ? FFixedString("(no path in the file)") : Tried);
                        continue;
                    }
                }

                FileToImage[i] = OutData.Images.Num();
                OutData.Images.Add(Move(Image));
            }

            uint32 EmbeddedCount = 0;
            for (const FSourceImage& Image : OutData.Images)
            {
                EmbeddedCount += Image.IsEmbedded() ? 1u : 0u;
            }

            LOG_INFO("[FBX] '{}': {} texture file(s) -> {} image(s) ({} embedded).",
                     SourceName.GetData(), (uint32)Scene->texture_files.count, (uint32)OutData.Images.Num(),
                     (uint32)EmbeddedCount);
        }

        //~ Stage 3, materials.

        // The engine carries two UV sets, so anything past set 1 clamps down onto it.
        THashMap<FFixedString, uint32> UVSetIndexByName;
        for (const ufbx_mesh* Mesh : Scene->meshes)
        {
            for (const ufbx_uv_set& Set : Mesh->uv_sets)
            {
                FFixedString Name = ToFixed(Set.name);
                if (!Name.IsEmpty() && !UVSetIndexByName.Contains(Name))
                {
                    UVSetIndexByName.TryAdd(Move(Name), Math::Min<uint32>(Set.index, 1u));
                }
            }
        }

        auto ResolveTexCoordSet = [&](const ufbx_texture* Texture) -> uint32
        {
            if (Texture == nullptr)
            {
                return 0;
            }
            const FFixedString Name = ToFixed(Texture->uv_set);
            if (Name.IsEmpty())
            {
                return 0;
            }
            auto It = UVSetIndexByName.Find(Name);
            return (It != nullptr) ? (*It) : 0u;
        };

        auto ResolveImage = [&](const ufbx_material_map& Map) -> int32
        {
            // texture_enabled is deliberately ignored, since exporters set it inconsistently.
            const ufbx_texture* File = FindFileTexture(Map.texture);
            if (File == nullptr || !File->has_file || File->file_index == UFBX_NO_INDEX)
            {
                return Constants::kIndexNone;
            }
            return (File->file_index < static_cast<uint32_t>(FileToImage.Num())) ? FileToImage[File->file_index] : Constants::kIndexNone;
        };

        auto ResolveUVTransform = [&](const ufbx_material_map& Map) -> FTextureUVTransform
        {
            FTextureUVTransform Out;

            const ufbx_texture* Texture = FindFileTexture(Map.texture);
            if (Texture == nullptr)
            {
                return Out;
            }

            Out.TexCoordSet = ResolveTexCoordSet(Texture);

            if (!Texture->has_uv_transform)
            {
                return Out;
            }

            // uv_to_texture runs the same direction as the engine's scale, rotate and offset chain.
            const ufbx_matrix& M = Texture->uv_to_texture;
            const FVector2 BasisU((float)M.m00, (float)M.m10);
            const FVector2 BasisV((float)M.m01, (float)M.m11);

            Out.Scale    = FVector2(Math::Length(BasisU), Math::Length(BasisV));
            Out.Rotation = Math::Atan2((float)M.m10, (float)M.m00);
            Out.Offset   = FVector2((float)M.m03, (float)M.m13);
            return Out;
        };

        auto ResolveSampler = [&](const ufbx_material_map& Map) -> EImportSampler
        {
            const ufbx_texture* Texture = FindFileTexture(Map.texture);
            if (Texture == nullptr)
            {
                return EImportSampler::LinearWrap;
            }
            // FBX wraps per axis and carries no filter mode; U wins for the same reason it does in glTF.
            return (Texture->wrap_u == UFBX_WRAP_CLAMP) ? EImportSampler::LinearClamp : EImportSampler::LinearWrap;
        };

        auto MarkRole = [&](int32 ImageIndex, ETextureColorSpace Role)
        {
            if (ImageIndex >= 0 && (size_t)ImageIndex < static_cast<size_t>(OutData.Images.Num()))
            {
                OutData.Images[ImageIndex].IntendedColorSpace = Role;
            }
        };

        // FBX has no masked flag, so alpha-tested foliage is flagged by name and can be overridden after.
        auto IsFoliageName = [&](const FString& MaterialName, int32 BaseColorImage) -> bool
        {
            static const char* const kKeywords[] = {
                "leaf", "leaves", "foliage", "tree", "plant", "ivy", "bush",
                "grass", "branch", "fern", "hedge", "vine", "shrub", "flower" };

            auto Lower = [](FStringView In) -> FString
            {
                FString L(In.GetData(), In.Len());
                for (FString::value_type& C : L) { if (C >= 'A' && C <= 'Z') { C = C - 'A' + 'a'; } }
                return L;
            };

            const FString Name = Lower(FStringView(*MaterialName, MaterialName.Len()));
            const FFixedString TexKey = (BaseColorImage >= 0 && (size_t)BaseColorImage < static_cast<size_t>(OutData.Images.Num()))
                ? OutData.Images[BaseColorImage].Key : FFixedString();
            const FString Texture = Lower(FStringView(*TexKey, TexKey.Len()));

            for (const char* Keyword : kKeywords)
            {
                if (Name.Find(Keyword) != Constants::kIndexNone || Texture.Find(Keyword) != Constants::kIndexNone)
                {
                    return true;
                }
            }
            return false;
        };

        TVector<int32> MaterialToUnique(IntCastChecked<int32>(Scene->materials.count), Constants::kIndexNone);

        if (Options.bImportMaterials)
        {
            FKeyDedup MaterialDedup(Scene->materials.count);
            OutData.Materials.Reserve(IntCastChecked<int32>(Scene->materials.count));

            for (int32 i = 0, Limit = IntCastChecked<int32>(Scene->materials.count); i < Limit; ++i)
            {
                const ufbx_material& Source        = *Scene->materials.data[i];
                const ufbx_material_pbr_maps& PBR  = Source.pbr;
                const ufbx_material_features& Feat = Source.features;

                FMeshImportMaterial Material;
                Material.Name = !ToView(Source.name).IsEmpty()
                    ? FString(Source.name.data, IntCastChecked<int32>(Source.name.length))
                    : Format("{}_Mat{}", FStringView(SourceName.GetData(), SourceName.Len()), (uint32)i);

                // ufbx normalizes Phong, Arnold, Max Physical, Substance and Blender onto this one set of maps.
                Material.BaseColorImage = ResolveImage(PBR.base_color);
                Material.NormalImage    = ResolveImage(PBR.normal_map);
                Material.EmissiveImage  = ResolveImage(PBR.emission_color);
                Material.OcclusionImage = ResolveImage(PBR.ambient_occlusion);

                // The packed form is only assumed when both channels genuinely name the same file.
                const int32 MetallicImage  = ResolveImage(PBR.metalness);
                const int32 RoughnessImage = ResolveImage(PBR.roughness);
                if (MetallicImage != Constants::kIndexNone && MetallicImage == RoughnessImage)
                {
                    Material.MetallicRoughnessImage = MetallicImage;
                }
                else
                {
                    Material.MetallicImage  = MetallicImage;
                    Material.RoughnessImage = RoughnessImage;
                }

                // An FBX texture REPLACES its property's constant rather than tinting it the way a glTF factor does.
                auto ScalarFactor = [](const ufbx_material_map& Map, int32 Image, float Default) -> float
                {
                    return (Image != Constants::kIndexNone) ? 1.0f : MapReal(Map, Default);
                };

                const int32 OpacityImage = ResolveImage(PBR.opacity);
                const float Opacity = (OpacityImage != Constants::kIndexNone) ? 1.0f : MapReal(PBR.opacity, 1.0f);

                if (Material.BaseColorImage != Constants::kIndexNone)
                {
                    Material.BaseColorFactor = FVector4(1.0f, 1.0f, 1.0f, Opacity);
                }
                else
                {
                    const FVector3 BaseColor  = MapVec3(PBR.base_color, FVector3(1.0f));
                    const float    BaseFactor = MapReal(PBR.base_factor, 1.0f);
                    Material.BaseColorFactor = FVector4(BaseColor.x * BaseFactor, BaseColor.y * BaseFactor,
                                                        BaseColor.z * BaseFactor, Opacity);
                }

                // No metalness value and no map means a dielectric, so a factor of 1 would import Phong as metal.
                Material.MetallicFactor = (PBR.metalness.has_value || MetallicImage != Constants::kIndexNone)
                    ? ScalarFactor(PBR.metalness, MetallicImage, 0.0f) : 0.0f;

                Material.RoughnessFactor = ScalarFactor(PBR.roughness, RoughnessImage, 0.5f);
                if (Feat.roughness_as_glossiness.enabled)
                {
                    if (RoughnessImage == Constants::kIndexNone)
                    {
                        Material.RoughnessFactor = 1.0f - Material.RoughnessFactor;
                    }
                    else
                    {
                        LOG_WARN("[FBX] Material '{}' stores glossiness in its roughness map; the generated "
                                 "graph has no invert node, so the map imports uninverted.", Material.Name);
                    }
                }

                const float EmissionFactor = MapReal(PBR.emission_factor, 1.0f);
                if (Material.EmissiveImage != Constants::kIndexNone)
                {
                    // The map supplies the color; only the weight stays a constant.
                    Material.EmissiveColor = FVector3(EmissionFactor);
                }
                else
                {
                    const FVector3 Emission = MapVec3(PBR.emission_color, FVector3(0.0f));
                    Material.EmissiveColor = Feat.emission.enabled ? Emission * EmissionFactor : FVector3(0.0f);
                }

                Material.IOR            = MapReal(PBR.specular_ior, 1.5f);
                Material.SpecularFactor = MapReal(PBR.specular_factor, 1.0f);

                if (Feat.coat.enabled)
                {
                    Material.ClearcoatFactor    = MapReal(PBR.coat_factor, 0.0f);
                    Material.ClearcoatRoughness = MapReal(PBR.coat_roughness, 0.0f);
                }

                Material.bTwoSided = Feat.double_sided.enabled;
                Material.bUnlit    = Feat.unlit.enabled;

                // The engine clips on base-color alpha only, so say so rather than importing it opaque.
                if (OpacityImage != Constants::kIndexNone)
                {
                    Material.AlphaMode   = EImportAlphaMode::Mask;
                    Material.AlphaCutoff = 0.5f;

                    if (OpacityImage != Material.BaseColorImage)
                    {
                        LOG_WARN("[FBX] Material '{}' uses a separate opacity map, which the engine samples "
                                 "from base-color alpha instead. Imported as alpha-tested against the base "
                                 "color; if that alpha is opaque the mask has no effect.", Material.Name);
                    }
                }
                else if (Opacity < 0.999f)
                {
                    Material.AlphaMode = EImportAlphaMode::Blend;
                }
                else if (IsFoliageName(Material.Name, Material.BaseColorImage))
                {
                    Material.AlphaMode   = EImportAlphaMode::Mask;
                    Material.AlphaCutoff = 0.5f;
                }

                Material.UVTransforms[(size_t)EMaterialTextureSlot::BaseColor]         = ResolveUVTransform(PBR.base_color);
                Material.UVTransforms[(size_t)EMaterialTextureSlot::Normal]            = ResolveUVTransform(PBR.normal_map);
                Material.UVTransforms[(size_t)EMaterialTextureSlot::Emissive]          = ResolveUVTransform(PBR.emission_color);
                Material.UVTransforms[(size_t)EMaterialTextureSlot::Occlusion]         = ResolveUVTransform(PBR.ambient_occlusion);
                Material.UVTransforms[(size_t)EMaterialTextureSlot::Metallic]          = ResolveUVTransform(PBR.metalness);
                Material.UVTransforms[(size_t)EMaterialTextureSlot::Roughness]         = ResolveUVTransform(PBR.roughness);
                Material.UVTransforms[(size_t)EMaterialTextureSlot::MetallicRoughness] =
                    (Material.MetallicRoughnessImage != Constants::kIndexNone) ? ResolveUVTransform(PBR.roughness) : FTextureUVTransform();

                Material.Samplers[(size_t)EMaterialTextureSlot::BaseColor]         = ResolveSampler(PBR.base_color);
                Material.Samplers[(size_t)EMaterialTextureSlot::Normal]            = ResolveSampler(PBR.normal_map);
                Material.Samplers[(size_t)EMaterialTextureSlot::Emissive]          = ResolveSampler(PBR.emission_color);
                Material.Samplers[(size_t)EMaterialTextureSlot::Occlusion]         = ResolveSampler(PBR.ambient_occlusion);
                Material.Samplers[(size_t)EMaterialTextureSlot::Metallic]          = ResolveSampler(PBR.metalness);
                Material.Samplers[(size_t)EMaterialTextureSlot::Roughness]         = ResolveSampler(PBR.roughness);
                Material.Samplers[(size_t)EMaterialTextureSlot::MetallicRoughness] = ResolveSampler(PBR.roughness);

                MarkRole(Material.BaseColorImage,         ETextureColorSpace::SRGB);
                MarkRole(Material.EmissiveImage,          ETextureColorSpace::SRGB);
                MarkRole(Material.MetallicRoughnessImage, ETextureColorSpace::PackedData);
                MarkRole(Material.MetallicImage,          ETextureColorSpace::Linear);
                MarkRole(Material.RoughnessImage,         ETextureColorSpace::Linear);
                MarkRole(Material.OcclusionImage,         ETextureColorSpace::Linear);
                // Normals cook as Linear BC7 since the BC5 path is broken and the output node rebuilds Z anyway.
                MarkRole(Material.NormalImage,            ETextureColorSpace::Linear);

                if (!bDeduplicateMaterials)
                {
                    MaterialToUnique[i] = OutData.Materials.Num();
                    OutData.Materials.Add(Move(Material));
                    continue;
                }

                TVector<uint32> Key;
                Key.Reserve(32);
                Key.Add(QuantizeFloat(Material.BaseColorFactor.x));
                Key.Add(QuantizeFloat(Material.BaseColorFactor.y));
                Key.Add(QuantizeFloat(Material.BaseColorFactor.z));
                Key.Add(QuantizeFloat(Material.BaseColorFactor.w));
                Key.Add(QuantizeFloat(Material.MetallicFactor));
                Key.Add(QuantizeFloat(Material.RoughnessFactor));
                Key.Add(QuantizeFloat(Material.EmissiveColor.x));
                Key.Add(QuantizeFloat(Material.EmissiveColor.y));
                Key.Add(QuantizeFloat(Material.EmissiveColor.z));
                Key.Add(QuantizeFloat(Material.AlphaCutoff));
                Key.Add((uint32)Material.AlphaMode | (Material.bTwoSided ? 0x100u : 0u) | (Material.bUnlit ? 0x200u : 0u));
                Key.Add((uint32)Material.BaseColorImage);
                Key.Add((uint32)Material.MetallicRoughnessImage);
                Key.Add((uint32)Material.MetallicImage);
                Key.Add((uint32)Material.RoughnessImage);
                Key.Add((uint32)Material.NormalImage);
                Key.Add((uint32)Material.EmissiveImage);
                Key.Add((uint32)Material.OcclusionImage);

                // Same images mapped or filtered differently are NOT the same material and must not collapse.
                for (const FTextureUVTransform& UVT : Material.UVTransforms)
                {
                    Key.Add(UVT.TexCoordSet);
                    Key.Add(QuantizeFloat(UVT.Offset.x));
                    Key.Add(QuantizeFloat(UVT.Offset.y));
                    Key.Add(QuantizeFloat(UVT.Scale.x));
                    Key.Add(QuantizeFloat(UVT.Scale.y));
                    Key.Add(QuantizeFloat(UVT.Rotation));
                }
                for (EImportSampler Sampler : Material.Samplers)
                {
                    Key.Add((uint32)Sampler);
                }
                Key.Add(QuantizeFloat(Material.IOR));
                Key.Add(QuantizeFloat(Material.SpecularFactor));
                Key.Add(QuantizeFloat(Material.ClearcoatFactor));
                Key.Add(QuantizeFloat(Material.ClearcoatRoughness));

                bool bIsNew = false;
                const uint32 Slot = MaterialDedup.Insert(Move(Key), bIsNew);
                MaterialToUnique[i] = (int32)Slot;
                if (bIsNew)
                {
                    OutData.Materials.Add(Move(Material));
                }
            }
        }

        //~ One DFS from the root so nodes land parents-before-children, which the prefab builder needs.

        if (Progress)
        {
            Progress->EnterProgressFrame(0.15f, "Discovering scene...");
        }

        // typed_id is a dense key into Scene->nodes, and the visit order is parents-before-children.
        TVector<uint32> NodeVisitOrder;
        NodeVisitOrder.Reserve(IntCastChecked<int32>(Scene->nodes.count));

        struct FStackEntry
        {
            const ufbx_node* Node;
            int32            ParentSceneNode;
        };

        TVector<FStackEntry> Stack;
        Stack.Reserve(IntCastChecked<int32>(Math::Max<size_t>(64, Scene->nodes.count)));

        // Light props are still in the file's own units; ufbx only converts transforms and geometry.
        const float SourceUnitMeters = (float)Scene->settings.original_unit_meters;

        const FEngineBasis Basis(*Scene);

        auto FillLight = [SourceUnitMeters, &Basis](FSourceSceneNode& SceneNode, const ufbx_light& Light)
        {
            if (!Light.cast_light)
            {
                return;
            }

            switch (Light.type)
            {
            case UFBX_LIGHT_DIRECTIONAL: SceneNode.Kind = ESourceNodeKind::DirectionalLight; break;
            case UFBX_LIGHT_SPOT:        SceneNode.Kind = ESourceNodeKind::SpotLight;        break;
            case UFBX_LIGHT_POINT:       SceneNode.Kind = ESourceNodeKind::PointLight;       break;
            // An area or volume light has no engine equivalent, so it lands as the point light it surrounds.
            case UFBX_LIGHT_AREA:
            case UFBX_LIGHT_VOLUME:      SceneNode.Kind = ESourceNodeKind::PointLight;       break;
            default: return;
            }

            SceneNode.Light.Color     = ToVector3(Light.color);
            SceneNode.Light.Intensity = (float)Light.intensity;

            // FBX intensity is a percentage against whatever renderer authored the file, so only its ratios carry over.
            SceneNode.Light.Units     = ESourceLightUnits::Relative;

            // FBX cone angles span the whole cone; the engine measures from the axis, so they are halved.
            SceneNode.Light.InnerConeAngle = Math::Radians((float)Light.inner_angle * 0.5f);
            SceneNode.Light.OuterConeAngle = Math::Radians((float)Light.outer_angle * 0.5f);

            const FVector3 Aim = ToVector3(Light.local_direction);
            SceneNode.Light.LocalDirection = Basis.Direction((Math::LengthSquared(Aim) > Math::Epsilon<float>())
                ? Math::Normalize(Aim)
                : FVector3(0.0f, -1.0f, 0.0f));

            // The cutoff distance is only meaningful when the file opted into it.
            static constexpr const char* EnableFarProp = "EnableFarAttenuation";
            static constexpr const char* FarEndProp    = "FarAttenuationEnd";
            if (ufbx_find_int_len(&Light.props, EnableFarProp, strlen(EnableFarProp), 0) != 0)
            {
                const float FarEnd = (float)ufbx_find_real_len(&Light.props, FarEndProp, strlen(FarEndProp), 0.0);
                SceneNode.Light.Range = FarEnd * SourceUnitMeters;
            }
        };

        // An FBX node may carry a mesh AND a light; the light becomes a child so neither is dropped.
        struct FDetachedLight
        {
            int32             ParentSceneNode;
            const ufbx_light* Light;
        };
        TVector<FDetachedLight> DetachedLights;

        OutData.SceneNodes.Reserve(IntCastChecked<int32>(Scene->nodes.count));
        OutData.MeshInstances.Reserve(IntCastChecked<int32>(Scene->nodes.count));

        TVector<uint8> MeshReferenced(IntCastChecked<int32>(Scene->meshes.count), 0);

        // The root itself is the scene's implicit container, so its children start at the top level.
        for (const ufbx_node* Child : Scene->root_node->children)
        {
            Stack.Emplace(Child, Constants::kIndexNone);
        }

        uint32 VisitedNodes = 0;
        while (!Stack.IsEmpty())
        {
            const FStackEntry Entry = Stack.Last();
            Stack.Pop();
            ++VisitedNodes;

            const ufbx_node& Node = *Entry.Node;

            const int32 SceneNodeIndex = OutData.SceneNodes.Num();
            NodeVisitOrder.Add(Node.typed_id);

            FSourceSceneNode& SceneNode = OutData.SceneNodes.AddDefaultedGetRef();
            SceneNode.ParentIndex = Entry.ParentSceneNode;
            SceneNode.Name = !ToView(Node.name).IsEmpty()
                ? FName(*ToFixed(Node.name))
                : FName("Node", (uint32)Node.typed_id);

            SceneNode.Translation = Basis.Translation(Node, ToVector3(Node.local_transform.translation));
            SceneNode.Rotation    = Basis.Orientation(Node, ToQuat(Node.local_transform.rotation));
            SceneNode.Scale       = Basis.Scale(ToVector3(Node.local_transform.scale));

            if (Node.mesh != nullptr)
            {
                // MeshSlot and SlotIndex hold the raw mesh index until the dedup table rewrites them.
                const uint32 MeshIndex = Node.mesh->typed_id;
                MeshReferenced[MeshIndex] = 1;
                SceneNode.Kind     = ESourceNodeKind::Mesh;
                SceneNode.MeshSlot = (int32)MeshIndex;

                // geometry_to_world, since helper nodes carry the mesh offset the merge path bakes in.
                OutData.MeshInstances.Emplace(MeshIndex, (uint32)Node.typed_id, Basis.WorldToEngine(Node.geometry_to_world));

                if (Node.light != nullptr)
                {
                    DetachedLights.Emplace(SceneNodeIndex, Node.light);
                }
            }
            else if (Node.light != nullptr)
            {
                FillLight(SceneNode, *Node.light);
            }
            else if (Node.camera != nullptr)
            {
                const ufbx_camera& Camera = *Node.camera;
                SceneNode.Kind = ESourceNodeKind::Camera;

                if (Camera.projection_mode == UFBX_PROJECTION_MODE_ORTHOGRAPHIC)
                {
                    SceneNode.Camera.bOrthographic = true;
                    SceneNode.Camera.OrthoWidth    = (float)Camera.orthographic_size.x;
                }
                else
                {
                    SceneNode.Camera.bOrthographic = false;
                    SceneNode.Camera.YFov          = Math::Radians((float)Camera.field_of_view_deg.y);
                }
                SceneNode.Camera.ZNear = (float)Camera.near_plane;
                SceneNode.Camera.ZFar  = (float)Camera.far_plane;
            }

            for (const ufbx_node* Child : Node.children)
            {
                Stack.Emplace(Child, SceneNodeIndex);
            }
        }

        for (const FDetachedLight& Detached : DetachedLights)
        {
            FSourceSceneNode& LightNode = OutData.SceneNodes.AddDefaultedGetRef();
            LightNode.ParentIndex = Detached.ParentSceneNode;
            LightNode.Name = !ToView(Detached.Light->name).IsEmpty()
                ? FName(*ToFixed(Detached.Light->name))
                : FName("Light", (uint32)Detached.Light->typed_id);
            FillLight(LightNode, *Detached.Light);
        }

        OutData.SourceNodeCount = VisitedNodes;

        // A file whose meshes hang off nothing still has geometry worth importing.
        bool bAnyReferenced = false;
        for (uint8 Referenced : MeshReferenced)
        {
            bAnyReferenced |= (Referenced != 0);
        }
        if (!bAnyReferenced)
        {
            for (int32 i = 0, Limit = IntCastChecked<int32>(Scene->meshes.count); i < Limit; ++i)
            {
                MeshReferenced[i] = 1;
                OutData.MeshInstances.Emplace((uint32)i, 0u, FMatrix4(1.0f));
            }
        }

        //~ ufbx already shares a mesh across instances, so this only catches doubly-emitted geometry.

        TVector<uint32> UniqueMeshes;
        TVector<int32>  MeshToUnique(IntCastChecked<int32>(Scene->meshes.count), Constants::kIndexNone);
        {
            FKeyDedup MeshDedup(Scene->meshes.count);
            UniqueMeshes.Reserve(IntCastChecked<int32>(Scene->meshes.count));

            TVector<uint32> Key;
            for (int32 MeshIndex = 0, Limit = IntCastChecked<int32>(Scene->meshes.count); MeshIndex < Limit; ++MeshIndex)
            {
                if (MeshReferenced[MeshIndex] == 0)
                {
                    continue;
                }

                const ufbx_mesh& Mesh = *Scene->meshes.data[MeshIndex];

                if (!bDeduplicateMeshes)
                {
                    MeshToUnique[MeshIndex] = UniqueMeshes.Num();
                    UniqueMeshes.Add((uint32)MeshIndex);
                    continue;
                }

                // Counts, material assignment and a position hash, specific enough that two meshes never collapse.
                Key.Reset();
                Key.Add((uint32)Mesh.num_vertices);
                Key.Add((uint32)Mesh.num_indices);
                Key.Add((uint32)Mesh.num_faces);
                Key.Add((uint32)Mesh.skin_deformers.count);
                for (const ufbx_material* Material : Mesh.materials)
                {
                    Key.Add(Material != nullptr ? Material->typed_id : Constants::kIndexNoneU32);
                }

                uint64 PositionHash = 0xCBF29CE484222325ull;
                for (const ufbx_vec3& Position : Mesh.vertices)
                {
                    const float Components[3] = { (float)Position.x, (float)Position.y, (float)Position.z };
                    for (float Component : Components)
                    {
                        uint32 Bits;
                        memcpy(&Bits, &Component, sizeof(Bits));
                        PositionHash = (PositionHash ^ Bits) * 0x100000001B3ull;
                    }
                }
                Key.Add((uint32)(PositionHash & 0xFFFFFFFFu));
                Key.Add((uint32)(PositionHash >> 32));

                bool bIsNew = false;
                TVector<uint32> OwnedKey = Key;
                const uint32 Slot = MeshDedup.Insert(Move(OwnedKey), bIsNew);
                MeshToUnique[MeshIndex] = (int32)Slot;
                if (bIsNew)
                {
                    UniqueMeshes.Add((uint32)MeshIndex);
                }
            }

            for (FSourceMeshInstance& Instance : OutData.MeshInstances)
            {
                const int32 Slot = MeshToUnique[Instance.SlotIndex];
                Instance.SlotIndex = (Slot >= 0) ? (uint32)Slot : 0u;
            }

            for (FSourceSceneNode& SceneNode : OutData.SceneNodes)
            {
                if (SceneNode.Kind == ESourceNodeKind::Mesh && SceneNode.MeshSlot >= 0)
                {
                    SceneNode.MeshSlot = MeshToUnique[SceneNode.MeshSlot];
                }
            }
        }

        //~ Every ancestor goes in too, since an FK chain that skips a node poses everything below it wrong.

        THashMap<uint32, int32> NodeToBone;   // ufbx_node::typed_id -> bone index
        TVector<const ufbx_node*> BoneNodes;

        {
            TVector<uint8> IsBone(IntCastChecked<int32>(Scene->nodes.count), 0);
            for (const ufbx_skin_deformer* Skin : Scene->skin_deformers)
            {
                for (const ufbx_skin_cluster* Cluster : Skin->clusters)
                {
                    for (const ufbx_node* Node = Cluster->bone_node; Node != nullptr; Node = Node->parent)
                    {
                        if (Node->is_root || IsBone[Node->typed_id] != 0)
                        {
                            break;
                        }
                        IsBone[Node->typed_id] = 1;
                    }
                }
            }

            // Cape joints, twist helpers and attachment nulls skin to nothing, so only this reaches them.
            if (Options.bImportUnskinnedBones)
            {
                TVector<const ufbx_node*> Pending;
                for (const ufbx_node* Node : Scene->nodes)
                {
                    const bool bSkeletonRoot = IsBone[Node->typed_id] != 0
                        && (Node->parent == nullptr || IsBone[Node->parent->typed_id] == 0);
                    if (bSkeletonRoot)
                    {
                        Pending.Add(Node);
                    }
                }

                while (!Pending.IsEmpty())
                {
                    const ufbx_node* Node = Pending.Last();
                    Pending.Pop();
                    for (const ufbx_node* Child : Node->children)
                    {
                        // Descending past a skipped node would orphan whatever hangs below it.
                        if (Child->mesh != nullptr || Child->camera != nullptr || Child->light != nullptr)
                        {
                            continue;
                        }
                        IsBone[Child->typed_id] = 1;
                        Pending.Add(Child);
                    }
                }
            }

            // Emitted parents-before-children, which BuildBindPoseCache and every FK pass assume.
            for (uint32 TypedId : NodeVisitOrder)
            {
                if (IsBone[TypedId] == 0)
                {
                    continue;
                }
                NodeToBone.TryAdd(TypedId, BoneNodes.Num());
                BoneNodes.Add(Scene->nodes.data[TypedId]);
            }
        }

        if (!BoneNodes.IsEmpty() && Options.bImportSkeleton)
        {
            TUniquePtr<FSkeletonResource> Skeleton = MakeUnique<FSkeletonResource>();
            Skeleton->Name = FName(*(FString(SourceName.GetData(), SourceName.Len()) + "_Skeleton"));
            Skeleton->Bones.SetNum(BoneNodes.Num());

            // A clustered bone's FK times InvBind is the skin's geometry-to-world, so every other bone must match it.
            FMatrix4 SkinGeometryToWorld = FMatrix4(1.0f);
            for (const ufbx_skin_deformer* Skin : Scene->skin_deformers)
            {
                if (Skin->clusters.count > 0)
                {
                    SkinGeometryToWorld = ToMatrix4(Skin->clusters.data[0]->geometry_to_world);
                    break;
                }
            }

            for (int32 BoneIndex = 0; BoneIndex < BoneNodes.Num(); ++BoneIndex)
            {
                const ufbx_node& Node = *BoneNodes[BoneIndex];

                FSkeletonResource::FBoneInfo& Bone = Skeleton->Bones[BoneIndex];
                Bone.Name           = !ToView(Node.name).IsEmpty() ? FName(*ToFixed(Node.name))
                                                                 : FName("Bone", (uint32)Node.typed_id);
                FMatrix4 LocalTransform = Math::ToMatrix4(InverseAdjustRotation(Node)) * ToMatrix4(Node.node_to_parent);
                Bone.LocalTransform = Basis.BoneToEngine(Node, LocalTransform);

                // Both sides carry ufbx's adjust, so it cancels and only the engine basis is left to apply.
                Bone.InvBindMatrix  = Basis.InvBindToEngine(Math::Inverse(ToMatrix4(Node.node_to_world)) * SkinGeometryToWorld);

                Bone.ParentIndex = Constants::kIndexNone;
                if (Node.parent != nullptr)
                {
                    auto It = NodeToBone.Find(Node.parent->typed_id);
                    if (It != nullptr)
                    {
                        Bone.ParentIndex = (*It);
                    }
                }

                Skeleton->BoneNameToIndex[Bone.Name] = (int32)BoneIndex;
            }

            // geometry_to_bone is already the inverse bind, and it carries the geometry transform with it.
            for (const ufbx_skin_deformer* Skin : Scene->skin_deformers)
            {
                for (const ufbx_skin_cluster* Cluster : Skin->clusters)
                {
                    if (Cluster->bone_node == nullptr)
                    {
                        continue;
                    }
                    auto It = NodeToBone.Find(Cluster->bone_node->typed_id);
                    if (It == nullptr)
                    {
                        continue;
                    }
                    Skeleton->Bones[(*It)].InvBindMatrix = Basis.InvBindToEngine(ToMatrix4(Cluster->geometry_to_bone));
                }
            }

            Skeleton->BuildBindPoseCache();
            OutData.Skeletons.Add(Move(Skeleton));
        }

        //~ The node's world transform stays on the instance, so an instanced scene costs one mesh.

        if (Progress)
        {
            Progress->EnterProgressFrame(0.10f, "Reading geometry...");
        }

        struct FMeshSlotResult
        {
            TUniquePtr<FMeshResource> Static;
            TUniquePtr<FMeshResource> Skinned;
            bool                      bHasColors = false;
        };

        TVector<FMeshSlotResult> Slots(UniqueMeshes.Num());

        // Named serially so a collision suffix is deterministic, then the parallel pass owns its own slot.
        TVector<FFixedString> MeshNames(UniqueMeshes.Num());
        {
            THashMap<FFixedString, uint32> NameCounts;
            for (int32 Slot = 0; Slot < UniqueMeshes.Num(); ++Slot)
            {
                const ufbx_mesh& Mesh = *Scene->meshes.data[UniqueMeshes[Slot]];

                FFixedString Name = SanitizedSourceName(Mesh.name.data, Mesh.name.length);
                if (Name.IsEmpty())
                {
                    Name = FormatAs<FFixedString>("{}_{}",
                                                  FStringView(SourceName.GetData(), SourceName.Len()), (uint32)UniqueMeshes[Slot]);
                }

                uint32& Count = NameCounts[Name];
                if (Count > 0)
                {
                    Name.Append("_").Append(Format("{}", Count));
                }
                ++Count;

                MeshNames[Slot] = Move(Name);
            }
        }

        const bool  bSkipFinalize = Options.bSkipFinalization;
        const float GeometryStep  = 0.6f / (float)Math::Max<size_t>(1, UniqueMeshes.Num());
        TAtomic<uint32> Completed{0};

        Jobs::ParallelFor((uint32)UniqueMeshes.Num(), [&](uint32 SlotIndex)
        {
            const ufbx_mesh& Mesh = *Scene->meshes.data[UniqueMeshes[SlotIndex]];
            const FFixedString& MeshName = MeshNames[SlotIndex];

            FMeshSlotResult& Slot = Slots[SlotIndex];

            const ufbx_skin_deformer* Skin = (Mesh.skin_deformers.count > 0) ? Mesh.skin_deformers.data[0] : nullptr;
            const bool bSkinned = (Skin != nullptr) && !NodeToBone.IsEmpty();

            TUniquePtr<FMeshResource> Resource = MakeUnique<FMeshResource>();
            Resource->bSkinnedMesh = bSkinned;
            Resource->Name = FName(*(FString(*MeshName, MeshName.Len())
                                    + (bSkinned ? "_SkeletalMesh" : "_Mesh")));

            const ufbx_vertex_vec2* UV1Source = (Mesh.uv_sets.count > 1) ? &Mesh.uv_sets.data[1].vertex_uv : nullptr;
            Slot.bHasColors = (Mesh.vertex_color.exists);

            // Working corner-by-corner is what makes split normals and hard edges come out right.
            const size_t CornerCount = Mesh.num_triangles * 3;
            if (CornerCount == 0)
            {
                Slot.Static = Move(Resource);
                return;
            }

            TVector<FSourceSkinnedVertex> Corners(IntCastChecked<int32>(CornerCount));
            TVector<uint32>               Indices(IntCastChecked<int32>(CornerCount));
            TVector<uint32>               TriangleIndices(IntCastChecked<int32>(Mesh.max_face_triangles * 3));

            // Bone remap for this mesh's clusters, resolved once rather than per influence.
            TVector<int32> ClusterToBone;
            if (bSkinned)
            {
                ClusterToBone.SetNum(IntCastChecked<int32>(Skin->clusters.count), Constants::kIndexNone);
                for (int32 c = 0, Limit = IntCastChecked<int32>(Skin->clusters.count); c < Limit; ++c)
                {
                    const ufbx_node* BoneNode = Skin->clusters.data[c]->bone_node;
                    if (BoneNode == nullptr)
                    {
                        continue;
                    }
                    auto It = NodeToBone.Find(BoneNode->typed_id);
                    if (It != nullptr)
                    {
                        ClusterToBone[c] = (*It);
                    }
                }
            }

            int32 CornerCursor = 0;

            struct FPendingSurface { uint32 Start; uint32 Count; int16 Material; };
            TVector<FPendingSurface> PendingSurfaces;
            PendingSurfaces.Reserve(IntCastChecked<int32>(Mesh.material_parts.count));

            for (const ufbx_mesh_part& Part : Mesh.material_parts)
            {
                if (Part.num_triangles == 0)
                {
                    continue;
                }

                const uint32 SurfaceStart = (uint32)CornerCursor;

                for (uint32 FaceIndex : Part.face_indices)
                {
                    const ufbx_face Face = Mesh.faces.data[FaceIndex];
                    const uint32 TriangleCount = ufbx_triangulate_face(
                        TriangleIndices.GetData(), TriangleIndices.Num(), &Mesh, Face);

                    for (uint32 i = 0; i < TriangleCount * 3; ++i)
                    {
                        const uint32 Corner = TriangleIndices[i];

                        FSourceSkinnedVertex& Vertex = Corners[CornerCursor];
                        Vertex = FSourceSkinnedVertex{};

                        Vertex.Position = Basis.Direction(ToVector3(ufbx_get_vertex_vec3(&Mesh.vertex_position, Corner)));
                        Vertex.Normal   = PackNormal(Math::Normalize(
                            Basis.Direction(ToVector3(ufbx_get_vertex_vec3(&Mesh.vertex_normal, Corner)))));

                        // FBX authors UVs bottom-left and the engine samples top-left, so only this importer flips.
                        FVector2 UV(0.0f, 0.0f);
                        if (Mesh.vertex_uv.exists)
                        {
                            UV = ToVector2(ufbx_get_vertex_vec2(&Mesh.vertex_uv, Corner));
                            UV.y = 1.0f - UV.y;
                        }
                        Vertex.UV = Math::PackHalf2x16(UV);

                        // Mirroring set 0 makes a material asking for set 1 render like set 0 rather than one texel.
                        FVector2 UV1 = UV;
                        if (UV1Source != nullptr && UV1Source->exists)
                        {
                            UV1 = ToVector2(ufbx_get_vertex_vec2(UV1Source, Corner));
                            UV1.y = 1.0f - UV1.y;
                        }
                        Vertex.UV1 = Math::PackHalf2x16(UV1);

                        Vertex.Color = Mesh.vertex_color.exists
                            ? PackColor(ToVector4(ufbx_get_vertex_vec4(&Mesh.vertex_color, Corner)))
                            : 0xFFFFFFFFu;

                        if (bSkinned)
                        {
                            const uint32 VertexIndex = Mesh.vertex_indices.data[Corner];

                            FU16Vector4 JointIndices{};
                            FVector4   JointWeights(0.0f);

                            if (VertexIndex < Skin->vertices.count)
                            {
                                const ufbx_skin_vertex& SkinVertex = Skin->vertices.data[VertexIndex];
                                // ufbx sorts influences by decreasing weight, so the first four are the four that matter.
                                const uint32 Count = Math::Min<uint32>((uint32)SkinVertex.num_weights, 4u);
                                for (uint32 w = 0; w < Count; ++w)
                                {
                                    const ufbx_skin_weight& Weight = Skin->weights.data[SkinVertex.weight_begin + w];
                                    const int32 Bone = (Weight.cluster_index < static_cast<uint32_t>(ClusterToBone.Num()))
                                        ? ClusterToBone[Weight.cluster_index] : Constants::kIndexNone;
                                    if (Bone < 0)
                                    {
                                        continue;
                                    }
                                    JointIndices[w] = (uint16)Math::Clamp(Bone, 0, kMaxJointIndex);
                                    JointWeights[w] = (float)Weight.weight;
                                }
                            }

                            Vertex.JointIndices = JointIndices;
                            // Quantizes to a quartet summing to 255, and a zero-sum set becomes rigid to joint 0.
                            Vertex.JointWeights = PackSkinWeights(JointWeights);
                        }

                        ++CornerCursor;
                    }
                }

                FPendingSurface Surface;
                Surface.Start = SurfaceStart;
                Surface.Count = (uint32)CornerCursor - SurfaceStart;
                Surface.Material = -1;

                if (Part.index < Mesh.materials.count)
                {
                    const ufbx_material* Material = Mesh.materials.data[Part.index];
                    if (Material != nullptr && Material->typed_id < static_cast<uint32_t>(MaterialToUnique.Num())
                        && MaterialToUnique[Material->typed_id] >= 0)
                    {
                        Surface.Material = (int16)MaterialToUnique[Material->typed_id];
                    }
                }

                PendingSurfaces.Add(Surface);
            }

            // Two corners collapse only when every attribute the renderer reads is bit-identical.
            ufbx_vertex_stream Stream = {};
            Stream.data         = Corners.GetData();
            Stream.vertex_count = CornerCursor;
            Stream.vertex_size  = sizeof(FSourceSkinnedVertex);

            ufbx_allocator_opts IndexAllocator = {};
            IndexAllocator.allocator = MakeTrackedAllocator();

            ufbx_error IndexError;
            int32 UniqueVertices = IntCastChecked<int32>(ufbx_generate_indices(&Stream, 1, Indices.GetData(), CornerCursor,
                                                                                &IndexAllocator, &IndexError));
            if (UniqueVertices == 0 && CornerCursor > 0)
            {
                // Welding is an optimization; a failure must not lose the mesh.
                LOG_WARN("[FBX] Mesh '{}': index generation failed ({}), keeping unwelded vertices.",
                         MeshName, FormatUfbxError(IndexError));
                UniqueVertices = CornerCursor;
                for (int32 i = 0; i < CornerCursor; ++i)
                {
                    Indices[i] = (uint32)i;
                }
            }

            Resource->ResizeVertices(UniqueVertices);
            for (int32 i = 0; i < UniqueVertices; ++i)
            {
                const FSourceSkinnedVertex& V = Corners[i];
                Resource->Positions[i] = V.Position;
                Resource->Normals[i]   = V.Normal;
                Resource->Tangents[i]  = V.Tangent;
                Resource->UVs[i]       = V.UV;
                Resource->UVs1[i]      = V.UV1;
                Resource->Colors[i]    = V.Color;
                if (bSkinned)
                {
                    Resource->JointIndices[i] = V.JointIndices;
                    Resource->JointWeights[i] = V.JointWeights;
                }
            }

            Resource->Indices.Reset();
            Resource->Indices.Append(Indices.begin(), Indices.begin() + CornerCursor);

            Resource->GeometrySurfaces.Reserve(PendingSurfaces.Num());
            for (int32 i = 0; i < PendingSurfaces.Num(); ++i)
            {
                const FPendingSurface& Pending = PendingSurfaces[i];

                FGeometrySurface Surface;
                FFixedString SurfaceName = MeshName;
                if (PendingSurfaces.Num() > 1)
                {
                    SurfaceName.Append("_").Append(Format("{}", i));
                }
                Surface.ID            = SurfaceName;
                Surface.StartIndex    = Pending.Start;
                Surface.IndexCount    = Pending.Count;
                Surface.MaterialIndex = Pending.Material;
                Resource->GeometrySurfaces.Add(Surface);
            }

            if (!bSkipFinalize)
            {
                if (Options.bOptimize)
                {
                    OptimizeNewlyImportedMesh(*Resource);
                }
                GenerateMeshlets(*Resource);
            }

            (bSkinned ? Slot.Skinned : Slot.Static) = Move(Resource);

            if (Progress)
            {
                const uint32 Done = Completed.fetch_add(1) + 1;
                const FFixedString Message = FormatAs<FFixedString>("Processing geometry ({}/{} meshes)...",
                                                                    Done, (uint32)UniqueMeshes.Num());
                Progress->EnterProgressFrame(GeometryStep, Message);
            }
        });

        OutData.Resources.Reserve(UniqueMeshes.Num());
        OutData.MeshSlots.SetNum(Slots.Num());

        for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
        {
            FMeshSlotResult& Slot = Slots[SlotIndex];
            OutData.bHasVertexColors |= Slot.bHasColors;

            if (Slot.Static && Slot.Static->GetNumVertices() > 0)
            {
                OutData.MeshSlots[SlotIndex].StaticResource = OutData.Resources.Num();
                OutData.Resources.Add(Move(Slot.Static));
            }
            if (Slot.Skinned && Slot.Skinned->GetNumVertices() > 0)
            {
                OutData.MeshSlots[SlotIndex].SkinnedResource = OutData.Resources.Num();
                OutData.Resources.Add(Move(Slot.Skinned));
            }
        }

        //~ Baked rather than read curve-by-curve, since FBX composes from pivots and per-axis Euler defaults.

        if (Options.bImportAnimations && Scene->anim_stacks.count > 0)
        {
            if (Progress)
            {
                Progress->UpdateMessage("Reading animations...");
            }

            OutData.Animations.Reserve(IntCastChecked<int32>(Scene->anim_stacks.count));

            for (const ufbx_anim_stack* AnimStack : Scene->anim_stacks)
            {
                ufbx_bake_opts BakeOptions = {};
                BakeOptions.temp_allocator.allocator   = MakeTrackedAllocator();
                BakeOptions.result_allocator.allocator = MakeTrackedAllocator();
                // Clips play from zero, so a take authored on frames 30 to 60 would start frozen at bind pose.
                BakeOptions.trim_start_time = true;

                ufbx_error BakeError;
                ufbx_baked_anim* Baked = ufbx_bake_anim(Scene, AnimStack->anim, &BakeOptions, &BakeError);
                if (Baked == nullptr)
                {
                    LOG_WARN("[FBX] Animation '{}' failed to bake: {}",
                             ToView(AnimStack->name).GetData(), FormatUfbxError(BakeError));
                    continue;
                }

                TUniquePtr<FAnimationResource> Clip = MakeUnique<FAnimationResource>();
                Clip->Name = !ToView(AnimStack->name).IsEmpty() ? FName(*ToFixed(AnimStack->name))
                                                          : FName("Animation");
                Clip->Duration = (float)Baked->playback_duration;
                Clip->Channels.Reserve(IntCastChecked<int32>(Baked->nodes.count * 3));

                for (const ufbx_baked_node& BakedNode : Baked->nodes)
                {
                    if (BakedNode.typed_id >= Scene->nodes.count)
                    {
                        continue;
                    }

                    const ufbx_node& Node = *Scene->nodes.data[BakedNode.typed_id];
                    const FName TargetBone = !ToView(Node.name).IsEmpty()
                        ? FName(*ToFixed(Node.name)) : FName("Bone", (uint32)Node.typed_id);

                    if (BakedNode.translation_keys.count > 0)
                    {
                        FAnimationChannel Channel;
                        Channel.TargetBone = TargetBone;
                        Channel.TargetPath = FAnimationChannel::ETargetPath::Translation;
                        Channel.Timestamps.Reserve(IntCastChecked<int32>(BakedNode.translation_keys.count));
                        Channel.Translations.Reserve(IntCastChecked<int32>(BakedNode.translation_keys.count));
                        for (const ufbx_baked_vec3& Key : BakedNode.translation_keys)
                        {
                            Channel.Timestamps.Add((float)Key.time);
                            Channel.Translations.Add(Basis.KeyTranslation(Node, ToVector3(Key.value)));
                        }
                        Clip->Channels.Add(Move(Channel));
                    }

                    if (BakedNode.rotation_keys.count > 0)
                    {
                        FAnimationChannel Channel;
                        Channel.TargetBone = TargetBone;
                        Channel.TargetPath = FAnimationChannel::ETargetPath::Rotation;
                        Channel.Timestamps.Reserve(IntCastChecked<int32>(BakedNode.rotation_keys.count));
                        Channel.Rotations.Reserve(IntCastChecked<int32>(BakedNode.rotation_keys.count));
                        for (const ufbx_baked_quat& Key : BakedNode.rotation_keys)
                        {
                            Channel.Timestamps.Add((float)Key.time);
                            Channel.Rotations.Add(Basis.KeyRotation(Node, ToQuat(Key.value)));
                        }
                        Clip->Channels.Add(Move(Channel));
                    }

                    if (BakedNode.scale_keys.count > 0)
                    {
                        FAnimationChannel Channel;
                        Channel.TargetBone = TargetBone;
                        Channel.TargetPath = FAnimationChannel::ETargetPath::Scale;
                        Channel.Timestamps.Reserve(IntCastChecked<int32>(BakedNode.scale_keys.count));
                        Channel.Scales.Reserve(IntCastChecked<int32>(BakedNode.scale_keys.count));
                        for (const ufbx_baked_vec3& Key : BakedNode.scale_keys)
                        {
                            Channel.Timestamps.Add((float)Key.time);
                            Channel.Scales.Add(ToVector3(Key.value));
                        }
                        Clip->Channels.Add(Move(Channel));
                    }
                }

                ufbx_free_baked_anim(Baked);

                if (!Clip->Channels.IsEmpty())
                {
                    OutData.Animations.Add(Move(Clip));
                }
            }
        }

        LOG_INFO("[FBX] '{}': {} nodes, {} mesh instances -> {} unique meshes, {} materials, {} images, "
                 "{} bones, {} animations.",
                 SourceName.GetData(), OutData.SourceNodeCount, (uint32)OutData.MeshInstances.Num(),
                 (uint32)OutData.Resources.Num(), (uint32)OutData.Materials.Num(),
                 (uint32)OutData.Images.Num(), (uint32)BoneNodes.Num(), (uint32)OutData.Animations.Num());

        return true;
    }
}
