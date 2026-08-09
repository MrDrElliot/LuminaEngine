#include "EditorPCH.h"
#include "GLTFImporter.h"

#include <cgltf.h>

#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Core/Math/Math.h"
#include "Core/Progress/SlowTask.h"
#include "Core/Threading/Atomic.h"
#include "FileSystem/FileSystem.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include "Paths/Paths.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkeletonResource.h"
#include "Renderer/Vertex.h"
#include "Renderer/ViewVolume.h"
#include "TaskSystem/TaskSystem.h"
#include "Log/Log.h"

namespace Lumina
{
    using namespace Import::Mesh;

    namespace
    {
        void* CgltfAlloc(void*, cgltf_size Size)
        {
            LUMINA_MEMORY_SCOPE("Import");
            return Memory::Malloc(Size);
        }

        void CgltfFree(void*, void* Ptr)
        {
            if (Ptr != nullptr)
            {
                Memory::Free(Ptr);
            }
        }

        FStringView ResultToString(cgltf_result Result)
        {
            switch (Result)
            {
            case cgltf_result_data_too_short:    return "file is truncated";
            case cgltf_result_unknown_format:    return "not a glTF or GLB file";
            case cgltf_result_invalid_json:      return "malformed JSON";
            case cgltf_result_invalid_gltf:      return "invalid glTF";
            case cgltf_result_out_of_memory:     return "out of memory";
            case cgltf_result_legacy_gltf:       return "glTF 1.0 is not supported";
            case cgltf_result_file_not_found:    return "file (or one of its buffers) not found";
            case cgltf_result_io_error:          return "I/O error";
            default:                             return "unknown error";
            }
        }

        FMatrix4 NodeLocalMatrix(const cgltf_node& Node)
        {
            if (Node.has_matrix)
            {
                return Math::MakeMat4(Node.matrix);
            }

            FMatrix4 Result(1.0f);
            if (Node.has_translation)
            {
                Result = Math::Translate(Result, FVector3(Node.translation[0], Node.translation[1], Node.translation[2]));
            }
            if (Node.has_rotation)
            {
                Result = Result * Math::ToMatrix4(FQuat(Node.rotation[3], Node.rotation[0], Node.rotation[1], Node.rotation[2]));
            }
            if (Node.has_scale)
            {
                Result = Math::Scale(Result, FVector3(Node.scale[0], Node.scale[1], Node.scale[2]));
            }
            return Result;
        }

        FORCEINLINE void HashCombine64(uint64& Seed, uint64 Value)
        {
            Seed ^= Value + 0x9E3779B97F4A7C15ull + (Seed << 6) + (Seed >> 2);
        }

        uint64 HashKey(const TVector<uint32>& Key)
        {
            uint64 Seed = 0xCBF29CE484222325ull;
            for (uint32 Word : Key)
            {
                HashCombine64(Seed, Word);
            }
            return Seed;
        }

        /**
         * Content-addressed dedup over small integer keys. Buckets by hash and compares the full key on a
         * hit, so a collision merges nothing it should not; the point is that equivalence is decided from
         * source-level identifiers (accessor and image indices, quantized factors) without ever touching
         * the vertex or pixel data those identifiers refer to.
         */
        class FKeyDedup
        {
        public:

            explicit FKeyDedup(size_t Reserve) { Keys.reserve(Reserve); }

            // Returns the slot this key maps to, and whether the slot is newly created.
            uint32 Insert(TVector<uint32>&& Key, bool& bOutIsNew)
            {
                const uint64 Hash = HashKey(Key);
                TVector<uint32>& Bucket = Buckets[Hash];

                for (uint32 Slot : Bucket)
                {
                    if (Keys[Slot] == Key)
                    {
                        bOutIsNew = false;
                        return Slot;
                    }
                }

                const uint32 Slot = (uint32)Keys.size();
                Keys.push_back(Move(Key));
                Bucket.push_back(Slot);
                bOutIsNew = true;
                return Slot;
            }

        private:

            THashMap<uint64, TVector<uint32>> Buckets;
            TVector<TVector<uint32>>          Keys;
        };

        FORCEINLINE uint32 QuantizeFloat(float Value)
        {
            // 1e-4 buckets: enough that authored duplicates collapse, tight enough that a deliberate
            // difference never does.
            return (uint32)(int32)Math::Round(Value * 10000.0f);
        }

        FFixedString SanitizedSourceName(const char* Raw)
        {
            FFixedString Name;
            if (Raw == nullptr)
            {
                return Name;
            }
            for (const char* C = Raw; *C != '\0'; ++C)
            {
                Name.push_back((*C == '.' || *C == '/' || *C == '\\') ? '_' : *C);
            }
            return Name;
        }

        struct FGeometryScratch
        {
            TVector<float>  Floats;
            TVector<uint32> Uints;

            float* Reserve(size_t Count)
            {
                if (Floats.size() < Count)
                {
                    Floats.resize(Count);
                }
                return Floats.data();
            }
        };
    }

    void CGLTFImporter::ReleaseSourceData()
    {
        // Source data first: its image spans point into the buffers cgltf owns.
        CMeshImporter::ReleaseSourceData();

        if (ParsedData != nullptr)
        {
            cgltf_free(ParsedData);
            ParsedData = nullptr;
        }
    }

    bool CGLTFImporter::ParseMeshSource(const FImportRequest& Request,
                                        const FMeshImportOptions& Options,
                                        FMeshImportData& OutData,
                                        FString& OutError,
                                        FScopedSlowTask* Progress)
    {
        if (ParsedData != nullptr)
        {
            cgltf_free(ParsedData);
            ParsedData = nullptr;
        }

        //~ Stage 1: source parsing.

        if (Progress)
        {
            Progress->UpdateMessage("Parsing glTF...");
        }

        cgltf_options ParseOptions = {};
        ParseOptions.memory.alloc_func = &CgltfAlloc;
        ParseOptions.memory.free_func  = &CgltfFree;

        const FFixedString SourcePath = Request.SourcePath;

        cgltf_result Result = cgltf_parse_file(&ParseOptions, SourcePath.c_str(), &ParsedData);
        if (Result != cgltf_result_success)
        {
            OutError = FString(std::format("Failed to parse '{0}': {1}.",
                SourcePath.c_str(), ResultToString(Result).data()).c_str());
            return false;
        }

        Result = cgltf_load_buffers(&ParseOptions, ParsedData, SourcePath.c_str());
        if (Result != cgltf_result_success)
        {
            OutError = FString(std::format("Failed to load buffers for '{0}': {1}.",
                SourcePath.c_str(), ResultToString(Result).data()).c_str());
            cgltf_free(ParsedData);
            ParsedData = nullptr;
            return false;
        }

        const cgltf_data& Data = *ParsedData;
        const FStringView SourceName = VFS::FileName(SourcePath, true);
        const FStringView SourceDir  = VFS::Parent(SourcePath);

        //~ Stage 2 + 3: images. Deduplicated by source key, and never copied: an embedded payload stays a
        //~ view into the buffer cgltf already parsed.

        TVector<int32> ImageToUnique(Data.images_count, INDEX_NONE);

        if (Options.bImportTextures)
        {
            THashMap<FFixedString, int32> KeyToUnique;
            OutData.Images.reserve(Data.images_count);

            for (cgltf_size i = 0; i < Data.images_count; ++i)
            {
                const cgltf_image& Image = Data.images[i];

                FSourceImage Source;
                if (Image.uri != nullptr && Image.uri[0] != '\0')
                {
                    Source.Key = Image.uri;
                    Source.ResolvedPath = Paths::Combine(SourceDir, Source.Key);
                }
                else
                {
                    Source.Key = (Image.name != nullptr && Image.name[0] != '\0')
                        ? SanitizedSourceName(Image.name)
                        : FFixedString(FFixedString::CtorSprintf(), "%.*s_Image_%u",
                                       (int)SourceName.length(), SourceName.data(), (uint32)i);

                    if (Image.buffer_view != nullptr)
                    {
                        if (const uint8* Base = cgltf_buffer_view_data(Image.buffer_view))
                        {
                            Source.Bytes = TSpan<const uint8>(Base, Image.buffer_view->size);
                        }
                    }

                    if (Source.Bytes.empty())
                    {
                        LOG_WARN("[glTF] image {} has no readable payload; skipping.", (uint32)i);
                        continue;
                    }
                }

                auto Existing = KeyToUnique.find(Source.Key);
                if (Existing != KeyToUnique.end())
                {
                    ImageToUnique[i] = Existing->second;
                    continue;
                }

                const int32 Slot = (int32)OutData.Images.size();
                KeyToUnique.emplace(Source.Key, Slot);
                ImageToUnique[i] = Slot;
                OutData.Images.push_back(Move(Source));
            }
        }

        // A texture's image can come from the core image or from a compressed-texture extension.
        auto ResolveImage = [&](const cgltf_texture_view& View) -> int32
        {
            if (View.texture == nullptr)
            {
                return INDEX_NONE;
            }

            const cgltf_image* Image = View.texture->image;
            if (Image == nullptr && View.texture->has_basisu) { Image = View.texture->basisu_image; }
            if (Image == nullptr && View.texture->has_webp)   { Image = View.texture->webp_image; }
            if (Image == nullptr)
            {
                return INDEX_NONE;
            }

            const cgltf_size Index = cgltf_image_index(&Data, Image);
            return (Index < ImageToUnique.size()) ? ImageToUnique[Index] : INDEX_NONE;
        };

        // Semantic role drives the cook's BC format and color space, so it is resolved from how each
        // material uses the image rather than guessed from the filename.
        auto MarkRole = [&](int32 ImageIndex, ETextureColorSpace Role)
        {
            if (ImageIndex >= 0 && (size_t)ImageIndex < OutData.Images.size())
            {
                OutData.Images[ImageIndex].IntendedColorSpace = Role;
            }
        };

        //~ Stage 2 + 3: materials, deduplicated by their resolved parameter set.

        TVector<int32> MaterialToUnique(Data.materials_count, INDEX_NONE);

        if (Options.bImportMaterials)
        {
            FKeyDedup MaterialDedup(Data.materials_count);
            OutData.Materials.reserve(Data.materials_count);

            for (cgltf_size i = 0; i < Data.materials_count; ++i)
            {
                const cgltf_material& Source = Data.materials[i];

                FMeshImportMaterial Material;
                Material.Name = (Source.name != nullptr && Source.name[0] != '\0')
                    ? FString(Source.name)
                    : FString(FFixedString(FFixedString::CtorSprintf(), "%.*s_Mat%u",
                              (int)SourceName.length(), SourceName.data(), (uint32)i).c_str());

                if (Source.has_pbr_metallic_roughness)
                {
                    const cgltf_pbr_metallic_roughness& PBR = Source.pbr_metallic_roughness;
                    Material.BaseColorFactor = FVector4(PBR.base_color_factor[0], PBR.base_color_factor[1],
                                                        PBR.base_color_factor[2], PBR.base_color_factor[3]);
                    Material.MetallicFactor  = PBR.metallic_factor;
                    Material.RoughnessFactor = PBR.roughness_factor;

                    Material.BaseColorImage         = ResolveImage(PBR.base_color_texture);
                    Material.MetallicRoughnessImage = ResolveImage(PBR.metallic_roughness_texture);
                }

                const float EmissiveStrength = Source.has_emissive_strength ? Source.emissive_strength.emissive_strength : 1.0f;
                Material.EmissiveColor = FVector3(Source.emissive_factor[0] * EmissiveStrength,
                                                  Source.emissive_factor[1] * EmissiveStrength,
                                                  Source.emissive_factor[2] * EmissiveStrength);

                switch (Source.alpha_mode)
                {
                case cgltf_alpha_mode_mask:  Material.AlphaMode = EImportAlphaMode::Mask;  break;
                case cgltf_alpha_mode_blend: Material.AlphaMode = EImportAlphaMode::Blend; break;
                default:                     Material.AlphaMode = EImportAlphaMode::Opaque; break;
                }
                Material.AlphaCutoff = Source.alpha_cutoff;
                Material.bTwoSided   = Source.double_sided != 0;
                Material.bUnlit      = Source.unlit != 0;

                Material.NormalImage    = ResolveImage(Source.normal_texture);
                Material.OcclusionImage = ResolveImage(Source.occlusion_texture);
                Material.EmissiveImage  = ResolveImage(Source.emissive_texture);

                MarkRole(Material.BaseColorImage,         ETextureColorSpace::SRGB);
                MarkRole(Material.MetallicRoughnessImage, ETextureColorSpace::PackedData);
                MarkRole(Material.OcclusionImage,         ETextureColorSpace::Linear);
                MarkRole(Material.EmissiveImage,          ETextureColorSpace::SRGB);
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
                Key.reserve(16);
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
                Key.push_back((uint32)Material.NormalImage);
                Key.push_back((uint32)Material.EmissiveImage);
                Key.push_back((uint32)Material.OcclusionImage);

                bool bIsNew = false;
                const uint32 Slot = MaterialDedup.Insert(Move(Key), bIsNew);
                MaterialToUnique[i] = (int32)Slot;
                if (bIsNew)
                {
                    OutData.Materials.push_back(Move(Material));
                }
            }
        }

        //~ Stage 2: scene / resource discovery. One iterative walk; world transforms accumulate down the
        //~ stack so a deep hierarchy stays linear instead of re-walking ancestors per node.

        if (Progress)
        {
            Progress->EnterProgressFrame(0.15f, "Discovering scene...");
        }

        struct FStackEntry
        {
            const cgltf_node* Node;
            FMatrix4          ParentWorld;
            int32             ParentSceneNode;
        };

        TVector<FStackEntry> Stack;
        TVector<const cgltf_scene*> Scenes;

        if (bImportAllScenes)
        {
            Scenes.reserve(Data.scenes_count);
            for (cgltf_size i = 0; i < Data.scenes_count; ++i)
            {
                Scenes.push_back(&Data.scenes[i]);
            }
        }
        else if (Data.scene != nullptr)
        {
            Scenes.push_back(Data.scene);
        }
        else if (Data.scenes_count > 0)
        {
            Scenes.push_back(&Data.scenes[0]);
        }

        // Bounded by the node count, so a 200k-node scene never grows this mid-walk.
        Stack.reserve(Math::Max<size_t>(64, Data.nodes_count));
        OutData.MeshInstances.reserve(Data.nodes_count);
        OutData.SceneNodes.reserve(Data.nodes_count);

        TVector<uint8> MeshReferenced(Data.meshes_count, 0);
        uint32 VisitedNodes = 0;

        for (const cgltf_scene* Scene : Scenes)
        {
            for (cgltf_size i = 0; i < Scene->nodes_count; ++i)
            {
                Stack.push_back(FStackEntry{ Scene->nodes[i], FMatrix4(1.0f), INDEX_NONE });
            }

            while (!Stack.empty())
            {
                const FStackEntry Entry = Stack.back();
                Stack.pop_back();
                ++VisitedNodes;

                const cgltf_node& Node = *Entry.Node;
                const FMatrix4 World = Entry.ParentWorld * NodeLocalMatrix(Node);

                // Nodes are appended in visit order, which is parents-before-children: a child is only
                // pushed once its parent has been popped and recorded.
                const int32 SceneNodeIndex = (int32)OutData.SceneNodes.size();
                FSourceSceneNode& SceneNode = OutData.SceneNodes.push_back();
                SceneNode.ParentIndex = Entry.ParentSceneNode;
                SceneNode.Name = (Node.name != nullptr && Node.name[0] != '\0')
                    ? FName(Node.name)
                    : FName("Node", (uint32)cgltf_node_index(&Data, &Node));

                if (Node.has_matrix)
                {
                    // glTF requires node matrices to be decomposable into TRS, so this never loses shear.
                    const FTransform Decomposed(Math::MakeMat4(Node.matrix));
                    SceneNode.Translation = Decomposed.GetLocation();
                    SceneNode.Rotation    = Decomposed.GetRotation();
                    SceneNode.Scale       = Decomposed.GetScale();
                }
                else
                {
                    if (Node.has_translation)
                    {
                        SceneNode.Translation = FVector3(Node.translation[0], Node.translation[1], Node.translation[2]);
                    }
                    if (Node.has_rotation)
                    {
                        SceneNode.Rotation = FQuat(Node.rotation[3], Node.rotation[0], Node.rotation[1], Node.rotation[2]);
                    }
                    if (Node.has_scale)
                    {
                        SceneNode.Scale = FVector3(Node.scale[0], Node.scale[1], Node.scale[2]);
                    }
                }

                if (Node.mesh != nullptr)
                {
                    // MeshSlot and SlotIndex hold the raw mesh index until the dedup table rewrites them.
                    const cgltf_size MeshIndex = cgltf_mesh_index(&Data, Node.mesh);
                    MeshReferenced[MeshIndex] = 1;
                    SceneNode.Kind     = ESourceNodeKind::Mesh;
                    SceneNode.MeshSlot = (int32)MeshIndex;
                    OutData.MeshInstances.push_back(FSourceMeshInstance{
                        (uint32)MeshIndex, (uint32)cgltf_node_index(&Data, &Node), World });
                }
                else if (Node.light != nullptr)
                {
                    const cgltf_light& Light = *Node.light;
                    switch (Light.type)
                    {
                    case cgltf_light_type_directional: SceneNode.Kind = ESourceNodeKind::DirectionalLight; break;
                    case cgltf_light_type_point:       SceneNode.Kind = ESourceNodeKind::PointLight;       break;
                    case cgltf_light_type_spot:        SceneNode.Kind = ESourceNodeKind::SpotLight;        break;
                    default: break;
                    }

                    SceneNode.Light.Color          = FVector3(Light.color[0], Light.color[1], Light.color[2]);
                    SceneNode.Light.Intensity      = Light.intensity;
                    SceneNode.Light.Range          = Light.range;
                    SceneNode.Light.InnerConeAngle = Light.spot_inner_cone_angle;
                    SceneNode.Light.OuterConeAngle = Light.spot_outer_cone_angle;
                }
                // Always parsed: the settings dialogue runs after this, so gating on bImportCameras here
                // would read whatever the previous import left behind. BuildScenePrefab applies the option.
                else if (Node.camera != nullptr)
                {
                    const cgltf_camera& Camera = *Node.camera;
                    SceneNode.Kind = ESourceNodeKind::Camera;

                    if (Camera.type == cgltf_camera_type_orthographic)
                    {
                        const cgltf_camera_orthographic& Ortho = Camera.data.orthographic;
                        SceneNode.Camera.bOrthographic = true;
                        // xmag is the half-width; the engine's ortho width spans the whole viewport.
                        SceneNode.Camera.OrthoWidth = Ortho.xmag * 2.0f;
                        SceneNode.Camera.ZNear      = Ortho.znear;
                        SceneNode.Camera.ZFar       = Ortho.zfar;
                    }
                    else
                    {
                        const cgltf_camera_perspective& Perspective = Camera.data.perspective;
                        SceneNode.Camera.bOrthographic = false;
                        SceneNode.Camera.YFov          = Perspective.yfov;
                        SceneNode.Camera.ZNear         = Perspective.znear;
                        // An absent zfar means an infinite projection, which the engine cannot express;
                        // 0 tells the consumer to keep the component's finite default.
                        SceneNode.Camera.ZFar          = Perspective.has_zfar ? Perspective.zfar : 0.0f;
                    }
                }

                for (cgltf_size i = 0; i < Node.children_count; ++i)
                {
                    Stack.push_back(FStackEntry{ Node.children[i], World, SceneNodeIndex });
                }
            }
        }

        // A file with no scene graph still has meshes worth importing.
        if (Scenes.empty())
        {
            for (cgltf_size i = 0; i < Data.meshes_count; ++i)
            {
                MeshReferenced[i] = 1;
                OutData.MeshInstances.push_back(FSourceMeshInstance{ (uint32)i, 0u, FMatrix4(1.0f) });
            }
            VisitedNodes = (uint32)Data.meshes_count;
        }

        OutData.SourceNodeCount = VisitedNodes;

        //~ Stage 3: mesh deduplication. Meshes are keyed by the accessors their primitives reference, so
        //~ two exporter-duplicated objects collapse without comparing a single vertex.

        TVector<uint32> UniqueMeshes;         // unique slot -> representative cgltf mesh index
        TVector<int32>  MeshToUnique(Data.meshes_count, INDEX_NONE);
        {
            FKeyDedup MeshDedup(Data.meshes_count);
            UniqueMeshes.reserve(Data.meshes_count);

            TVector<uint32> Key;
            for (cgltf_size MeshIndex = 0; MeshIndex < Data.meshes_count; ++MeshIndex)
            {
                if (MeshReferenced[MeshIndex] == 0)
                {
                    continue;
                }

                const cgltf_mesh& Mesh = Data.meshes[MeshIndex];

                if (!bDeduplicateMeshes)
                {
                    MeshToUnique[MeshIndex] = (int32)UniqueMeshes.size();
                    UniqueMeshes.push_back((uint32)MeshIndex);
                    continue;
                }

                Key.clear();
                for (cgltf_size p = 0; p < Mesh.primitives_count; ++p)
                {
                    const cgltf_primitive& Primitive = Mesh.primitives[p];
                    Key.push_back((uint32)Primitive.type);
                    Key.push_back(Primitive.indices != nullptr ? (uint32)cgltf_accessor_index(&Data, Primitive.indices) : 0xFFFFFFFFu);

                    const int32 MaterialSlot = (Primitive.material != nullptr)
                        ? MaterialToUnique[cgltf_material_index(&Data, Primitive.material)] : INDEX_NONE;
                    Key.push_back((uint32)MaterialSlot);

                    Key.push_back((uint32)Primitive.attributes_count);
                    for (cgltf_size a = 0; a < Primitive.attributes_count; ++a)
                    {
                        const cgltf_attribute& Attribute = Primitive.attributes[a];
                        Key.push_back((uint32)Attribute.type);
                        Key.push_back((uint32)Attribute.index);
                        Key.push_back((uint32)cgltf_accessor_index(&Data, Attribute.data));
                    }
                }

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
                if (SceneNode.Kind == ESourceNodeKind::Mesh)
                {
                    SceneNode.MeshSlot = MeshToUnique[SceneNode.MeshSlot];
                }
            }
        }

        //~ Stage 4: geometry processing.

        // Per-primitive counts, computed once for the unique meshes. Merge mode multiplies these out over
        // the instance list rather than re-walking primitives once per placement.
        struct FMeshCounts { size_t StaticVerts, StaticIndices, SkinnedVerts, SkinnedIndices; };
        TVector<FMeshCounts> PerMeshCounts(Data.meshes_count, FMeshCounts{0, 0, 0, 0});

        auto IsSkinnedPrimitive = [](const cgltf_primitive& Primitive) -> bool
        {
            return cgltf_find_accessor(&Primitive, cgltf_attribute_type_joints, 0) != nullptr
                && cgltf_find_accessor(&Primitive, cgltf_attribute_type_weights, 0) != nullptr;
        };

        for (uint32 MeshIndex : UniqueMeshes)
        {
            const cgltf_mesh& Mesh = Data.meshes[MeshIndex];
            FMeshCounts& Counts = PerMeshCounts[MeshIndex];

            for (cgltf_size p = 0; p < Mesh.primitives_count; ++p)
            {
                const cgltf_primitive& Primitive = Mesh.primitives[p];
                if (Primitive.type != cgltf_primitive_type_triangles)
                {
                    continue;
                }
                const cgltf_accessor* Position = cgltf_find_accessor(&Primitive, cgltf_attribute_type_position, 0);
                if (Position == nullptr)
                {
                    continue;
                }

                const size_t VertexCount = Position->count;
                const size_t IndexCount  = (Primitive.indices != nullptr) ? Primitive.indices->count : VertexCount;
                const bool bSkinned = IsSkinnedPrimitive(Primitive);

                (bSkinned ? Counts.SkinnedVerts   : Counts.StaticVerts)   += VertexCount;
                (bSkinned ? Counts.SkinnedIndices : Counts.StaticIndices) += IndexCount;
            }
        }

        // Appends one mesh's primitives into the target resources. WorldMatrix is identity for the
        // deduplicated per-mesh path, where geometry stays in object space and the renderer carries the
        // per-instance transform itself.
        auto AppendMesh = [&](const cgltf_mesh& Mesh,
                              FStringView MeshName,
                              FMeshResource* StaticTarget,
                              FMeshResource* SkinnedTarget,
                              THashMap<int16, int16>* MergedMaterialRemap,
                              const FMatrix4& WorldMatrix,
                              FGeometryScratch& Scratch)
        {
            const FMatrix4 PositionMatrix = Math::Scale(FMatrix4(1.0f), FVector3(Options.Scale)) * WorldMatrix;
            const FMatrix3 NormalMatrix   = Math::Transpose(Math::Inverse(FMatrix3(WorldMatrix)));

            for (cgltf_size p = 0; p < Mesh.primitives_count; ++p)
            {
                const cgltf_primitive& Primitive = Mesh.primitives[p];
                if (Primitive.type != cgltf_primitive_type_triangles)
                {
                    continue;
                }

                const cgltf_accessor* Position = cgltf_find_accessor(&Primitive, cgltf_attribute_type_position, 0);
                if (Position == nullptr)
                {
                    continue;
                }

                const cgltf_accessor* Joints  = cgltf_find_accessor(&Primitive, cgltf_attribute_type_joints, 0);
                const cgltf_accessor* Weights = cgltf_find_accessor(&Primitive, cgltf_attribute_type_weights, 0);
                const bool bSkinned = (Joints != nullptr && Weights != nullptr);

                FMeshResource& Resource = bSkinned ? *SkinnedTarget : *StaticTarget;

                const size_t BaseVertex = Resource.GetNumVertices();
                const size_t BaseIndex  = Resource.GetNumIndices();
                const size_t VertexCount = Position->count;

                FGeometrySurface Surface;
                Surface.StartIndex = (uint32)BaseIndex;

                FFixedString SurfaceName(MeshName.data(), MeshName.size());
                if (Mesh.primitives_count > 1 || MergedMaterialRemap != nullptr)
                {
                    SurfaceName.append("_").append_convert(eastl::to_string(Resource.GetNumSurfaces()));
                }
                Surface.ID = SurfaceName;

                if (Primitive.material != nullptr)
                {
                    const cgltf_size SourceIndex = cgltf_material_index(&Data, Primitive.material);
                    const int16 UniqueIndex = (SourceIndex < MaterialToUnique.size() && MaterialToUnique[SourceIndex] >= 0)
                        ? (int16)MaterialToUnique[SourceIndex] : (int16)SourceIndex;

                    if (MergedMaterialRemap != nullptr)
                    {
                        auto It = MergedMaterialRemap->find(UniqueIndex);
                        if (It == MergedMaterialRemap->end())
                        {
                            const int16 NewSlot = (int16)MergedMaterialRemap->size();
                            MergedMaterialRemap->emplace(UniqueIndex, NewSlot);
                            Surface.MaterialIndex = NewSlot;
                        }
                        else
                        {
                            Surface.MaterialIndex = It->second;
                        }
                    }
                    else
                    {
                        Surface.MaterialIndex = UniqueIndex;
                    }
                }

                Resource.ResizeVertices(BaseVertex + VertexCount);

                // Positions land straight in the destination stream; the world+scale transform is then
                // applied in place rather than through an intermediate array.
                cgltf_accessor_unpack_floats(Position, &Resource.Positions[BaseVertex].x, VertexCount * 3);
                for (size_t i = 0; i < VertexCount; ++i)
                {
                    FVector3& P = Resource.Positions[BaseVertex + i];
                    P = FVector3(PositionMatrix * FVector4(P, 1.0f));
                }

                if (const cgltf_accessor* Normals = cgltf_find_accessor(&Primitive, cgltf_attribute_type_normal, 0))
                {
                    float* Raw = Scratch.Reserve(VertexCount * 3);
                    cgltf_accessor_unpack_floats(Normals, Raw, VertexCount * 3);
                    for (size_t i = 0; i < VertexCount; ++i)
                    {
                        const FVector3 N(Raw[i * 3 + 0], Raw[i * 3 + 1], Raw[i * 3 + 2]);
                        Resource.Normals[BaseVertex + i] = PackNormal(Math::Normalize(NormalMatrix * N));
                    }
                }
                else
                {
                    const uint32 DefaultNormal = PackNormal(Math::Normalize(NormalMatrix * FViewVolume::UpAxis));
                    for (size_t i = 0; i < VertexCount; ++i)
                    {
                        Resource.Normals[BaseVertex + i] = DefaultNormal;
                    }
                }

                // ResizeVertices zeroed every stream, so an absent attribute needs no fill.
                if (const cgltf_accessor* UVs = cgltf_find_accessor(&Primitive, cgltf_attribute_type_texcoord, 0))
                {
                    float* Raw = Scratch.Reserve(VertexCount * 2);
                    cgltf_accessor_unpack_floats(UVs, Raw, VertexCount * 2);
                    for (size_t i = 0; i < VertexCount; ++i)
                    {
                        Resource.UVs[BaseVertex + i] = Math::PackHalf2x16(FVector2(Raw[i * 2 + 0], Raw[i * 2 + 1]));
                    }
                }

                if (const cgltf_accessor* Colors = cgltf_find_accessor(&Primitive, cgltf_attribute_type_color, 0))
                {
                    const size_t Components = cgltf_num_components(Colors->type);
                    float* Raw = Scratch.Reserve(VertexCount * Components);
                    cgltf_accessor_unpack_floats(Colors, Raw, VertexCount * Components);
                    for (size_t i = 0; i < VertexCount; ++i)
                    {
                        const float* C = Raw + i * Components;
                        Resource.Colors[BaseVertex + i] = PackColor(FVector4(C[0], C[1], C[2], Components >= 4 ? C[3] : 1.0f));
                    }
                }
                else
                {
                    for (size_t i = 0; i < VertexCount; ++i)
                    {
                        Resource.Colors[BaseVertex + i] = 0xFFFFFFFFu;
                    }
                }

                if (bSkinned)
                {
                    float* RawJoints = Scratch.Reserve(VertexCount * 4);
                    cgltf_accessor_unpack_floats(Joints, RawJoints, VertexCount * 4);
                    for (size_t i = 0; i < VertexCount; ++i)
                    {
                        const float* J = RawJoints + i * 4;
                        Resource.JointIndices[BaseVertex + i] = FU8Vector4(
                            (uint8)Math::Clamp((int32)J[0], 0, 255), (uint8)Math::Clamp((int32)J[1], 0, 255),
                            (uint8)Math::Clamp((int32)J[2], 0, 255), (uint8)Math::Clamp((int32)J[3], 0, 255));
                    }

                    // glTF only guarantees WEIGHTS_0 is normalized ACROSS ALL weight sets, so a mesh using
                    // WEIGHTS_1 leaves the four retained weights summing well under 1. An unnormalized set
                    // scales the blended affine matrix and pulls those vertices toward the origin.
                    float* RawWeights = Scratch.Reserve(VertexCount * 4);
                    cgltf_accessor_unpack_floats(Weights, RawWeights, VertexCount * 4);
                    for (size_t i = 0; i < VertexCount; ++i)
                    {
                        const float* W = RawWeights + i * 4;
                        Resource.JointWeights[BaseVertex + i] = PackSkinWeights(FVector4(W[0], W[1], W[2], W[3]));
                    }
                }

                if (Primitive.indices != nullptr)
                {
                    const size_t IndexCount = Primitive.indices->count;
                    Resource.Indices.resize(BaseIndex + IndexCount);
                    cgltf_accessor_unpack_indices(Primitive.indices, Resource.Indices.data() + BaseIndex, sizeof(uint32), IndexCount);
                    for (size_t i = 0; i < IndexCount; ++i)
                    {
                        Resource.Indices[BaseIndex + i] += (uint32)BaseVertex;
                    }
                }
                else
                {
                    Resource.Indices.resize(BaseIndex + VertexCount);
                    for (size_t i = 0; i < VertexCount; ++i)
                    {
                        Resource.Indices[BaseIndex + i] = (uint32)(BaseVertex + i);
                    }
                }

                Surface.IndexCount = (uint32)Resource.GetNumIndices() - Surface.StartIndex;
                Resource.GeometrySurfaces.push_back(Surface);
            }
        };

        const bool bSkipFinalize = Options.bSkipFinalization;
        auto FinalizeResource = [&](FMeshResource& Resource)
        {
            if (bSkipFinalize)
            {
                return;
            }
            if (Options.bOptimize)
            {
                OptimizeNewlyImportedMesh(Resource);
            }
            GenerateMeshlets(Resource);
        };

        if (Progress)
        {
            Progress->EnterProgressFrame(0.10f, "Reading geometry...");
        }

        {
            // One asset per UNIQUE mesh, in object space. The scene graph decides WHICH meshes are reached;
            // a node's world transform is deliberately not baked in, because doing so forces a private copy
            // of the geometry per node. blender-3.3-splash.glb carries 187,851 mesh nodes over 89 unique
            // meshes: baking would turn 3.07M real vertices into 644M.
            struct FMeshSlot
            {
                TUniquePtr<FMeshResource> Static;
                TUniquePtr<FMeshResource> Skinned;
            };

            TVector<FMeshSlot> Slots(UniqueMeshes.size());

            // Names are assigned serially so a collision suffix is deterministic, then the parallel pass
            // only touches its own slot.
            TVector<FFixedString> MeshNames(UniqueMeshes.size());
            {
                THashMap<FFixedString, uint32> NameCounts;
                for (size_t Slot = 0; Slot < UniqueMeshes.size(); ++Slot)
                {
                    const cgltf_mesh& Mesh = Data.meshes[UniqueMeshes[Slot]];

                    FFixedString Name = (Mesh.name != nullptr && Mesh.name[0] != '\0')
                        ? SanitizedSourceName(Mesh.name)
                        : FFixedString(FFixedString::CtorSprintf(), "%.*s_%u",
                                       (int)SourceName.length(), SourceName.data(), (uint32)UniqueMeshes[Slot]);

                    // glTF does not require unique mesh names, and two DIFFERENT meshes sharing one would
                    // collide as asset names. Suffix only genuine collisions so the common case stays clean.
                    uint32& Count = NameCounts[Name];
                    if (Count > 0)
                    {
                        Name.append("_").append_convert(eastl::to_string(Count));
                    }
                    ++Count;

                    MeshNames[Slot] = Move(Name);
                }
            }

            const float GeometryStep = 0.6f / (float)Math::Max<size_t>(1, UniqueMeshes.size());
            TAtomic<uint32> Completed{0};

            Task::ParallelFor((uint32)UniqueMeshes.size(), [&](uint32 SlotIndex)
            {
                const cgltf_mesh& Mesh = Data.meshes[UniqueMeshes[SlotIndex]];
                const FFixedString& MeshName = MeshNames[SlotIndex];

                FMeshSlot& Slot = Slots[SlotIndex];
                const FMeshCounts& Counts = PerMeshCounts[UniqueMeshes[SlotIndex]];

                Slot.Static = MakeUnique<FMeshResource>();
                Slot.Static->Name = FString(MeshName) + "_Mesh";

                Slot.Skinned = MakeUnique<FMeshResource>();
                Slot.Skinned->bSkinnedMesh = true;
                Slot.Skinned->Name = FString(MeshName) + "_SkeletalMesh";

                if (Counts.StaticVerts > 0)
                {
                    Slot.Static->ReserveVertices(Counts.StaticVerts);
                    Slot.Static->Indices.reserve(Counts.StaticIndices);
                }
                if (Counts.SkinnedVerts > 0)
                {
                    Slot.Skinned->ReserveVertices(Counts.SkinnedVerts);
                    Slot.Skinned->Indices.reserve(Counts.SkinnedIndices);
                }

                // One scratch buffer per task, reused across the mesh's primitives.
                FGeometryScratch Scratch;
                AppendMesh(Mesh, FStringView(MeshName.c_str(), MeshName.size()),
                           Slot.Static.get(), Slot.Skinned.get(), nullptr, FMatrix4(1.0f), Scratch);

                if (Slot.Static->GetNumVertices() > 0)
                {
                    FinalizeResource(*Slot.Static);
                }
                if (Slot.Skinned->GetNumVertices() > 0)
                {
                    FinalizeResource(*Slot.Skinned);
                }

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
                FMeshSlot& Slot = Slots[SlotIndex];
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
        }

        //~ Skeletons and animations.

        if (Options.bImportSkeleton)
        {
            OutData.Skeletons.reserve(Data.skins_count);
            for (cgltf_size s = 0; s < Data.skins_count; ++s)
            {
                const cgltf_skin& Skin = Data.skins[s];

                TUniquePtr<FSkeletonResource> Skeleton = MakeUnique<FSkeletonResource>();
                Skeleton->Name = (Skin.name != nullptr && Skin.name[0] != '\0')
                    ? FName(Skin.name)
                    : FName("Skeleton_" + eastl::to_string(OutData.Skeletons.size()));

                Skeleton->Bones.reserve(Skin.joints_count);

                TVector<FMatrix4> InverseBindMatrices;
                if (Skin.inverse_bind_matrices != nullptr)
                {
                    InverseBindMatrices.resize(Skin.inverse_bind_matrices->count);
                    cgltf_accessor_unpack_floats(Skin.inverse_bind_matrices,
                        &InverseBindMatrices[0][0][0], InverseBindMatrices.size() * 16);
                }

                // Joint parent lookup by node pointer; the joint list is not ordered by hierarchy.
                THashMap<const cgltf_node*, int32> NodeToJoint;
                NodeToJoint.reserve(Skin.joints_count);
                for (cgltf_size j = 0; j < Skin.joints_count; ++j)
                {
                    NodeToJoint.emplace(Skin.joints[j], (int32)j);
                }

                for (cgltf_size j = 0; j < Skin.joints_count; ++j)
                {
                    const cgltf_node& JointNode = *Skin.joints[j];

                    FSkeletonResource::FBoneInfo Bone;
                    Bone.Name = (JointNode.name != nullptr && JointNode.name[0] != '\0')
                        ? FName(JointNode.name)
                        : FName("Bone_" + eastl::to_string(cgltf_node_index(&Data, &JointNode)));

                    Bone.ParentIndex = INDEX_NONE;
                    if (JointNode.parent != nullptr)
                    {
                        auto It = NodeToJoint.find(JointNode.parent);
                        if (It != NodeToJoint.end())
                        {
                            Bone.ParentIndex = It->second;
                        }
                    }

                    Bone.LocalTransform = NodeLocalMatrix(JointNode);
                    Bone.InvBindMatrix  = (j < InverseBindMatrices.size()) ? InverseBindMatrices[j] : FMatrix4(1.0f);

                    Skeleton->BoneNameToIndex[Bone.Name] = (int32)j;
                    Skeleton->Bones.push_back(Bone);
                }

                Skeleton->BuildBindPoseCache();
                OutData.Skeletons.push_back(Move(Skeleton));
            }
        }

        if (Options.bImportAnimations)
        {
            OutData.Animations.reserve(Data.animations_count);
            for (cgltf_size a = 0; a < Data.animations_count; ++a)
            {
                const cgltf_animation& Animation = Data.animations[a];

                TUniquePtr<FAnimationResource> Clip = MakeUnique<FAnimationResource>();
                Clip->Name = (Animation.name != nullptr) ? Animation.name : "";
                Clip->Channels.reserve(Animation.channels_count);

                for (cgltf_size c = 0; c < Animation.channels_count; ++c)
                {
                    const cgltf_animation_channel& Source = Animation.channels[c];
                    if (Source.target_node == nullptr || Source.sampler == nullptr)
                    {
                        continue;
                    }

                    FAnimationChannel Channel;
                    Channel.TargetBone = (Source.target_node->name != nullptr && Source.target_node->name[0] != '\0')
                        ? FName(Source.target_node->name)
                        : FName("Bone_" + eastl::to_string(cgltf_node_index(&Data, Source.target_node)));

                    switch (Source.target_path)
                    {
                    case cgltf_animation_path_type_translation: Channel.TargetPath = FAnimationChannel::ETargetPath::Translation; break;
                    case cgltf_animation_path_type_rotation:    Channel.TargetPath = FAnimationChannel::ETargetPath::Rotation;    break;
                    case cgltf_animation_path_type_scale:       Channel.TargetPath = FAnimationChannel::ETargetPath::Scale;       break;
                    default: continue;
                    }

                    const cgltf_accessor* Times  = Source.sampler->input;
                    const cgltf_accessor* Values = Source.sampler->output;
                    if (Times == nullptr || Values == nullptr || Times->count == 0)
                    {
                        continue;
                    }

                    Channel.Timestamps.resize(Times->count);
                    cgltf_accessor_unpack_floats(Times, Channel.Timestamps.data(), Times->count);

                    const size_t Components = cgltf_num_components(Values->type);
                    TVector<float> Raw(Values->count * Components);
                    cgltf_accessor_unpack_floats(Values, Raw.data(), Raw.size());

                    if (Channel.TargetPath == FAnimationChannel::ETargetPath::Translation)
                    {
                        Channel.Translations.resize(Values->count);
                        for (cgltf_size i = 0; i < Values->count; ++i)
                        {
                            Channel.Translations[i] = FVector3(Raw[i * 3 + 0], Raw[i * 3 + 1], Raw[i * 3 + 2]) * Options.Scale;
                        }
                    }
                    else if (Channel.TargetPath == FAnimationChannel::ETargetPath::Scale)
                    {
                        Channel.Scales.resize(Values->count);
                        for (cgltf_size i = 0; i < Values->count; ++i)
                        {
                            Channel.Scales[i] = FVector3(Raw[i * 3 + 0], Raw[i * 3 + 1], Raw[i * 3 + 2]);
                        }
                    }
                    else
                    {
                        Channel.Rotations.resize(Values->count);
                        for (cgltf_size i = 0; i < Values->count; ++i)
                        {
                            Channel.Rotations[i] = FQuat(Raw[i * 4 + 3], Raw[i * 4 + 0], Raw[i * 4 + 1], Raw[i * 4 + 2]);
                        }
                    }

                    Clip->Duration = Math::Max(Clip->Duration, Channel.Timestamps.back());
                    Clip->Channels.push_back(Move(Channel));
                }

                OutData.Animations.push_back(Move(Clip));
            }
        }

        LOG_INFO("[glTF] '{}': {} nodes, {} mesh instances -> {} unique meshes, {} materials, {} images.",
                 SourceName.data(), OutData.SourceNodeCount, OutData.MeshInstances.size(),
                 OutData.Resources.size(), OutData.Materials.size(), OutData.Images.size());

        return true;
    }
}
