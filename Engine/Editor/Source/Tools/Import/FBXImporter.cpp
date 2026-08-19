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
            return (S.data != nullptr) ? FStringView(S.data, S.length) : FStringView();
        }

        FORCEINLINE FFixedString ToFixed(const ufbx_string& S)
        {
            return (S.data != nullptr && S.length > 0) ? FFixedString(S.data, S.length) : FFixedString();
        }

        FORCEINLINE FVector2 ToVector2(const ufbx_vec2& V) { return FVector2((float)V.x, (float)V.y); }
        FORCEINLINE FVector3 ToVector3(const ufbx_vec3& V) { return FVector3((float)V.x, (float)V.y, (float)V.z); }
        FORCEINLINE FVector4 ToVector4(const ufbx_vec4& V) { return FVector4((float)V.x, (float)V.y, (float)V.z, (float)V.w); }
        FORCEINLINE FQuat    ToQuat(const ufbx_quat& Q)    { return FQuat((float)Q.w, (float)Q.x, (float)Q.y, (float)Q.z); }

        /** ufbx_matrix is a column-major 4x3 affine; the missing row is (0,0,0,1). */
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

        /**
         * The image file behind a material map.
         *
         * A material property does not always point straight at a file texture: 3ds Max wraps normal maps
         * in a bump shader node, and layered stacks sit in front of the real image. `file_textures` is
         * ufbx's flattened view of the actual files behind any of those, and contains the texture itself
         * for the plain case.
         */
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

        /** Lowercased leaf of a path, used to key an image and to match a texture against a file on disk. */
        FFixedString PathLeaf(FStringView Path)
        {
            FFixedString Normalized(Path.data(), Path.size());
            for (FFixedString::value_type& C : Normalized)
            {
                if (C == '\\') { C = '/'; }
            }
            const FStringView Leaf = VFS::FileName(FStringView(Normalized.c_str(), Normalized.size()), false);
            return FFixedString(Leaf.data(), Leaf.size());
        }
    }

    void CFBXImporter::ReleaseSourceData()
    {
        // Source data first: its image spans point into the scene ufbx owns.
        CMeshImporter::ReleaseSourceData();

        if (Scene != nullptr)
        {
            ufbx_free_scene(Scene);
            Scene = nullptr;
        }

        SourceBlob.clear();
        SourceBlob.shrink_to_fit();
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

        //~ Stage 1: source parsing.

        if (Progress)
        {
            Progress->UpdateMessage("Loading source file...");
        }

        if (!FileHelper::LoadFileToArray(SourceBlob, FStringView(SourcePath.c_str(), SourcePath.size())))
        {
            OutError = FString(std::format("Failed to read '{0}'.", SourcePath.c_str()).c_str());
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
        LoadOptions.filename.data   = SourcePath.c_str();
        LoadOptions.filename.length = SourcePath.size();

        // The engine is left-handed Y-up with +Z forward, which is -Z in ufbx's "front points at the
        // viewer" convention. Mirroring on X rather than Z keeps this matching what FBX imports have
        // always produced here, so existing FBX-sourced assets stay consistent with new ones.
        // ufbx applies the conversion to geometry, node transforms, skin binds and animation together,
        // and reverses winding on the mirrored meshes, which is the whole reason to hand it the job.
        LoadOptions.target_axes                = ufbx_axes_left_handed_y_up;
        LoadOptions.handedness_conversion_axis = UFBX_MIRROR_AXIS_X;
        LoadOptions.space_conversion           = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;

        // Engine world units are metres. The user's Scale is deliberately NOT applied here: the shared
        // FinalizeMeshImportData pass applies it to vertices, skeletons and animation translations, and
        // applying it twice is exactly the bug the old importer had.
        LoadOptions.target_unit_meters = 1.0f;

        // FBX geometry transforms affect the mesh but not the node's children, which an entity hierarchy
        // cannot express. Helper nodes keep instancing intact, unlike baking them into the geometry.
        LoadOptions.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
        LoadOptions.inherit_mode_handling       = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;

        LoadOptions.generate_missing_normals = true;
        LoadOptions.normalize_normals        = true;
        LoadOptions.clean_skin_weights       = true;

        // Blender writes PBR materials as legacy Phong in a documented, reversible way. Without this every
        // Blender FBX loses its roughness and metallic maps.
        LoadOptions.use_blender_pbr_material = true;

        LoadOptions.load_external_files            = bWantTextures;
        LoadOptions.ignore_missing_external_files  = true;
        LoadOptions.ignore_embedded                = !bWantTextures;

        // A broken index becomes UFBX_NO_INDEX, which the ufbx_get_vertex_* accessors read as zero.
        // Aborting instead would reject files that load fine everywhere else.
        LoadOptions.index_error_handling = UFBX_INDEX_ERROR_HANDLING_NO_INDEX;
        LoadOptions.connect_broken_elements = true;

        ufbx_error LoadError;
        Scene = ufbx_load_memory(SourceBlob.data(), SourceBlob.size(), &LoadOptions, &LoadError);
        if (Scene == nullptr)
        {
            OutError = FString(std::format("Failed to parse '{0}': {1}",
                SourcePath.c_str(), FormatUfbxError(LoadError).c_str()).c_str());
            SourceBlob.clear();
            SourceBlob.shrink_to_fit();
            return false;
        }

        for (const ufbx_warning& Warning : Scene->metadata.warnings)
        {
            LOG_WARN("[FBX] '{}': {} (x{})", SourceName.data(), ToView(Warning.description).data(), (uint32)Warning.count);
        }

        //~ Stage 2: images. ufbx already collapses the scene's textures onto a unique file list, so this is
        //~ one FSourceImage per distinct image. Embedded payloads stay VIEWS into the scene ufbx owns --
        //~ ReleaseSourceData is what frees them.

        TVector<int32> FileToImage(Scene->texture_files.count, INDEX_NONE);

        if (bWantTextures && Scene->texture_files.count > 0)
        {
            // Every place a texture's path might actually live, in decreasing order of authority. ufbx's
            // `filename` is the authored path already resolved against the FBX, which is right whenever the
            // asset travelled with its textures; the rest recover the common cases where it did not -- an
            // absolute path from the authoring machine, a flattened texture folder, or the "<name>.fbm"
            // sidecar the FBX SDK unpacks embedded media into.
            auto ResolveOnDisk = [&](const ufbx_texture_file& File, FFixedString& OutTried) -> FFixedString
            {
                const FStringView Relative = ToView(File.relative_filename);
                const FStringView Absolute = ToView(File.absolute_filename);
                const FStringView Resolved = ToView(File.filename);

                const FFixedString Leaf = PathLeaf(!Relative.empty() ? Relative
                                                 : (!Resolved.empty() ? Resolved : Absolute));

                FFixedString Fbm(SourceName.data(), SourceName.size());
                Fbm.append(".fbm");

                FFixedString Candidates[6];
                uint32 Count = 0;
                if (!Resolved.empty()) { Candidates[Count++] = FFixedString(Resolved.data(), Resolved.size()); }
                if (!Relative.empty()) { Candidates[Count++] = Paths::Combine(SourceDir, Relative); }
                if (!Absolute.empty()) { Candidates[Count++] = FFixedString(Absolute.data(), Absolute.size()); }
                if (!Leaf.empty())
                {
                    Candidates[Count++] = Paths::Combine(SourceDir, Leaf);
                    Candidates[Count++] = Paths::Combine(SourceDir, Fbm, Leaf);
                    Candidates[Count++] = Paths::Combine(SourceDir, "textures", Leaf);
                }

                for (uint32 i = 0; i < Count; ++i)
                {
                    if (Paths::Exists(FStringView(Candidates[i].c_str(), Candidates[i].size())))
                    {
                        return Candidates[i];
                    }
                    if (!OutTried.empty()) { OutTried.append(", "); }
                    OutTried.append(Candidates[i]);
                }

                return FFixedString();
            };

            OutData.Images.reserve(Scene->texture_files.count);

            for (size_t i = 0; i < Scene->texture_files.count; ++i)
            {
                const ufbx_texture_file& File = Scene->texture_files.data[i];

                FSourceImage Image;

                // The key names the image for the asset it becomes and drives the cook's filename-based
                // colour-space heuristic, so it wants to look like a filename even for embedded content.
                Image.Key = PathLeaf(!ToView(File.relative_filename).empty() ? ToView(File.relative_filename)
                                                                            : ToView(File.filename));
                if (Image.Key.empty())
                {
                    Image.Key = FFixedString(FFixedString::CtorSprintf(), "%.*s_Image_%u",
                                             (int)SourceName.length(), SourceName.data(), (uint32)i);
                }

                if (File.content.size > 0 && File.content.data != nullptr)
                {
                    Image.Bytes = TSpan<const uint8>((const uint8*)File.content.data, File.content.size);
                }
                else
                {
                    FFixedString Tried;
                    Image.ResolvedPath = ResolveOnDisk(File, Tried);
                    if (Image.ResolvedPath.empty())
                    {
                        // The single most common reason an FBX imports untextured. Named loudly, with the
                        // paths that were tried, because the silent fallback to a default material is
                        // otherwise indistinguishable from the file having no textures at all.
                        LOG_WARN("[FBX] Texture '{}' is neither embedded nor on disk; the materials using it "
                                 "will import with the neutral default. Looked in: {}",
                                 Image.Key, Tried.empty() ? FFixedString("(no path in the file)") : Tried);
                        continue;
                    }
                }

                FileToImage[i] = (int32)OutData.Images.size();
                OutData.Images.push_back(Move(Image));
            }

            uint32 EmbeddedCount = 0;
            for (const FSourceImage& Image : OutData.Images)
            {
                EmbeddedCount += Image.IsEmbedded() ? 1u : 0u;
            }

            LOG_INFO("[FBX] '{}': {} texture file(s) -> {} image(s) ({} embedded).",
                     SourceName.data(), (uint32)Scene->texture_files.count, (uint32)OutData.Images.size(),
                     (uint32)EmbeddedCount);
        }

        //~ Stage 3: materials.

        // A texture names its UV set by string. The engine carries two sets, so the name resolves to a set
        // index through the meshes that declare it; anything past set 1 clamps down onto it.
        THashMap<FFixedString, uint32> UVSetIndexByName;
        for (const ufbx_mesh* Mesh : Scene->meshes)
        {
            for (const ufbx_uv_set& Set : Mesh->uv_sets)
            {
                FFixedString Name = ToFixed(Set.name);
                if (!Name.empty() && UVSetIndexByName.find(Name) == UVSetIndexByName.end())
                {
                    UVSetIndexByName.emplace(Move(Name), Math::Min<uint32>(Set.index, 1u));
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
            if (Name.empty())
            {
                return 0;
            }
            auto It = UVSetIndexByName.find(Name);
            return (It != UVSetIndexByName.end()) ? It->second : 0u;
        };

        auto ResolveImage = [&](const ufbx_material_map& Map) -> int32
        {
            // texture_enabled is deliberately not consulted: exporters set it inconsistently, and a false
            // there is the difference between a textured import and a flat grey one.
            const ufbx_texture* File = FindFileTexture(Map.texture);
            if (File == nullptr || !File->has_file || File->file_index == UFBX_NO_INDEX)
            {
                return INDEX_NONE;
            }
            return (File->file_index < FileToImage.size()) ? FileToImage[File->file_index] : INDEX_NONE;
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

            // uv_to_texture maps a UV coordinate to the sampled texture coordinate, which is the same
            // direction the engine's scale/rotate/offset chain runs in.
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
            if (ImageIndex >= 0 && (size_t)ImageIndex < OutData.Images.size())
            {
                OutData.Images[ImageIndex].IntendedColorSpace = Role;
            }
        };

        // FBX has no masked flag and no way to point the engine's clip at anything but base-colour alpha,
        // so alpha-tested foliage would otherwise import Opaque. Flagged heuristically by name; the user
        // can override the blend mode per material afterwards.
        auto IsFoliageName = [&](const FString& MaterialName, int32 BaseColorImage) -> bool
        {
            static const char* const kKeywords[] = {
                "leaf", "leaves", "foliage", "tree", "plant", "ivy", "bush",
                "grass", "branch", "fern", "hedge", "vine", "shrub", "flower" };

            auto Lower = [](FStringView In) -> FString
            {
                FString L(In.data(), In.size());
                for (FString::value_type& C : L) { if (C >= 'A' && C <= 'Z') { C = C - 'A' + 'a'; } }
                return L;
            };

            const FString Name = Lower(FStringView(MaterialName.c_str(), MaterialName.size()));
            const FFixedString TexKey = (BaseColorImage >= 0 && (size_t)BaseColorImage < OutData.Images.size())
                ? OutData.Images[BaseColorImage].Key : FFixedString();
            const FString Texture = Lower(FStringView(TexKey.c_str(), TexKey.size()));

            for (const char* Keyword : kKeywords)
            {
                if (Name.find(Keyword) != FString::npos || Texture.find(Keyword) != FString::npos)
                {
                    return true;
                }
            }
            return false;
        };

        TVector<int32> MaterialToUnique(Scene->materials.count, INDEX_NONE);

        if (Options.bImportMaterials)
        {
            FKeyDedup MaterialDedup(Scene->materials.count);
            OutData.Materials.reserve(Scene->materials.count);

            for (size_t i = 0; i < Scene->materials.count; ++i)
            {
                const ufbx_material& Source        = *Scene->materials.data[i];
                const ufbx_material_pbr_maps& PBR  = Source.pbr;
                const ufbx_material_features& Feat = Source.features;

                FMeshImportMaterial Material;
                Material.Name = !ToView(Source.name).empty()
                    ? FString(Source.name.data, Source.name.length)
                    : FString(FFixedString(FFixedString::CtorSprintf(), "%.*s_Mat%u",
                              (int)SourceName.length(), SourceName.data(), (uint32)i).c_str());

                // ufbx normalises every shading model it knows -- Phong, Arnold, 3ds Max Physical,
                // Substance, Blender's Phong-encoded PBR -- onto this one set of maps.
                Material.BaseColorImage = ResolveImage(PBR.base_color);
                Material.NormalImage    = ResolveImage(PBR.normal_map);
                Material.EmissiveImage  = ResolveImage(PBR.emission_color);
                Material.OcclusionImage = ResolveImage(PBR.ambient_occlusion);

                // FBX authors metalness and roughness as two single-channel maps far more often than as one
                // packed image. The packed form is only assumed when both channels genuinely name the same
                // file, otherwise each keeps its own slot and the generated master samples them separately.
                const int32 MetallicImage  = ResolveImage(PBR.metalness);
                const int32 RoughnessImage = ResolveImage(PBR.roughness);
                if (MetallicImage != INDEX_NONE && MetallicImage == RoughnessImage)
                {
                    Material.MetallicRoughnessImage = MetallicImage;
                }
                else
                {
                    Material.MetallicImage  = MetallicImage;
                    Material.RoughnessImage = RoughnessImage;
                }

                // THE factor rule for FBX, and what decides whether a textured import looks right at all:
                // a texture connected to an FBX property REPLACES that property's constant, it does not tint
                // it the way a glTF factor does. Standard Surface leaves base_color sitting at 0.5 and
                // roughness at 0 next to a live map, so carrying those through as multipliers would halve
                // every albedo and drive every roughness map to a mirror.
                auto ScalarFactor = [](const ufbx_material_map& Map, int32 Image, float Default) -> float
                {
                    return (Image != INDEX_NONE) ? 1.0f : MapReal(Map, Default);
                };

                const int32 OpacityImage = ResolveImage(PBR.opacity);
                const float Opacity = (OpacityImage != INDEX_NONE) ? 1.0f : MapReal(PBR.opacity, 1.0f);

                if (Material.BaseColorImage != INDEX_NONE)
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

                // A material with neither a metalness value nor a map is a dielectric; leaving the factor at
                // the glTF default of 1 would import every Phong surface as raw metal.
                Material.MetallicFactor = (PBR.metalness.has_value || MetallicImage != INDEX_NONE)
                    ? ScalarFactor(PBR.metalness, MetallicImage, 0.0f) : 0.0f;

                Material.RoughnessFactor = ScalarFactor(PBR.roughness, RoughnessImage, 0.5f);
                if (Feat.roughness_as_glossiness.enabled)
                {
                    if (RoughnessImage == INDEX_NONE)
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
                if (Material.EmissiveImage != INDEX_NONE)
                {
                    // The map supplies the colour; only the weight stays a constant.
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

                // Alpha. The engine clips and blends on base-colour alpha only, so an opacity map that is
                // NOT the base-colour image cannot be honoured -- say so rather than importing it opaque.
                if (OpacityImage != INDEX_NONE)
                {
                    Material.AlphaMode   = EImportAlphaMode::Mask;
                    Material.AlphaCutoff = 0.5f;

                    if (OpacityImage != Material.BaseColorImage)
                    {
                        LOG_WARN("[FBX] Material '{}' uses a separate opacity map, which the engine samples "
                                 "from base-colour alpha instead. Imported as alpha-tested against the base "
                                 "colour; if that alpha is opaque the mask has no effect.", Material.Name);
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
                    (Material.MetallicRoughnessImage != INDEX_NONE) ? ResolveUVTransform(PBR.roughness) : FTextureUVTransform();

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
                // Normal maps cook as Linear (BC7 RGB), not NormalMap: the BC5-packed path is broken and
                // the material output node reconstructs Z from XY either way.
                MarkRole(Material.NormalImage,            ETextureColorSpace::Linear);

                if (!bDeduplicateMaterials)
                {
                    MaterialToUnique[i] = (int32)OutData.Materials.size();
                    OutData.Materials.push_back(Move(Material));
                    continue;
                }

                TVector<uint32> Key;
                Key.reserve(32);
                Key.push_back(QuantizeFloat(Material.BaseColorFactor.x));
                Key.push_back(QuantizeFloat(Material.BaseColorFactor.y));
                Key.push_back(QuantizeFloat(Material.BaseColorFactor.z));
                Key.push_back(QuantizeFloat(Material.BaseColorFactor.w));
                Key.push_back(QuantizeFloat(Material.MetallicFactor));
                Key.push_back(QuantizeFloat(Material.RoughnessFactor));
                Key.push_back(QuantizeFloat(Material.EmissiveColor.x));
                Key.push_back(QuantizeFloat(Material.EmissiveColor.y));
                Key.push_back(QuantizeFloat(Material.EmissiveColor.z));
                Key.push_back(QuantizeFloat(Material.AlphaCutoff));
                Key.push_back((uint32)Material.AlphaMode | (Material.bTwoSided ? 0x100u : 0u) | (Material.bUnlit ? 0x200u : 0u));
                Key.push_back((uint32)Material.BaseColorImage);
                Key.push_back((uint32)Material.MetallicRoughnessImage);
                Key.push_back((uint32)Material.MetallicImage);
                Key.push_back((uint32)Material.RoughnessImage);
                Key.push_back((uint32)Material.NormalImage);
                Key.push_back((uint32)Material.EmissiveImage);
                Key.push_back((uint32)Material.OcclusionImage);

                // Two materials pointing at the same images but mapping or filtering them differently are
                // NOT the same material; without this they collapse and one renders with the other's setup.
                for (const FTextureUVTransform& UVT : Material.UVTransforms)
                {
                    Key.push_back(UVT.TexCoordSet);
                    Key.push_back(QuantizeFloat(UVT.Offset.x));
                    Key.push_back(QuantizeFloat(UVT.Offset.y));
                    Key.push_back(QuantizeFloat(UVT.Scale.x));
                    Key.push_back(QuantizeFloat(UVT.Scale.y));
                    Key.push_back(QuantizeFloat(UVT.Rotation));
                }
                for (EImportSampler Sampler : Material.Samplers)
                {
                    Key.push_back((uint32)Sampler);
                }
                Key.push_back(QuantizeFloat(Material.IOR));
                Key.push_back(QuantizeFloat(Material.SpecularFactor));
                Key.push_back(QuantizeFloat(Material.ClearcoatFactor));
                Key.push_back(QuantizeFloat(Material.ClearcoatRoughness));

                bool bIsNew = false;
                const uint32 Slot = MaterialDedup.Insert(Move(Key), bIsNew);
                MaterialToUnique[i] = (int32)Slot;
                if (bIsNew)
                {
                    OutData.Materials.push_back(Move(Material));
                }
            }
        }

        //~ Stage 4: scene graph. One DFS from the root so nodes land parents-before-children, which is what
        //~ the prefab builder and the bone table below both need.

        if (Progress)
        {
            Progress->EnterProgressFrame(0.15f, "Discovering scene...");
        }

        // ufbx_node::typed_id indexes Scene->nodes, which makes it a dense key for every node-side table.
        // The visit order is parents-before-children, which the bone table below reuses.
        TVector<uint32> NodeVisitOrder;
        NodeVisitOrder.reserve(Scene->nodes.count);

        struct FStackEntry
        {
            const ufbx_node* Node;
            int32            ParentSceneNode;
        };

        TVector<FStackEntry> Stack;
        Stack.reserve(Math::Max<size_t>(64, Scene->nodes.count));

        // Light props are still in the file's own units; ufbx only converts transforms and geometry.
        const float SourceUnitMeters = (float)Scene->settings.original_unit_meters;

        auto FillLight = [SourceUnitMeters](FSourceSceneNode& SceneNode, const ufbx_light& Light)
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
            SceneNode.Light.LocalDirection = (Math::LengthSquared(Aim) > Math::Epsilon<float>())
                ? Math::Normalize(Aim)
                : FVector3(0.0f, -1.0f, 0.0f);

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

        OutData.SceneNodes.reserve(Scene->nodes.count);
        OutData.MeshInstances.reserve(Scene->nodes.count);

        TVector<uint8> MeshReferenced(Scene->meshes.count, 0);

        // The root itself is the scene's implicit container, so its children start at the top level.
        for (const ufbx_node* Child : Scene->root_node->children)
        {
            Stack.push_back(FStackEntry{ Child, INDEX_NONE });
        }

        uint32 VisitedNodes = 0;
        while (!Stack.empty())
        {
            const FStackEntry Entry = Stack.back();
            Stack.pop_back();
            ++VisitedNodes;

            const ufbx_node& Node = *Entry.Node;

            const int32 SceneNodeIndex = (int32)OutData.SceneNodes.size();
            NodeVisitOrder.push_back(Node.typed_id);

            FSourceSceneNode& SceneNode = OutData.SceneNodes.push_back();
            SceneNode.ParentIndex = Entry.ParentSceneNode;
            SceneNode.Name = !ToView(Node.name).empty()
                ? FName(ToFixed(Node.name).c_str())
                : FName("Node", (uint32)Node.typed_id);

            SceneNode.Translation = ToVector3(Node.local_transform.translation);
            SceneNode.Rotation    = ToQuat(Node.local_transform.rotation);
            SceneNode.Scale       = ToVector3(Node.local_transform.scale);

            if (Node.mesh != nullptr)
            {
                // MeshSlot and SlotIndex hold the raw mesh index until the dedup table rewrites them.
                const uint32 MeshIndex = Node.mesh->typed_id;
                MeshReferenced[MeshIndex] = 1;
                SceneNode.Kind     = ESourceNodeKind::Mesh;
                SceneNode.MeshSlot = (int32)MeshIndex;

                // geometry_to_world, not node_to_world: geometry helper nodes carry the mesh's own offset,
                // and the merge path bakes exactly this matrix into the vertices.
                OutData.MeshInstances.push_back(FSourceMeshInstance{
                    MeshIndex, (uint32)Node.typed_id, ToMatrix4(Node.geometry_to_world) });

                if (Node.light != nullptr)
                {
                    DetachedLights.push_back(FDetachedLight{ SceneNodeIndex, Node.light });
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
                Stack.push_back(FStackEntry{ Child, SceneNodeIndex });
            }
        }

        for (const FDetachedLight& Detached : DetachedLights)
        {
            FSourceSceneNode& LightNode = OutData.SceneNodes.push_back();
            LightNode.ParentIndex = Detached.ParentSceneNode;
            LightNode.Name = !ToView(Detached.Light->name).empty()
                ? FName(ToFixed(Detached.Light->name).c_str())
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
            for (size_t i = 0; i < Scene->meshes.count; ++i)
            {
                MeshReferenced[i] = 1;
                OutData.MeshInstances.push_back(FSourceMeshInstance{ (uint32)i, 0u, FMatrix4(1.0f) });
            }
        }

        //~ Stage 5: mesh deduplication. ufbx already shares one mesh across every node that instances it,
        //~ so this only catches exporters that emitted the same geometry more than once.

        TVector<uint32> UniqueMeshes;
        TVector<int32>  MeshToUnique(Scene->meshes.count, INDEX_NONE);
        {
            FKeyDedup MeshDedup(Scene->meshes.count);
            UniqueMeshes.reserve(Scene->meshes.count);

            TVector<uint32> Key;
            for (size_t MeshIndex = 0; MeshIndex < Scene->meshes.count; ++MeshIndex)
            {
                if (MeshReferenced[MeshIndex] == 0)
                {
                    continue;
                }

                const ufbx_mesh& Mesh = *Scene->meshes.data[MeshIndex];

                if (!bDeduplicateMeshes)
                {
                    MeshToUnique[MeshIndex] = (int32)UniqueMeshes.size();
                    UniqueMeshes.push_back((uint32)MeshIndex);
                    continue;
                }

                // Counts and material assignment, plus a hash of the position values. Cheap next to the
                // geometry pass, and specific enough that two different meshes never collapse.
                Key.clear();
                Key.push_back((uint32)Mesh.num_vertices);
                Key.push_back((uint32)Mesh.num_indices);
                Key.push_back((uint32)Mesh.num_faces);
                Key.push_back((uint32)Mesh.skin_deformers.count);
                for (const ufbx_material* Material : Mesh.materials)
                {
                    Key.push_back(Material != nullptr ? Material->typed_id : 0xFFFFFFFFu);
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
                Key.push_back((uint32)(PositionHash & 0xFFFFFFFFu));
                Key.push_back((uint32)(PositionHash >> 32));

                bool bIsNew = false;
                TVector<uint32> OwnedKey = Key;
                const uint32 Slot = MeshDedup.Insert(Move(OwnedKey), bIsNew);
                MeshToUnique[MeshIndex] = (int32)Slot;
                if (bIsNew)
                {
                    UniqueMeshes.push_back((uint32)MeshIndex);
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

        //~ Stage 6: the skeleton. Built before geometry, because the vertex joint indices reference it.
        //~ Every bone a cluster binds AND every ancestor up to the scene root goes in: an FK chain that
        //~ skips an intermediate node poses every bone below it in the wrong place.

        THashMap<uint32, int32> NodeToBone;   // ufbx_node::typed_id -> bone index
        TVector<const ufbx_node*> BoneNodes;

        {
            TVector<uint8> IsBone(Scene->nodes.count, 0);
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

            // Emitted in the scene walk's order, which is parents-before-children -- what BuildBindPoseCache
            // and every FK pass assume of the bone list.
            for (uint32 TypedId : NodeVisitOrder)
            {
                if (IsBone[TypedId] == 0)
                {
                    continue;
                }
                NodeToBone.emplace(TypedId, (int32)BoneNodes.size());
                BoneNodes.push_back(Scene->nodes.data[TypedId]);
            }
        }

        if (!BoneNodes.empty() && Options.bImportSkeleton)
        {
            TUniquePtr<FSkeletonResource> Skeleton = MakeUnique<FSkeletonResource>();
            Skeleton->Name = FName((FString(SourceName.data(), SourceName.size()) + "_Skeleton").c_str());
            Skeleton->Bones.resize(BoneNodes.size());

            for (size_t BoneIndex = 0; BoneIndex < BoneNodes.size(); ++BoneIndex)
            {
                const ufbx_node& Node = *BoneNodes[BoneIndex];

                FSkeletonResource::FBoneInfo& Bone = Skeleton->Bones[BoneIndex];
                Bone.Name           = !ToView(Node.name).empty() ? FName(ToFixed(Node.name).c_str())
                                                                 : FName("Bone", (uint32)Node.typed_id);
                Bone.LocalTransform = ToMatrix4(Node.node_to_parent);
                // Overwritten below for bones a cluster actually binds; a pure ancestor keeps the inverse of
                // its rest world transform, which is what makes FK*InvBind identity at bind pose for it too.
                Bone.InvBindMatrix  = ToMatrix4(Node.node_to_world);
                Bone.InvBindMatrix  = Math::Inverse(Bone.InvBindMatrix);

                Bone.ParentIndex = INDEX_NONE;
                if (Node.parent != nullptr)
                {
                    auto It = NodeToBone.find(Node.parent->typed_id);
                    if (It != NodeToBone.end())
                    {
                        Bone.ParentIndex = It->second;
                    }
                }

                Skeleton->BoneNameToIndex[Bone.Name] = (int32)BoneIndex;
            }

            // geometry_to_bone maps the mesh's geometry space into bone space at bind time, which is
            // exactly the inverse bind the skinning pass wants -- and it already carries whatever geometry
            // transform the mesh node had, so nothing has to be composed back in here.
            for (const ufbx_skin_deformer* Skin : Scene->skin_deformers)
            {
                for (const ufbx_skin_cluster* Cluster : Skin->clusters)
                {
                    if (Cluster->bone_node == nullptr)
                    {
                        continue;
                    }
                    auto It = NodeToBone.find(Cluster->bone_node->typed_id);
                    if (It == NodeToBone.end())
                    {
                        continue;
                    }
                    Skeleton->Bones[It->second].InvBindMatrix = ToMatrix4(Cluster->geometry_to_bone);
                }
            }

            Skeleton->BuildBindPoseCache();
            OutData.Skeletons.push_back(Move(Skeleton));
        }

        //~ Stage 7: geometry. One asset per unique mesh, in object space; the node's world transform stays
        //~ on the instance so an instanced scene costs one mesh rather than one per placement.

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

        TVector<FMeshSlotResult> Slots(UniqueMeshes.size());

        // Names are assigned serially so a collision suffix is deterministic, then the parallel pass only
        // touches its own slot.
        TVector<FFixedString> MeshNames(UniqueMeshes.size());
        {
            THashMap<FFixedString, uint32> NameCounts;
            for (size_t Slot = 0; Slot < UniqueMeshes.size(); ++Slot)
            {
                const ufbx_mesh& Mesh = *Scene->meshes.data[UniqueMeshes[Slot]];

                FFixedString Name = SanitizedSourceName(Mesh.name.data, Mesh.name.length);
                if (Name.empty())
                {
                    Name = FFixedString(FFixedString::CtorSprintf(), "%.*s_%u",
                                        (int)SourceName.length(), SourceName.data(), (uint32)UniqueMeshes[Slot]);
                }

                uint32& Count = NameCounts[Name];
                if (Count > 0)
                {
                    Name.append("_").append_convert(eastl::to_string(Count));
                }
                ++Count;

                MeshNames[Slot] = Move(Name);
            }
        }

        const bool  bSkipFinalize = Options.bSkipFinalization;
        const float GeometryStep  = 0.6f / (float)Math::Max<size_t>(1, UniqueMeshes.size());
        TAtomic<uint32> Completed{0};

        Task::ParallelFor((uint32)UniqueMeshes.size(), [&](uint32 SlotIndex)
        {
            const ufbx_mesh& Mesh = *Scene->meshes.data[UniqueMeshes[SlotIndex]];
            const FFixedString& MeshName = MeshNames[SlotIndex];

            FMeshSlotResult& Slot = Slots[SlotIndex];

            const ufbx_skin_deformer* Skin = (Mesh.skin_deformers.count > 0) ? Mesh.skin_deformers.data[0] : nullptr;
            const bool bSkinned = (Skin != nullptr) && !NodeToBone.empty();

            TUniquePtr<FMeshResource> Resource = MakeUnique<FMeshResource>();
            Resource->bSkinnedMesh = bSkinned;
            Resource->Name = FName((FString(MeshName.c_str(), MeshName.size())
                                    + (bSkinned ? "_SkeletalMesh" : "_Mesh")).c_str());

            const ufbx_vertex_vec2* UV1Source = (Mesh.uv_sets.count > 1) ? &Mesh.uv_sets.data[1].vertex_uv : nullptr;
            Slot.bHasColors = (Mesh.vertex_color.exists);

            // One vertex per triangle corner up front, welded by ufbx_generate_indices afterwards. Working
            // corner-by-corner is what makes the per-face attribute indices (split normals, hard edges) come
            // out right; welding them back down is a bitwise compare over the packed vertex.
            const size_t CornerCount = Mesh.num_triangles * 3;
            if (CornerCount == 0)
            {
                Slot.Static = Move(Resource);
                return;
            }

            TVector<FSourceSkinnedVertex> Corners(CornerCount);
            TVector<uint32>               Indices(CornerCount);
            TVector<uint32>               TriangleIndices(Mesh.max_face_triangles * 3);

            // Bone remap for this mesh's clusters, resolved once rather than per influence.
            TVector<int32> ClusterToBone;
            if (bSkinned)
            {
                ClusterToBone.resize(Skin->clusters.count, INDEX_NONE);
                for (size_t c = 0; c < Skin->clusters.count; ++c)
                {
                    const ufbx_node* BoneNode = Skin->clusters.data[c]->bone_node;
                    if (BoneNode == nullptr)
                    {
                        continue;
                    }
                    auto It = NodeToBone.find(BoneNode->typed_id);
                    if (It != NodeToBone.end())
                    {
                        ClusterToBone[c] = It->second;
                    }
                }
            }

            size_t CornerCursor = 0;

            struct FPendingSurface { uint32 Start; uint32 Count; int16 Material; };
            TVector<FPendingSurface> PendingSurfaces;
            PendingSurfaces.reserve(Mesh.material_parts.count);

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
                        TriangleIndices.data(), TriangleIndices.size(), &Mesh, Face);

                    for (uint32 i = 0; i < TriangleCount * 3; ++i)
                    {
                        const uint32 Corner = TriangleIndices[i];

                        FSourceSkinnedVertex& Vertex = Corners[CornerCursor];
                        Vertex = FSourceSkinnedVertex{};

                        Vertex.Position = ToVector3(ufbx_get_vertex_vec3(&Mesh.vertex_position, Corner));
                        Vertex.Normal   = PackNormal(Math::Normalize(
                            ToVector3(ufbx_get_vertex_vec3(&Mesh.vertex_normal, Corner))));

                        // FBX authors UVs with the OpenGL bottom-left origin; the engine samples Vulkan
                        // top-left. glTF is already top-left, which is why only this importer flips.
                        FVector2 UV(0.0f, 0.0f);
                        if (Mesh.vertex_uv.exists)
                        {
                            UV = ToVector2(ufbx_get_vertex_vec2(&Mesh.vertex_uv, Corner));
                            UV.y = 1.0f - UV.y;
                        }
                        Vertex.UV = Math::PackHalf2x16(UV);

                        // Mirror set 0 rather than leaving zeros: a material that asks for set 1 on a
                        // single-set mesh then renders like set 0 instead of collapsing to one texel.
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
                                // ufbx sorts influences by decreasing weight, so the first four are the
                                // four that matter -- no keep-the-largest search to get wrong.
                                const uint32 Count = Math::Min<uint32>((uint32)SkinVertex.num_weights, 4u);
                                for (uint32 w = 0; w < Count; ++w)
                                {
                                    const ufbx_skin_weight& Weight = Skin->weights.data[SkinVertex.weight_begin + w];
                                    const int32 Bone = (Weight.cluster_index < ClusterToBone.size())
                                        ? ClusterToBone[Weight.cluster_index] : INDEX_NONE;
                                    if (Bone < 0)
                                    {
                                        continue;
                                    }
                                    JointIndices[w] = (uint16)Math::Clamp(Bone, 0, kMaxJointIndex);
                                    JointWeights[w] = (float)Weight.weight;
                                }
                            }

                            Vertex.JointIndices = JointIndices;
                            // Normalizes and quantizes to a quartet summing to exactly 255; a zero-sum set
                            // becomes rigid-to-joint-0 rather than an all-zero matrix.
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
                    if (Material != nullptr && Material->typed_id < MaterialToUnique.size()
                        && MaterialToUnique[Material->typed_id] >= 0)
                    {
                        Surface.Material = (int16)MaterialToUnique[Material->typed_id];
                    }
                }

                PendingSurfaces.push_back(Surface);
            }

            // Weld. The stream is the packed vertex the engine stores, so two corners collapse only when
            // every attribute the renderer reads is bit-identical. The skinned members are zeroed on an
            // unskinned mesh, so one stride covers both cases.
            ufbx_vertex_stream Stream = {};
            Stream.data         = Corners.data();
            Stream.vertex_count = CornerCursor;
            Stream.vertex_size  = sizeof(FSourceSkinnedVertex);

            ufbx_allocator_opts IndexAllocator = {};
            IndexAllocator.allocator = MakeTrackedAllocator();

            ufbx_error IndexError;
            size_t UniqueVertices = ufbx_generate_indices(&Stream, 1, Indices.data(), CornerCursor,
                                                          &IndexAllocator, &IndexError);
            if (UniqueVertices == 0 && CornerCursor > 0)
            {
                // Welding is an optimisation; a failure must not lose the mesh.
                LOG_WARN("[FBX] Mesh '{}': index generation failed ({}), keeping unwelded vertices.",
                         MeshName, FormatUfbxError(IndexError));
                UniqueVertices = CornerCursor;
                for (size_t i = 0; i < CornerCursor; ++i)
                {
                    Indices[i] = (uint32)i;
                }
            }

            Resource->ResizeVertices(UniqueVertices);
            for (size_t i = 0; i < UniqueVertices; ++i)
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

            Resource->Indices.assign(Indices.begin(), Indices.begin() + CornerCursor);

            Resource->GeometrySurfaces.reserve(PendingSurfaces.size());
            for (size_t i = 0; i < PendingSurfaces.size(); ++i)
            {
                const FPendingSurface& Pending = PendingSurfaces[i];

                FGeometrySurface Surface;
                FFixedString SurfaceName = MeshName;
                if (PendingSurfaces.size() > 1)
                {
                    SurfaceName.append("_").append_convert(eastl::to_string(i));
                }
                Surface.ID            = SurfaceName;
                Surface.StartIndex    = Pending.Start;
                Surface.IndexCount    = Pending.Count;
                Surface.MaterialIndex = Pending.Material;
                Resource->GeometrySurfaces.push_back(Surface);
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
                FFixedString Message(FFixedString::CtorSprintf(), "Processing geometry (%u/%u meshes)...",
                                     Done, (uint32)UniqueMeshes.size());
                Progress->EnterProgressFrame(GeometryStep, Message);
            }
        });

        OutData.Resources.reserve(UniqueMeshes.size());
        OutData.MeshSlots.resize(Slots.size());

        for (size_t SlotIndex = 0; SlotIndex < Slots.size(); ++SlotIndex)
        {
            FMeshSlotResult& Slot = Slots[SlotIndex];
            OutData.bHasVertexColors |= Slot.bHasColors;

            if (Slot.Static && Slot.Static->GetNumVertices() > 0)
            {
                OutData.MeshSlots[SlotIndex].StaticResource = (int32)OutData.Resources.size();
                OutData.Resources.push_back(Move(Slot.Static));
            }
            if (Slot.Skinned && Slot.Skinned->GetNumVertices() > 0)
            {
                OutData.MeshSlots[SlotIndex].SkinnedResource = (int32)OutData.Resources.size();
                OutData.Resources.push_back(Move(Slot.Skinned));
            }
        }

        //~ Stage 8: animation. Baked rather than read curve-by-curve: FBX composes a transform from pivots,
        //~ pre/post-rotations and Euler curves whose per-axis defaults matter, and ufbx_bake_anim collapses
        //~ all of that into the TRS keys the engine's channels actually hold.

        if (Options.bImportAnimations && Scene->anim_stacks.count > 0)
        {
            if (Progress)
            {
                Progress->UpdateMessage("Reading animations...");
            }

            OutData.Animations.reserve(Scene->anim_stacks.count);

            for (const ufbx_anim_stack* AnimStack : Scene->anim_stacks)
            {
                ufbx_bake_opts BakeOptions = {};
                BakeOptions.temp_allocator.allocator   = MakeTrackedAllocator();
                BakeOptions.result_allocator.allocator = MakeTrackedAllocator();
                // Clips play from zero in the engine, and a take authored on frames [30,60] would otherwise
                // spend its first second frozen at the bind pose.
                BakeOptions.trim_start_time = true;

                ufbx_error BakeError;
                ufbx_baked_anim* Baked = ufbx_bake_anim(Scene, AnimStack->anim, &BakeOptions, &BakeError);
                if (Baked == nullptr)
                {
                    LOG_WARN("[FBX] Animation '{}' failed to bake: {}",
                             ToView(AnimStack->name).data(), FormatUfbxError(BakeError));
                    continue;
                }

                TUniquePtr<FAnimationResource> Clip = MakeUnique<FAnimationResource>();
                Clip->Name = !ToView(AnimStack->name).empty() ? FName(ToFixed(AnimStack->name).c_str())
                                                          : FName("Animation");
                Clip->Duration = (float)Baked->playback_duration;
                Clip->Channels.reserve(Baked->nodes.count * 3);

                for (const ufbx_baked_node& BakedNode : Baked->nodes)
                {
                    if (BakedNode.typed_id >= Scene->nodes.count)
                    {
                        continue;
                    }

                    const ufbx_node& Node = *Scene->nodes.data[BakedNode.typed_id];
                    const FName TargetBone = !ToView(Node.name).empty()
                        ? FName(ToFixed(Node.name).c_str()) : FName("Bone", (uint32)Node.typed_id);

                    if (BakedNode.translation_keys.count > 0)
                    {
                        FAnimationChannel Channel;
                        Channel.TargetBone = TargetBone;
                        Channel.TargetPath = FAnimationChannel::ETargetPath::Translation;
                        Channel.Timestamps.reserve(BakedNode.translation_keys.count);
                        Channel.Translations.reserve(BakedNode.translation_keys.count);
                        for (const ufbx_baked_vec3& Key : BakedNode.translation_keys)
                        {
                            Channel.Timestamps.push_back((float)Key.time);
                            Channel.Translations.push_back(ToVector3(Key.value));
                        }
                        Clip->Channels.push_back(Move(Channel));
                    }

                    if (BakedNode.rotation_keys.count > 0)
                    {
                        FAnimationChannel Channel;
                        Channel.TargetBone = TargetBone;
                        Channel.TargetPath = FAnimationChannel::ETargetPath::Rotation;
                        Channel.Timestamps.reserve(BakedNode.rotation_keys.count);
                        Channel.Rotations.reserve(BakedNode.rotation_keys.count);
                        for (const ufbx_baked_quat& Key : BakedNode.rotation_keys)
                        {
                            Channel.Timestamps.push_back((float)Key.time);
                            Channel.Rotations.push_back(ToQuat(Key.value));
                        }
                        Clip->Channels.push_back(Move(Channel));
                    }

                    if (BakedNode.scale_keys.count > 0)
                    {
                        FAnimationChannel Channel;
                        Channel.TargetBone = TargetBone;
                        Channel.TargetPath = FAnimationChannel::ETargetPath::Scale;
                        Channel.Timestamps.reserve(BakedNode.scale_keys.count);
                        Channel.Scales.reserve(BakedNode.scale_keys.count);
                        for (const ufbx_baked_vec3& Key : BakedNode.scale_keys)
                        {
                            Channel.Timestamps.push_back((float)Key.time);
                            Channel.Scales.push_back(ToVector3(Key.value));
                        }
                        Clip->Channels.push_back(Move(Channel));
                    }
                }

                ufbx_free_baked_anim(Baked);

                if (!Clip->Channels.empty())
                {
                    OutData.Animations.push_back(Move(Clip));
                }
            }
        }

        LOG_INFO("[FBX] '{}': {} nodes, {} mesh instances -> {} unique meshes, {} materials, {} images, "
                 "{} bones, {} animations.",
                 SourceName.data(), OutData.SourceNodeCount, (uint32)OutData.MeshInstances.size(),
                 (uint32)OutData.Resources.size(), (uint32)OutData.Materials.size(),
                 (uint32)OutData.Images.size(), (uint32)BoneNodes.size(), (uint32)OutData.Animations.size());

        return true;
    }
}
