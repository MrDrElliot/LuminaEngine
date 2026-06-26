#include "pch.h"

#include <filesystem>
#include <meshoptimizer.h>
#include <fastgltf/base64.hpp>
#include <fastgltf/core.hpp>
#include "Tools/Import/FastGLTFLuminaTraits.h"
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include "Core/Math/Math.h"

#include "MeshFormatImport.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Core/Progress/SlowTask.h"
#include "Core/Threading/Atomic.h"
#include "FileSystem/FileSystem.h"
#include "Memory/Memory.h"
#include "Paths/Paths.h"
#include "Renderer/MeshData.h"
#include "Renderer/RendererUtils.h"
#include "Renderer/RenderResource.h"
#include "Renderer/Vertex.h"
#include "Renderer/ViewVolume.h"
#include "TaskSystem/TaskSystem.h"

namespace Lumina::Import::Mesh::GLTF
{
    namespace
    {
        TExpected<fastgltf::Asset, FString> ExtractAsset(FStringView InPath, FScopedSlowTask* Progress)
        {
            std::filesystem::path FSPath(InPath.begin(), InPath.end());

            if (Progress)
            {
                Progress->UpdateMessage("Loading source file...");
            }

            fastgltf::GltfDataBuffer Buffer;

            if (!Buffer.loadFromFile(FSPath))
            {
                return TUnexpected(std::format("Failed to load glTF model with path: {0}. Aborting import.", FSPath.string()).c_str());
            }

            fastgltf::GltfType SourceType = fastgltf::determineGltfFileType(&Buffer);

            if (SourceType == fastgltf::GltfType::Invalid)
            {
                return TUnexpected(std::format("Failed to determine glTF file type with path: {0}. Aborting import.", FSPath.string()).c_str());
            }

            if (Progress)
            {
                Progress->UpdateMessage("Parsing glTF data...");
            }

            // Do NOT decompose node matrices: TRS decomposition is lossy for sheared matrices,
            // which misplaces parts when combining meshes. NodeLocalMatrix handles both variants exactly.
            constexpr fastgltf::Options options = fastgltf::Options::DontRequireValidAssetMember
            | fastgltf::Options::LoadGLBBuffers
            | fastgltf::Options::LoadExternalBuffers
            | fastgltf::Options::GenerateMeshIndices;

            fastgltf::Expected<fastgltf::Asset> Asset(fastgltf::Error::None);

            constexpr fastgltf::Extensions extensions =
                  fastgltf::Extensions::KHR_texture_transform
                | fastgltf::Extensions::KHR_texture_basisu
                | fastgltf::Extensions::MSFT_texture_dds
                | fastgltf::Extensions::KHR_mesh_quantization
                | fastgltf::Extensions::EXT_meshopt_compression
                | fastgltf::Extensions::KHR_lights_punctual
                | fastgltf::Extensions::EXT_texture_webp
                | fastgltf::Extensions::KHR_materials_specular
                | fastgltf::Extensions::KHR_materials_ior
                | fastgltf::Extensions::KHR_materials_iridescence
                | fastgltf::Extensions::KHR_materials_volume
                | fastgltf::Extensions::KHR_materials_transmission
                | fastgltf::Extensions::KHR_materials_clearcoat
                | fastgltf::Extensions::KHR_materials_emissive_strength
                | fastgltf::Extensions::KHR_materials_sheen
                | fastgltf::Extensions::KHR_materials_unlit
                | fastgltf::Extensions::KHR_materials_anisotropy
                | fastgltf::Extensions::EXT_mesh_gpu_instancing
                | fastgltf::Extensions::MSFT_packing_normalRoughnessMetallic
                | fastgltf::Extensions::MSFT_packing_occlusionRoughnessMetallic
                | fastgltf::Extensions::KHR_materials_dispersion
                | fastgltf::Extensions::KHR_materials_variants;

            fastgltf::Parser Parser(extensions);
            if (SourceType == fastgltf::GltfType::glTF)
            {
                Asset = Parser.loadGltf(&Buffer, FSPath.parent_path(), options);
            }
            else if (SourceType == fastgltf::GltfType::GLB)
            {
                Asset = Parser.loadGltfBinary(&Buffer, FSPath.parent_path(), options);
            }

            if (const fastgltf::Error& Error = Asset.error(); Error != fastgltf::Error::None)
            {
                return TUnexpected(std::format("Failed to load asset source with path: {0}. [{1}]: {2} Aborting import.", FSPath.string(),
                fastgltf::getErrorName(Error), fastgltf::getErrorMessage(Error)).c_str());
            }

            return Move(Asset.get());
        }
    }

    TExpected<FMeshImportData, FString> ImportGLTF(const FMeshImportOptions& ImportOptions, FStringView FilePath, FScopedSlowTask* Progress)
    {
        TExpected<fastgltf::Asset, FString> ExpectedAsset = ExtractAsset(FilePath.data(), Progress);
        if (ExpectedAsset.IsError())
        {
            return TUnexpected(ExpectedAsset.Error());
        }
        if (Progress)
        {
            Progress->EnterProgressFrame(0.40f, "Reading scene data...");
        }

        const fastgltf::Asset& Asset = ExpectedAsset.Value();
        float ImportScale = ImportOptions.Scale;
        
        FStringView Name = VFS::FileName(FilePath, true);
        
        FMeshImportData ImportData;
        ImportData.Resources.reserve(Asset.meshes.size());
        
        for (const fastgltf::Animation& Animation : Asset.animations)
        {
            TUniquePtr<FAnimationResource> AnimClip = MakeUnique<FAnimationResource>();
            AnimClip->Name = Animation.name.c_str();
            
            for (const fastgltf::AnimationChannel& Channel : Animation.channels)
            {
                FAnimationChannel AnimChannel;
                
                size_t NodeIndex = Channel.nodeIndex.value();
                const fastgltf::Node& Node = Asset.nodes[NodeIndex];
                AnimChannel.TargetBone = FName(Node.name.empty() ? ("Bone_" + eastl::to_string(NodeIndex)) : Node.name.c_str());
                
                if (Channel.path == fastgltf::AnimationPath::Translation)
                {
                    AnimChannel.TargetPath = FAnimationChannel::ETargetPath::Translation;
                }
                else if (Channel.path == fastgltf::AnimationPath::Rotation)
                {
                    AnimChannel.TargetPath = FAnimationChannel::ETargetPath::Rotation;
                }
                else if (Channel.path == fastgltf::AnimationPath::Scale)
                {
                    AnimChannel.TargetPath = FAnimationChannel::ETargetPath::Scale;
                }
                
                const auto& Sampler = Animation.samplers[Channel.samplerIndex];
                
                const auto& TimeAccessor = Asset.accessors[Sampler.inputAccessor];
                fastgltf::iterateAccessor<float>(Asset, TimeAccessor, [&](float time)
                {
                    AnimChannel.Timestamps.push_back(time);
                });
                
                const auto& ValueAccessor = Asset.accessors[Sampler.outputAccessor];
                if (Channel.path == fastgltf::AnimationPath::Translation || Channel.path == fastgltf::AnimationPath::Scale)
                {
                    fastgltf::iterateAccessor<FVector3>(Asset, ValueAccessor, [&](FVector3 Value)
                    {
                        if (Channel.path == fastgltf::AnimationPath::Translation)
                        {
                            Value *= ImportScale;
                            AnimChannel.Translations.push_back(Value);
                        }
                        else
                        {
                            AnimChannel.Scales.push_back(Value);
                        }
                    });
                }
                else if (Channel.path == fastgltf::AnimationPath::Rotation)
                {
                    fastgltf::iterateAccessor<FVector4>(Asset, ValueAccessor, [&](FVector4 value)
                    {
                        AnimChannel.Rotations.push_back(FQuat(value.w, value.x, value.y, value.z));
                    });
                }
                
                AnimClip->Channels.push_back(AnimChannel);
                AnimClip->Duration = Math::Max(AnimClip->Duration, AnimChannel.Timestamps.back());
            }
            
            ImportData.Animations.push_back(Move(AnimClip));
        }
        
        for (const fastgltf::Skin& Skin : Asset.skins)
        {
            TUniquePtr<FSkeletonResource> NewSkeleton = MakeUnique<FSkeletonResource>();
    
            if (Skin.name.empty())
            {
                NewSkeleton->Name = FName("Skeleton_" + eastl::to_string(ImportData.Skeletons.size()));
            }
            else
            {
                NewSkeleton->Name = FName(Skin.name.c_str());
            }
            
            NewSkeleton->Bones.reserve(Skin.joints.size());
            
            TVector<FMatrix4> InverseBindMatrices;
            if (Skin.inverseBindMatrices.has_value())
            {
                const fastgltf::Accessor& MatrixAccessor = Asset.accessors[Skin.inverseBindMatrices.value()];
                InverseBindMatrices.reserve(MatrixAccessor.count);
                
                fastgltf::iterateAccessor<FMatrix4>(Asset, MatrixAccessor, [&](const FMatrix4& matrix)
                {
                    InverseBindMatrices.push_back(matrix);
                });
            }
            
            THashMap<size_t, size_t> NodeToParent;
            for (size_t nodeIdx = 0; nodeIdx < Asset.nodes.size(); ++nodeIdx)
            {
                const fastgltf::Node& Node = Asset.nodes[nodeIdx];
                for (size_t ChildIdx : Node.children)
                {
                    NodeToParent[ChildIdx] = nodeIdx;
                }
            }
            
            for (size_t JointIdx = 0; JointIdx < Skin.joints.size(); ++JointIdx)
            {
                size_t NodeIdx = Skin.joints[JointIdx];
                const fastgltf::Node& BoneNode = Asset.nodes[NodeIdx];
                
                FSkeletonResource::FBoneInfo Bone;
                Bone.Name = FName(BoneNode.name.empty() ? ("Bone_" + eastl::to_string(NodeIdx)) : BoneNode.name.c_str());
                
                Bone.ParentIndex = -1;
                
                auto ParentIt = NodeToParent.find(NodeIdx);
                if (ParentIt != NodeToParent.end())
                {
                    size_t ParentNodeIdx = ParentIt->second;
        
                    for (size_t i = 0; i < Skin.joints.size(); ++i)
                    {
                        if (Skin.joints[i] == ParentNodeIdx)
                        {
                            Bone.ParentIndex = (int32)i;
                            break;
                        }
                    }
                }
                
                FMatrix4 LocalTransform(1.0f);
                if (auto* trs = std::get_if<fastgltf::TRS>(&BoneNode.transform))
                {
                    FMatrix4 translation = Math::Translate(FMatrix4(1.0f), FVector3(trs->translation[0], trs->translation[1], trs->translation[2]));
                    FQuat rotation(trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]);
                    FMatrix4 rotationMat = Math::ToMatrix4(rotation);
                    FMatrix4 scale = Math::Scale(FMatrix4(1.0f), FVector3(trs->scale[0], trs->scale[1], trs->scale[2]));
                    LocalTransform = translation * rotationMat * scale;
                }
                else if (auto* mat = std::get_if<fastgltf::Node::TransformMatrix>(&BoneNode.transform))
                {
                    LocalTransform = Math::MakeMat4(mat->data());
                }
                
                Bone.LocalTransform = LocalTransform;
                
                if (JointIdx < InverseBindMatrices.size())
                {
                    Bone.InvBindMatrix = InverseBindMatrices[JointIdx];
                }
                else
                {
                    Bone.InvBindMatrix = FMatrix4(1.0f);
                }
                
                NewSkeleton->Bones.push_back(Bone);
                NewSkeleton->BoneNameToIndex[Bone.Name] = (int32)JointIdx;
            }

            NewSkeleton->BuildBindPoseCache();
            ImportData.Skeletons.push_back(Move(NewSkeleton));
        }

        if (Progress)
        {
            Progress->EnterProgressFrame(0.05f, "Reading textures...");
        }

        // Per-image RelativePath, populated alongside texture extraction. Materials link to their textures by
        // this key (FMeshImportData::Textures is an unordered set, so image index alone isn't recoverable later).
        TVector<FFixedString> ImageRelativePaths(Asset.images.size());

        // Texture extraction is per-asset; tag each image with its material role so the texture factory picks correct BC/colorspace.
        if (ImportOptions.bImportTextures)
        {
            TVector<ETextureColorSpace> ImageRoles(Asset.images.size(), ETextureColorSpace::Auto);

            auto MarkImageForTexture = [&](size_t TextureIndex, ETextureColorSpace Role)
            {
                if (TextureIndex >= Asset.textures.size())
                {
                    return;
                }
                const auto& Tex = Asset.textures[TextureIndex];
                if (!Tex.imageIndex.has_value())
                {
                    return;
                }
                const size_t ImgIdx = Tex.imageIndex.value();
                if (ImgIdx >= ImageRoles.size())
                {
                    return;
                }
                ImageRoles[ImgIdx] = Role;
            };

            for (const fastgltf::Material& Material : Asset.materials)
            {
                if (Material.pbrData.baseColorTexture.has_value())
                {
                    MarkImageForTexture(Material.pbrData.baseColorTexture->textureIndex, ETextureColorSpace::SRGB);
                }
                if (Material.pbrData.metallicRoughnessTexture.has_value())
                {
                    MarkImageForTexture(Material.pbrData.metallicRoughnessTexture->textureIndex, ETextureColorSpace::PackedData);
                }
                if (Material.normalTexture.has_value())
                {
                    // Normal maps are imported as Linear (BC7 RGB), NOT NormalMap: the BC5-packed normal path
                    // is currently broken. The material output node reconstructs Z from XY either way.
                    MarkImageForTexture(Material.normalTexture->textureIndex, ETextureColorSpace::Linear);
                }
                if (Material.occlusionTexture.has_value())
                {
                    MarkImageForTexture(Material.occlusionTexture->textureIndex, ETextureColorSpace::Linear);
                }
                if (Material.emissiveTexture.has_value())
                {
                    MarkImageForTexture(Material.emissiveTexture->textureIndex, ETextureColorSpace::SRGB);
                }
            }

            uint32 ImageCounter = 0;
            for (auto& Image : Asset.images)
            {
                FMeshImportImage GLTFImage;
                GLTFImage.IntendedColorSpace = ImageRoles[ImageCounter];

                auto AssignFallbackName = [&]()
                {
                    if (!Image.name.empty())
                    {
                        GLTFImage.RelativePath = Image.name.c_str();
                    }
                    else
                    {
                        GLTFImage.RelativePath.append(Name.begin(), Name.end())
                                              .append("_Image_")
                                              .append_convert(eastl::to_string(ImageCounter));
                    }
                };

                if (auto* URI = std::get_if<fastgltf::sources::URI>(&Image.data))
                {
                    GLTFImage.RelativePath = URI->uri.c_str();
                    // Skip thumbnail on worker-thread preview parse; built on main thread after adoption.
                    if (!ImportOptions.bSkipFinalization)
                    {
                        FFixedString FullPath = Paths::Combine(VFS::Parent(FilePath), GLTFImage.RelativePath);
                        GLTFImage.DisplayImage = Textures::CreateTextureFromImport(FullPath, true, FUIntVector2(128, 128));
                    }
                    ImageRelativePaths[ImageCounter] = GLTFImage.RelativePath;
                    ImportData.Textures.emplace(Move(GLTFImage));
                }
                else if (auto* BufferView = std::get_if<fastgltf::sources::BufferView>(&Image.data))
                {
                    const fastgltf::BufferView& View = Asset.bufferViews[BufferView->bufferViewIndex];
                    const fastgltf::Buffer& Buffer   = Asset.buffers[View.bufferIndex];

                    if (auto* BufferArray = std::get_if<fastgltf::sources::Array>(&Buffer.data))
                    {
                        const uint8* Start = BufferArray->bytes.data() + View.byteOffset;
                        GLTFImage.Bytes.assign(Start, Start + View.byteLength);
                    }

                    AssignFallbackName();
                    // Skip thumbnail on worker-thread preview parse; built on main thread after adoption.
                    if (!ImportOptions.bSkipFinalization)
                    {
                        GLTFImage.DisplayImage = RenderUtils::CreateImageFromPixels(GLTFImage.Bytes, true, FUIntVector2(128, 128));
                    }
                    ImageRelativePaths[ImageCounter] = GLTFImage.RelativePath;
                    ImportData.Textures.emplace(Move(GLTFImage));
                }
                else if (auto* Array = std::get_if<fastgltf::sources::Array>(&Image.data))
                {
                    const uint8* Start = Array->bytes.data();
                    GLTFImage.Bytes.assign(Start, Start + Array->bytes.size());
                    AssignFallbackName();
                    // Skip thumbnail on worker-thread preview parse; built on main thread after adoption.
                    if (!ImportOptions.bSkipFinalization)
                    {
                        GLTFImage.DisplayImage = RenderUtils::CreateImageFromPixels(GLTFImage.Bytes, true, FUIntVector2(128, 128));
                    }
                    ImageRelativePaths[ImageCounter] = GLTFImage.RelativePath;
                    ImportData.Textures.emplace(Move(GLTFImage));
                }
                else if (auto* Vector = std::get_if<fastgltf::sources::Vector>(&Image.data))
                {
                    const uint8* Start = Vector->bytes.data();
                    GLTFImage.Bytes.assign(Start, Start + Vector->bytes.size());
                    AssignFallbackName();
                    // Skip thumbnail on worker-thread preview parse; built on main thread after adoption.
                    if (!ImportOptions.bSkipFinalization)
                    {
                        GLTFImage.DisplayImage = RenderUtils::CreateImageFromPixels(GLTFImage.Bytes, true, FUIntVector2(128, 128));
                    }
                    ImageRelativePaths[ImageCounter] = GLTFImage.RelativePath;
                    ImportData.Textures.emplace(Move(GLTFImage));
                }
                else if (auto* ByteView = std::get_if<fastgltf::sources::ByteView>(&Image.data))
                {
                    const uint8* Start = reinterpret_cast<const uint8*>(ByteView->bytes.data());
                    GLTFImage.Bytes.assign(Start, Start + ByteView->bytes.size());
                    AssignFallbackName();
                    // Skip thumbnail on worker-thread preview parse; built on main thread after adoption.
                    if (!ImportOptions.bSkipFinalization)
                    {
                        GLTFImage.DisplayImage = RenderUtils::CreateImageFromPixels(GLTFImage.Bytes, true, FUIntVector2(128, 128));
                    }
                    ImageRelativePaths[ImageCounter] = GLTFImage.RelativePath;
                    ImportData.Textures.emplace(Move(GLTFImage));
                }

                ImageCounter++;
            }
        }

        // Extract PBR material definitions. Indexed by glTF material index, which is exactly what
        // FGeometrySurface::MaterialIndex stores in non-merge mode (merge mode remaps; see below).
        if (ImportOptions.bImportMaterials)
        {
            auto ResolveTexturePath = [&](size_t TextureIndex) -> FFixedString
            {
                if (TextureIndex >= Asset.textures.size())
                {
                    return {};
                }
                const fastgltf::Texture& Tex = Asset.textures[TextureIndex];
                if (!Tex.imageIndex.has_value())
                {
                    return {};
                }
                const size_t ImgIdx = Tex.imageIndex.value();
                if (ImgIdx >= ImageRelativePaths.size())
                {
                    return {};
                }
                return ImageRelativePaths[ImgIdx];
            };

            ImportData.Materials.reserve(Asset.materials.size());
            uint32 MaterialCounter = 0;
            for (const fastgltf::Material& Mat : Asset.materials)
            {
                FMeshImportMaterial Out;

                if (Mat.name.empty())
                {
                    FFixedString GenName(FFixedString::CtorSprintf(), "%.*s_Mat%u", (int)Name.length(), Name.data(), MaterialCounter);
                    Out.Name = GenName.c_str();
                }
                else
                {
                    Out.Name = Mat.name.c_str();
                }

                Out.BaseColorFactor = FVector4(Mat.pbrData.baseColorFactor[0], Mat.pbrData.baseColorFactor[1],
                                               Mat.pbrData.baseColorFactor[2], Mat.pbrData.baseColorFactor[3]);
                Out.MetallicFactor  = (float)Mat.pbrData.metallicFactor;
                Out.RoughnessFactor = (float)Mat.pbrData.roughnessFactor;

                const float EmissiveStrength = (float)Mat.emissiveStrength;
                Out.EmissiveColor = FVector3((float)Mat.emissiveFactor[0] * EmissiveStrength,
                                             (float)Mat.emissiveFactor[1] * EmissiveStrength,
                                             (float)Mat.emissiveFactor[2] * EmissiveStrength);

                switch (Mat.alphaMode)
                {
                case fastgltf::AlphaMode::Mask:  Out.AlphaMode = EImportAlphaMode::Mask;  break;
                case fastgltf::AlphaMode::Blend: Out.AlphaMode = EImportAlphaMode::Blend; break;
                default:                         Out.AlphaMode = EImportAlphaMode::Opaque; break;
                }
                Out.AlphaCutoff = (float)Mat.alphaCutoff;
                Out.bTwoSided   = Mat.doubleSided;
                Out.bUnlit      = Mat.unlit;

                if (Mat.pbrData.baseColorTexture.has_value())
                {
                    Out.BaseColorTexture = ResolveTexturePath(Mat.pbrData.baseColorTexture->textureIndex);
                }
                if (Mat.pbrData.metallicRoughnessTexture.has_value())
                {
                    Out.MetallicRoughnessTexture = ResolveTexturePath(Mat.pbrData.metallicRoughnessTexture->textureIndex);
                }
                if (Mat.normalTexture.has_value())
                {
                    Out.NormalTexture = ResolveTexturePath(Mat.normalTexture->textureIndex);
                }
                if (Mat.emissiveTexture.has_value())
                {
                    Out.EmissiveTexture = ResolveTexturePath(Mat.emissiveTexture->textureIndex);
                }
                if (Mat.occlusionTexture.has_value())
                {
                    Out.OcclusionTexture = ResolveTexturePath(Mat.occlusionTexture->textureIndex);
                }

                ImportData.Materials.push_back(Move(Out));
                ++MaterialCounter;
            }
        }

        if (Progress)
        {
            Progress->EnterProgressFrame(0.05f, "Reading geometry...");
        }

        auto NodeLocalMatrix = [](const fastgltf::Node& Node) -> FMatrix4
        {
            if (auto* Trs = std::get_if<fastgltf::TRS>(&Node.transform))
            {
                FMatrix4 T = Math::Translate(FMatrix4(1.0f), FVector3(Trs->translation[0], Trs->translation[1], Trs->translation[2]));
                FQuat R(Trs->rotation[3], Trs->rotation[0], Trs->rotation[1], Trs->rotation[2]);
                FMatrix4 S = Math::Scale(FMatrix4(1.0f), FVector3(Trs->scale[0], Trs->scale[1], Trs->scale[2]));
                return T * Math::ToMatrix4(R) * S;
            }
            if (auto* Mat = std::get_if<fastgltf::Node::TransformMatrix>(&Node.transform))
            {
                return Math::MakeMat4(Mat->data());
            }
            return FMatrix4(1.0f);
        };

        // Per-mesh primitive extraction; concurrent across resources but serial in merge mode (shared targets).
        auto ProcessMeshPrimitives = [&](
            const fastgltf::Mesh& Mesh,
            const FFixedString&   MeshName,
            FMeshResource*        StaticTarget,
            FMeshResource*        SkinnedTarget,
            THashMap<int16, int16>* MergedMaterialRemapPtr,
            const FMatrix4&      WorldMatrix)
        {
            const FMatrix4 PosMatrix    = Math::Scale(FMatrix4(1.0f), FVector3(ImportScale)) * WorldMatrix;
            const FMatrix3 NormalMatrix = Math::Transpose(Math::Inverse(FMatrix3(WorldMatrix)));

            for (auto& Primitive : Mesh.primitives)
            {
                auto Joints  = Primitive.findAttribute("JOINTS_0");
                auto Weights = Primitive.findAttribute("WEIGHTS_0");
                const bool bSkinnedPrim = (Joints != Primitive.attributes.end() && Weights != Primitive.attributes.end());
                FMeshResource* NewResource = bSkinnedPrim ? SkinnedTarget : StaticTarget;

                FGeometrySurface NewSurface;
                NewSurface.StartIndex = (uint32)NewResource->GetNumIndices();

                FFixedString PrimitiveName;
                if (Mesh.name.empty())
                {
                    PrimitiveName.append(Name.begin(), Name.end()).append_convert(eastl::to_string(NewResource->GetNumSurfaces()));
                }
                else
                {
                    PrimitiveName.append_convert(Mesh.name);
                    if (MergedMaterialRemapPtr != nullptr)
                    {
                        PrimitiveName.append("_");
                        PrimitiveName.append_convert(eastl::to_string(NewResource->GetNumSurfaces()));
                    }
                }

                NewSurface.ID = PrimitiveName;

                if (Primitive.materialIndex.has_value())
                {
                    const int16 SourceMaterialIndex = (int16)Primitive.materialIndex.value();
                    if (MergedMaterialRemapPtr != nullptr)
                    {
                        THashMap<int16, int16>& Remap = *MergedMaterialRemapPtr;
                        auto MatIt = Remap.find(SourceMaterialIndex);
                        if (MatIt == Remap.end())
                        {
                            const int16 NewSlot = (int16)Remap.size();
                            Remap.emplace(SourceMaterialIndex, NewSlot);
                            NewSurface.MaterialIndex = NewSlot;
                        }
                        else
                        {
                            NewSurface.MaterialIndex = MatIt->second;
                        }
                    }
                    else
                    {
                        NewSurface.MaterialIndex = SourceMaterialIndex;
                    }
                }

                const size_t InitialIndex = NewResource->GetNumIndices();
                const size_t InitialVert  = NewResource->GetNumVertices();
                const fastgltf::Accessor& PosAccessor = Asset.accessors[Primitive.findAttribute("POSITION")->second];
                const size_t VertexCount = PosAccessor.count;

                NewResource->ResizeVertices(InitialVert + VertexCount);

                // Position: bulk-copy raw values, then bake the world+scale transform in place.
                fastgltf::copyFromAccessor<FVector3>(Asset, PosAccessor, NewResource->Positions.data() + InitialVert);
                for (size_t i = 0; i < VertexCount; ++i)
                {
                    FVector3& P = NewResource->Positions[InitialVert + i];
                    P = FVector3(PosMatrix * FVector4(P, 1.0f));
                }

                // Normal: bulk-copy into scratch, then transform + octahedral-pack into the stream.
                auto NormalAttr = Primitive.findAttribute("NORMAL");
                if (NormalAttr != Primitive.attributes.end())
                {
                    TVector<FVector3> ScratchNormals(VertexCount);
                    fastgltf::copyFromAccessor<FVector3>(Asset, Asset.accessors[NormalAttr->second], ScratchNormals.data());
                    for (size_t i = 0; i < VertexCount; ++i)
                    {
                        NewResource->Normals[InitialVert + i] = PackNormal(Math::Normalize(NormalMatrix * ScratchNormals[i]));
                    }
                }
                else
                {
                    const uint32 DefaultNormal = PackNormal(Math::Normalize(NormalMatrix * FViewVolume::UpAxis));
                    for (size_t i = 0; i < VertexCount; ++i)
                    {
                        NewResource->Normals[InitialVert + i] = DefaultNormal;
                    }
                }

                // UVs: ResizeVertices zeroed the stream; only fill when the attribute exists.
                auto UVAttr = Primitive.findAttribute("TEXCOORD_0");
                if (UVAttr != Primitive.attributes.end())
                {
                    fastgltf::iterateAccessorWithIndex<FVector2>(Asset, Asset.accessors[UVAttr->second], [&](FVector2 Value, size_t Index)
                    {
                        if (ImportOptions.bFlipUVs)
                        {
                            Value.y = 1.0f - Value.y;
                        }
                        NewResource->UVs[InitialVert + Index] = Math::PackHalf2x16(Value);
                    });
                }

                // Colors: default to opaque white, override from COLOR_0 when present.
                auto ColorAttr = Primitive.findAttribute("COLOR_0");
                if (ColorAttr != Primitive.attributes.end())
                {
                    fastgltf::iterateAccessorWithIndex<FVector4>(Asset, Asset.accessors[ColorAttr->second], [&](FVector4 Value, size_t Index)
                    {
                        NewResource->Colors[InitialVert + Index] = PackColor(Value);
                    });
                }
                else
                {
                    for (size_t i = 0; i < VertexCount; ++i)
                    {
                        NewResource->Colors[InitialVert + i] = 0xFFFFFFFF;
                    }
                }

                // Tangents stay zero (ResizeVertices zeroed them); MikkTSpace fills them at finalize.

                if (bSkinnedPrim)
                {
                    fastgltf::iterateAccessorWithIndex<FU8Vector4>(Asset, Asset.accessors[Joints->second], [&](FU8Vector4 Value, size_t Index)
                    {
                        NewResource->JointIndices[InitialVert + Index] = Value;
                    });
                    fastgltf::iterateAccessorWithIndex<FVector4>(Asset, Asset.accessors[Weights->second], [&](FVector4 Value, size_t Index)
                    {
                        NewResource->JointWeights[InitialVert + Index] = FU8Vector4(Value * 255.0f);
                    });
                }

                const fastgltf::Accessor& IndexAccessor = Asset.accessors[Primitive.indicesAccessor.value()];
                NewResource->Indices.reserve(InitialIndex + IndexAccessor.count);
                fastgltf::iterateAccessor<uint32>(Asset, IndexAccessor, [&](uint32 Index)
                {
                    NewResource->Indices.push_back((uint32)(InitialVert + Index));
                });

                NewSurface.IndexCount = (uint32)NewResource->GetNumIndices() - NewSurface.StartIndex;
                NewResource->GeometrySurfaces.push_back(NewSurface);
            }
        };

        // bSkipFinalization defers heavy passes to FinalizeMeshImportData at commit time.
        const bool bSkipFinalize = ImportOptions.bSkipFinalization;
        auto FinalizeResource = [&](FMeshResource& Resource)
        {
            if (bSkipFinalize)
            {
                return;
            }
            if (ImportOptions.bOptimize)
            {
                OptimizeNewlyImportedMesh(Resource);
            }
            GenerateMeshlets(Resource);
        };

        if (ImportOptions.bMergeMeshes)
        {
            // Merge mode: collapse all meshes into one static/skinned pair, deduping material slots.
            TUniquePtr<FMeshResource> MergedStaticMesh = MakeUnique<FMeshResource>();
            MergedStaticMesh->Name = FString(Name.begin(), Name.end()) + "_Mesh";

            TUniquePtr<FMeshResource> MergedSkinnedMesh = MakeUnique<FMeshResource>();
            MergedSkinnedMesh->bSkinnedMesh = true;
            MergedSkinnedMesh->Name = FString(Name.begin(), Name.end()) + "_SkeletalMesh";

            THashMap<int16, int16> MergedMaterialRemap;

            // Walk the scene graph (not Asset.meshes) to accumulate node->world transforms; required for correct placement.
            auto VisitMeshInstance = [&](size_t MeshIdx, const FMatrix4& WorldMatrix)
            {
                const fastgltf::Mesh& Mesh = Asset.meshes[MeshIdx];

                FString SanitizedMeshName = Mesh.name.c_str();
                eastl::replace(SanitizedMeshName.begin(), SanitizedMeshName.end(), '.', '_');

                FFixedString MeshName;
                if (Mesh.name.empty())
                {
                    MeshName.append(Name.begin(), Name.end()).append_convert(eastl::to_string(ImportData.Resources.size()));
                }
                else
                {
                    MeshName.append_convert(SanitizedMeshName);
                }

                ProcessMeshPrimitives(Mesh, MeshName, MergedStaticMesh.get(), MergedSkinnedMesh.get(), &MergedMaterialRemap, WorldMatrix);
            };

            const size_t SceneIdx = Asset.defaultScene.has_value() ? Asset.defaultScene.value()
                                                                   : (Asset.scenes.empty() ? size_t(-1) : 0);

            if (SceneIdx != size_t(-1))
            {
                struct FStackEntry
                {
                    size_t    NodeIdx;
                    FMatrix4 ParentWorld;
                };
                TVector<FStackEntry> Stack;
                for (size_t Root : Asset.scenes[SceneIdx].nodeIndices)
                {
                    Stack.push_back({Root, FMatrix4(1.0f)});
                }

                while (!Stack.empty())
                {
                    FStackEntry Entry = Stack.back();
                    Stack.pop_back();

                    const fastgltf::Node& Node = Asset.nodes[Entry.NodeIdx];
                    FMatrix4 World = Entry.ParentWorld * NodeLocalMatrix(Node);

                    if (Node.meshIndex.has_value())
                    {
                        VisitMeshInstance(Node.meshIndex.value(), World);
                    }

                    for (size_t Child : Node.children)
                    {
                        Stack.push_back({Child, World});
                    }
                }
            }
            else
            {
                // No scene info; fall back to identity-transform iteration.
                for (size_t MeshIdx = 0; MeshIdx < Asset.meshes.size(); ++MeshIdx)
                {
                    VisitMeshInstance(MeshIdx, FMatrix4(1.0f));
                }
            }

            // Merge mode collapsed source material indices into dense slots; expose the inverse so the
            // asset commit can map each merged mesh slot back to its FMeshImportData::Materials entry.
            ImportData.MergedMaterialSlotToSource.assign(MergedMaterialRemap.size(), 0);
            for (const auto& Pair : MergedMaterialRemap)
            {
                if (Pair.second >= 0 && (size_t)Pair.second < ImportData.MergedMaterialSlotToSource.size())
                {
                    ImportData.MergedMaterialSlotToSource[Pair.second] = Pair.first;
                }
            }

            if (Progress)
            {
                Progress->EnterProgressFrame(0.50f, "Reading geometry...");
            }

            TVector<FMeshResource*> ToFinalize;
            if (MergedStaticMesh && MergedStaticMesh->GetNumVertices() > 0)
            {
                ToFinalize.push_back(MergedStaticMesh.get());
            }
            if (MergedSkinnedMesh && MergedSkinnedMesh->GetNumVertices() > 0)
            {
                ToFinalize.push_back(MergedSkinnedMesh.get());
            }

            Task::ParallelFor((uint32)ToFinalize.size(), [&](uint32 i)
            {
                FinalizeResource(*ToFinalize[i]);
            });

            for (FMeshResource* Res : ToFinalize)
            {
                AnalyzeMeshStatistics(*Res, ImportData.MeshStatistics);
            }

            if (MergedStaticMesh && MergedStaticMesh->GetNumVertices() > 0)
            {
                ImportData.Resources.push_back(eastl::move(MergedStaticMesh));
            }
            if (MergedSkinnedMesh && MergedSkinnedMesh->GetNumVertices() > 0)
            {
                ImportData.Resources.push_back(eastl::move(MergedSkinnedMesh));
            }
        }
        else
        {
            struct FInstance
            {
                size_t       MeshIdx;
                FMatrix4    World;
                FFixedString MeshName;
            };

            TVector<FInstance> Instances;
            THashMap<size_t, uint32> InstanceCountPerMesh;

            auto EmitInstance = [&](size_t MeshIdx, const FMatrix4& World)
            {
                const fastgltf::Mesh& Mesh = Asset.meshes[MeshIdx];

                FString SanitizedMeshName = Mesh.name.c_str();
                eastl::replace(SanitizedMeshName.begin(), SanitizedMeshName.end(), '.', '_');

                FFixedString MeshName;
                if (Mesh.name.empty())
                {
                    MeshName.append(Name.begin(), Name.end()).append_convert(eastl::to_string(MeshIdx));
                }
                else
                {
                    MeshName.append_convert(SanitizedMeshName);
                }

                // Disambiguate same-mesh-multi-node so asset names don't collide.
                uint32& Count = InstanceCountPerMesh[MeshIdx];
                if (Count > 0)
                {
                    MeshName.append("_");
                    MeshName.append_convert(eastl::to_string(Count));
                }
                ++Count;

                Instances.push_back({MeshIdx, World, MeshName});
            };

            const size_t SceneIdx = Asset.defaultScene.has_value() ? Asset.defaultScene.value()
                                                                   : (Asset.scenes.empty() ? size_t(-1) : 0);

            if (SceneIdx != size_t(-1))
            {
                struct FStackEntry
                {
                    size_t    NodeIdx;
                    FMatrix4 ParentWorld;
                };
                TVector<FStackEntry> Stack;
                for (size_t Root : Asset.scenes[SceneIdx].nodeIndices)
                {
                    Stack.push_back({Root, FMatrix4(1.0f)});
                }

                while (!Stack.empty())
                {
                    FStackEntry Entry = Stack.back();
                    Stack.pop_back();

                    const fastgltf::Node& Node = Asset.nodes[Entry.NodeIdx];
                    FMatrix4 World = Entry.ParentWorld * NodeLocalMatrix(Node);

                    if (Node.meshIndex.has_value())
                    {
                        EmitInstance(Node.meshIndex.value(), World);
                    }

                    for (size_t Child : Node.children)
                    {
                        Stack.push_back({Child, World});
                    }
                }
            }
            else
            {
                // No scene info; fall back to one identity-transform instance per mesh.
                for (size_t MeshIdx = 0; MeshIdx < Asset.meshes.size(); ++MeshIdx)
                {
                    EmitInstance(MeshIdx, FMatrix4(1.0f));
                }
            }

            struct FMeshSlot
            {
                TUniquePtr<FMeshResource> Static;
                TUniquePtr<FMeshResource> Skinned;
            };

            TVector<FMeshSlot> Slots(Instances.size());

            // The geometry phase owns the final 0.5 of the parse budget, spread per mesh instance.
            const float GeometryStep = 0.5f / (float)eastl::max<size_t>((size_t)1, Instances.size());
            TAtomic<uint32> CompletedInstances{0};

            Task::ParallelFor((uint32)Instances.size(), [&](uint32 SlotIdx)
            {
                const FInstance&      Inst   = Instances[SlotIdx];
                const fastgltf::Mesh& Mesh   = Asset.meshes[Inst.MeshIdx];
                const FFixedString&   MeshNm = Inst.MeshName;

                FMeshSlot& Slot = Slots[SlotIdx];

                Slot.Static = MakeUnique<FMeshResource>();
                Slot.Static->Name = FString(MeshNm) + "_Mesh";

                Slot.Skinned = MakeUnique<FMeshResource>();
                Slot.Skinned->bSkinnedMesh = true;
                Slot.Skinned->Name = FString(MeshNm) + "_SkeletalMesh";

                ProcessMeshPrimitives(Mesh, MeshNm, Slot.Static.get(), Slot.Skinned.get(), nullptr, Inst.World);

                if (Progress)
                {
                    const uint32 Done = CompletedInstances.fetch_add(1) + 1;
                    FFixedString Msg(FFixedString::CtorSprintf(), "Reading geometry (%u/%u meshes)...", Done, (uint32)Instances.size());
                    Progress->EnterProgressFrame(GeometryStep, Msg);
                }
            });

            TVector<FMeshResource*> ToFinalize;
            ToFinalize.reserve(Slots.size() * 2);
            for (FMeshSlot& Slot : Slots)
            {
                if (Slot.Static && Slot.Static->GetNumVertices() > 0)
                {
                    ToFinalize.push_back(Slot.Static.get());
                }
                if (Slot.Skinned && Slot.Skinned->GetNumVertices() > 0)
                {
                    ToFinalize.push_back(Slot.Skinned.get());
                }
            }

            Task::ParallelFor((uint32)ToFinalize.size(), [&](uint32 i)
            {
                FinalizeResource(*ToFinalize[i]);
            });

            for (FMeshSlot& Slot : Slots)
            {
                if (Slot.Static && Slot.Static->GetNumVertices() > 0)
                {
                    AnalyzeMeshStatistics(*Slot.Static, ImportData.MeshStatistics);
                    ImportData.Resources.push_back(eastl::move(Slot.Static));
                }
                if (Slot.Skinned && Slot.Skinned->GetNumVertices() > 0)
                {
                    AnalyzeMeshStatistics(*Slot.Skinned, ImportData.MeshStatistics);
                    ImportData.Resources.push_back(eastl::move(Slot.Skinned));
                }
            }
        }

        return Move(ImportData);
    }
}
