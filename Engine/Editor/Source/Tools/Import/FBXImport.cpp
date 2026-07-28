#include "PCH.h"
#include <chrono>
#include "Core/Math/Math.h"
#include "MeshFormatImport.h"
#include "Assets/AssetTypes/Mesh/Animation/Animation.h"
#include "Core/Progress/SlowTask.h"
#include "Core/Threading/Atomic.h"
#include "Core/Utils/Defer.h"
#include "FileSystem/FileSystem.h"
#include "OpenFBX/ofbx.h"
#include "OpenFBX/libdeflate.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Renderer/MeshData.h"
#include "TaskSystem/TaskSystem.h"
#include "Log/Log.h"

// Route libdeflate (OpenFBX's bundled inflate) through Lumina's tracked allocator.
extern "C" void* LmThirdPartyMalloc(size_t Size, const char* Category);
extern "C" void  LmThirdPartyFree(void* Ptr);

namespace
{
    void* LibdeflateTrackedMalloc(size_t Size) { return LmThirdPartyMalloc(Size, "libdeflate"); }
    void  LibdeflateTrackedFree(void* Ptr)     { LmThirdPartyFree(Ptr); }
}

namespace Lumina::Import::Mesh::FBX
{
    static FMatrix4 ConvertMatrix(const ofbx::DMatrix& m)
    {
        return FMatrix4(
            m.m[0], m.m[1], m.m[2], m.m[3],
            m.m[4], m.m[5], m.m[6], m.m[7],
            m.m[8], m.m[9], m.m[10], m.m[11],
            m.m[12], m.m[13], m.m[14], m.m[15]
        );
    }

    // Conjugate by the X reflection (M' = R * M * R, R = diag(-1,1,1)).
    static void ConjugateHandedness(FMatrix4& M)
    {
        M[0][1] = -M[0][1];
        M[0][2] = -M[0][2];
        M[0][3] = -M[0][3];
        M[1][0] = -M[1][0];
        M[2][0] = -M[2][0];
        M[3][0] = -M[3][0];
    }
    
    
    TExpected<FMeshImportData, FString> ImportFBX(const FMeshImportOptions& ImportOptions, FStringView FilePath, FScopedSlowTask* Progress)
    {
        // Install our tracked allocator for libdeflate once, before the first ofbx::load.
        static const bool bLibdeflateAllocInstalled = []
        {
            libdeflate_set_memory_allocator(&LibdeflateTrackedMalloc, &LibdeflateTrackedFree);
            return true;
        }();
        (void)bLibdeflateAllocInstalled;

        // [DIAG] Time the file read vs. the ofbx parse so we can see where "Parsing source file" goes.
        if (Progress)
        {
            Progress->UpdateMessage("Loading source file...");
        }
        const auto TLoadStart = std::chrono::steady_clock::now();

        TVector<uint8> FileBlob;
        if (!FileHelper::LoadFileToArray(FileBlob, FilePath))
        {
            return TUnexpected("Failed to load file path");
        }

        const auto TParseStart = std::chrono::steady_clock::now();
        LOG_INFO("[FBX] File load: {} ms ({} MB)",
            std::chrono::duration_cast<std::chrono::milliseconds>(TParseStart - TLoadStart).count(),
            FileBlob.size() / (1024ull * 1024ull));

        if (Progress)
        {
            Progress->UpdateMessage("Parsing FBX data...");
        }
        
        ofbx::LoadFlags LoadFlags = ofbx::LoadFlags::IGNORE_CAMERAS
            | ofbx::LoadFlags::IGNORE_LIGHTS
            | ofbx::LoadFlags::IGNORE_BLEND_SHAPES
            | ofbx::LoadFlags::IGNORE_POSES;
        
        if (!ImportOptions.bImportTextures && !ImportOptions.bImportMaterials)
        {
            LoadFlags = LoadFlags | ofbx::LoadFlags::IGNORE_VIDEOS;
        }

        ofbx::IScene* FBXScene = ofbx::load(FileBlob.data(), FileBlob.size(), (uint16)LoadFlags);
        if (!FBXScene)
        {
            return TUnexpected("Failed to load FBX Scene");
        }

        const auto TSceneStart = std::chrono::steady_clock::now();
        LOG_INFO("[FBX] Parse (ofbx::load): {} ms",
            std::chrono::duration_cast<std::chrono::milliseconds>(TSceneStart - TParseStart).count());

        // The engine is left-handed.
        const ofbx::GlobalSettings* GlobalSettings = FBXScene->getGlobalSettings();
        const bool bConvertHandedness = (GlobalSettings == nullptr) || (GlobalSettings->CoordAxis == ofbx::CoordSystem_RightHanded);

        if (Progress)
        {
            Progress->EnterProgressFrame(0.40f, "Reading animations...");
        }

        float SceneScale = FBXScene->getGlobalSettings()->UnitScaleFactor * 0.01f;
        SceneScale *= ImportOptions.Scale;
        
        DEFER
        {
            FBXScene->destroy();
        };
        
        FMeshImportData ImportData;
        FStringView FileName = VFS::FileName(FilePath, true);
        
        
        int AnimStackCount = ImportOptions.bImportAnimations ? FBXScene->getAnimationStackCount() : 0;
        for (int i = 0; i < AnimStackCount; ++i)
        {
            const ofbx::AnimationStack* AnimationStack = FBXScene->getAnimationStack(i);
            auto AnimClip = MakeUnique<FAnimationResource>();
            AnimClip->Name = FString(AnimationStack->name) + "_Animation";
            
            const ofbx::AnimationLayer* Layer = AnimationStack->getLayer(0);
            if (Layer == nullptr)
            {
                continue;
            }
        
            if (const ofbx::TakeInfo* TakeInfo = FBXScene->getTakeInfo(AnimationStack->name))
            {
                AnimClip->Duration = static_cast<float>(TakeInfo->local_time_to - TakeInfo->local_time_from);
            }
            
            int ObjectCount = FBXScene->getAllObjectCount();
            for (int ObjIdx = 0; ObjIdx < ObjectCount; ++ObjIdx)
            {
                const ofbx::Object* Bone = FBXScene->getAllObjects()[ObjIdx];
                
                if (!Bone->isNode())
                {
                    continue;
                }
                
                const ofbx::AnimationCurveNode* TranslationNode = Layer->getCurveNode(*Bone, "Lcl Translation");
                const ofbx::AnimationCurveNode* RotationNode    = Layer->getCurveNode(*Bone, "Lcl Rotation");
                const ofbx::AnimationCurveNode* ScaleNode       = Layer->getCurveNode(*Bone, "Lcl Scale");
                
                if (!TranslationNode && !RotationNode && !ScaleNode)
                {
                    continue;
                }
        
                TSet<int64> AllTimestamps;
                
                auto CollectTimestamps = [&](const ofbx::AnimationCurveNode* Node)
                {
                    if (!Node)
                    {
                        return;
                    }
                    for (int j = 0; j < 3; ++j)
                    {
                        if (const ofbx::AnimationCurve* Curve = Node->getCurve(j))
                        {
                            int KeyCount = Curve->getKeyCount();
                            const int64* KeyTimes = Curve->getKeyTime();
                            for (int k = 0; k < KeyCount; ++k)
                            {
                                AllTimestamps.insert(KeyTimes[k]);
                            }
                        }
                    }
                };
                
                CollectTimestamps(TranslationNode);
                CollectTimestamps(RotationNode);
                CollectTimestamps(ScaleNode);
                
                if (AllTimestamps.empty())
                {
                    continue;
                }
                
                TVector<int64> SortedTimestamps(AllTimestamps.begin(), AllTimestamps.end());
                eastl::sort(SortedTimestamps.begin(), SortedTimestamps.end());
                
                struct KeyframeData
                {
                    float Time;
                    FVector3 Translation;
                    FQuat Rotation;
                    FVector3 Scale;
                };
                
                TVector<KeyframeData> Keyframes;
                Keyframes.reserve(SortedTimestamps.size());
                
                for (int64 FBXTime : SortedTimestamps)
                {
                    float Time = static_cast<float>(ofbx::fbxTimeToSeconds(FBXTime));

                    ofbx::DVec3 AnimTranslation = Bone->getLocalTranslation();
                    ofbx::DVec3 AnimRotation = Bone->getLocalRotation();
                    ofbx::DVec3 AnimScale = Bone->getLocalScaling();

                    // getNodeLocalTransform falls back to the curve node's authored per-axis default
                    // (d|X/Y/Z) when an axis has no curve. Exporters routinely omit unanimated axes;
                    // evaluating curves directly with a 0 default snapped those axes to zero (bones
                    // scattered to axis planes, scale-0 collapses).
                    if (TranslationNode)
                    {
                        AnimTranslation = TranslationNode->getNodeLocalTransform(Time);
                    }

                    if (RotationNode)
                    {
                        AnimRotation = RotationNode->getNodeLocalTransform(Time);
                    }

                    if (ScaleNode)
                    {
                        AnimScale = ScaleNode->getNodeLocalTransform(Time);
                    }
                    
                    ofbx::DMatrix LocalMatrix = Bone->evalLocal(AnimTranslation, AnimRotation, AnimScale);
                    FMatrix4 Mat = ConvertMatrix(LocalMatrix);

                    if (bConvertHandedness)
                    {
                        ConjugateHandedness(Mat);
                    }

                    Mat[3][0] *= SceneScale;
                    Mat[3][1] *= SceneScale;
                    Mat[3][2] *= SceneScale;
                    
                    KeyframeData Keyframe;
                    Keyframe.Time = Time;
                    // Math::Decompose normalizes the basis before the quat extraction; ToQuat on the raw
                    // matrix breaks whenever the rig carries non-unit bone scale (baked unit conversion,
                    // squash-and-stretch).
                    if (!Math::Decompose(Mat, Keyframe.Scale, Keyframe.Rotation, Keyframe.Translation))
                    {
                        Keyframe.Translation = FVector3(Mat[3][0], Mat[3][1], Mat[3][2]);
                        Keyframe.Rotation    = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
                        Keyframe.Scale       = FVector3(1.0f);
                    }
                    
                    Keyframes.push_back(Keyframe);
                }
                
                if (TranslationNode)
                {
                    FAnimationChannel TranslationChannel;
                    TranslationChannel.TargetBone = Bone->name;
                    TranslationChannel.TargetPath = FAnimationChannel::ETargetPath::Translation;
                    
                    for (const KeyframeData& KF : Keyframes)
                    {
                        TranslationChannel.Timestamps.push_back(KF.Time);
                        TranslationChannel.Translations.push_back(KF.Translation);
                    }
                    
                    AnimClip->Channels.push_back(TranslationChannel);
                }
                
                if (RotationNode)
                {
                    FAnimationChannel RotationChannel;
                    RotationChannel.TargetBone = Bone->name;
                    RotationChannel.TargetPath = FAnimationChannel::ETargetPath::Rotation;
                    
                    for (const KeyframeData& KF : Keyframes)
                    {
                        RotationChannel.Timestamps.push_back(KF.Time);
                        RotationChannel.Rotations.push_back(KF.Rotation);
                    }
                    
                    AnimClip->Channels.push_back(RotationChannel);
                }
                
                if (ScaleNode)
                {
                    FAnimationChannel ScaleChannel;
                    ScaleChannel.TargetBone = Bone->name;
                    ScaleChannel.TargetPath = FAnimationChannel::ETargetPath::Scale;
                    
                    for (const KeyframeData& KF : Keyframes)
                    {
                        ScaleChannel.Timestamps.push_back(KF.Time);
                        ScaleChannel.Scales.push_back(KF.Scale);
                    }
                    
                    AnimClip->Channels.push_back(ScaleChannel);
                }
            }
            
            if (!AnimClip->Channels.empty())
            {
                ImportData.Animations.push_back(Move(AnimClip));
            }
        }

        if (Progress)
        {
            Progress->UpdateMessage("Reading skeleton & materials...");
        }

        // FBX stores embedded textures as scene-level Video objects.
        auto DecodeBase64 = [](const char* Data, size_t Len) -> TVector<uint8>
        {
            auto V = [](char C) -> int
            {
                if (C >= 'A' && C <= 'Z') { return C - 'A'; }
                if (C >= 'a' && C <= 'z') { return C - 'a' + 26; }
                if (C >= '0' && C <= '9') { return C - '0' + 52; }
                if (C == '+') { return 62; }
                if (C == '/') { return 63; }
                return -1;
            };
            TVector<uint8> Out;
            Out.reserve(Len * 3 / 4 + 3);
            int Buf = 0, Bits = 0;
            for (size_t i = 0; i < Len; ++i)
            {
                const int D = V(Data[i]);
                if (D < 0) { continue; }
                Buf = (Buf << 6) | D;
                Bits += 6;
                if (Bits >= 8)
                {
                    Bits -= 8;
                    Out.push_back((uint8)((Buf >> Bits) & 0xFF));
                }
            }
            return Out;
        };

        auto EmbeddedLeafKey = [](ofbx::DataView View) -> FString
        {
            FString S;
            if (View.begin != nullptr && View.end > View.begin)
            {
                S.assign(reinterpret_cast<const char*>(View.begin), reinterpret_cast<const char*>(View.end));
            }
            for (FString::value_type& C : S) { if (C == '\\') { C = '/'; } }
            FStringView Leaf = VFS::FileName(S, true);
            FString Key(Leaf.data(), Leaf.length());
            for (FString::value_type& C : Key) { if (C >= 'A' && C <= 'Z') { C = C - 'A' + 'a'; } }
            return Key;
        };

        THashMap<FString, TVector<uint8>> EmbeddedByName;
        if (ImportOptions.bImportTextures || ImportOptions.bImportMaterials)
        {
            const int DataCount = FBXScene->getEmbeddedDataCount();
            for (int DataIdx = 0; DataIdx < DataCount; ++DataIdx)
            {
                FString Key = EmbeddedLeafKey(FBXScene->getEmbeddedFilename(DataIdx));
                if (Key.empty())
                {
                    continue;
                }

                TVector<uint8> Bytes;
                const ofbx::DataView Content = FBXScene->getEmbeddedData(DataIdx);
                if (Content.begin != nullptr && Content.end > Content.begin)
                {
                    Bytes.assign(reinterpret_cast<const uint8*>(Content.begin), reinterpret_cast<const uint8*>(Content.end));
                }
                else if (FBXScene->isEmbeddedBase64(DataIdx))
                {
                    if (const ofbx::IElementProperty* B64 = FBXScene->getEmbeddedBase64Data(DataIdx))
                    {
                        const ofbx::DataView Enc = B64->getValue();
                        if (Enc.begin != nullptr && Enc.end > Enc.begin)
                        {
                            Bytes = DecodeBase64(reinterpret_cast<const char*>(Enc.begin), (size_t)(Enc.end - Enc.begin));
                        }
                    }
                }

                if (!Bytes.empty())
                {
                    EmbeddedByName.emplace(Move(Key), Move(Bytes));
                }
            }

            LOG_INFO("[FBX] Embedded textures: {} video(s) in scene, {} with usable bytes.", DataCount, EmbeddedByName.size());
        }
        
        
        
        THashMap<FName, int32> BoneNameToIndex;
        TVector<const ofbx::Object*> BoneObjects;

        int MeshCount = FBXScene->getMeshCount();

        THashMap<const ofbx::Material*, int16> MaterialToGlobal;
        TVector<TVector<int16>> MeshPartitionToGlobal(MeshCount);

        if (ImportOptions.bImportMaterials)
        {
            auto ViewToString = [](const ofbx::DataView& V) -> FString
            {
                if (V.begin == nullptr || V.end <= V.begin)
                {
                    return FString();
                }
                FString S(reinterpret_cast<const char*>(V.begin), reinterpret_cast<const char*>(V.end));
                for (FString::value_type& C : S)
                {
                    if (C == '\\') { C = '/'; }
                }
                return S;
            };

            // Resolves an ofbx texture into an FMeshImportImage (embedded bytes or relative path), emits it into
            // the texture set (when textures are imported), and returns the RelativePath key the slot links to.
            auto ExtractTexture = [&](const ofbx::Material* Material, ofbx::Texture::TextureType Type,
                                      ETextureColorSpace Role, int16 GlobalIdx, const char* Suffix) -> FFixedString
            {
                const ofbx::Texture* Tex = Material->getTexture(Type);
                if (Tex == nullptr)
                {
                    return {};
                }

                const FString Rel = ViewToString(Tex->getRelativeFileName());
                const ofbx::DataView Embedded = Tex->getEmbeddedData();

                FMeshImportImage Image;
                Image.IntendedColorSpace = Role;

                // Filename leaf, computed once: the embedded key and the Video-table lookup both use it.
                FFixedString Leaf;
                if (!Rel.empty())
                {
                    FStringView L = VFS::FileName(Rel, true);
                    Leaf.append_convert(L.data(), L.length());
                }

                if (Embedded.begin != nullptr && Embedded.end > Embedded.begin)
                {
                    const uint8* Start = reinterpret_cast<const uint8*>(Embedded.begin);
                    Image.Bytes.assign(Start, reinterpret_cast<const uint8*>(Embedded.end));
                    Image.RelativePath = !Leaf.empty()
                        ? Leaf
                        : FFixedString(FFixedString::CtorSprintf(), "FBXMat%d_%s", (int)GlobalIdx, Suffix);
                }
                else if (!Leaf.empty())
                {
                    // getEmbeddedData() missed it (no Media link); recover from the scene Video table by leaf
                    // before giving up to an external path that, for an embedded texture, won't exist on disk.
                    FString LeafLower(Leaf.c_str());
                    for (FString::value_type& C : LeafLower) { if (C >= 'A' && C <= 'Z') { C = C - 'A' + 'a'; } }

                    auto It = EmbeddedByName.find(LeafLower);
                    if (It != EmbeddedByName.end() && !It->second.empty())
                    {
                        Image.Bytes.assign(It->second.begin(), It->second.end());
                        Image.RelativePath = Leaf;
                    }
                    else
                    {
                        Image.RelativePath = Rel.c_str();   // external file (Rel is non-empty here)
                    }
                }
                else
                {
                    // No embedded bytes and no path at all.
                    return {};
                }

                FFixedString Key = Image.RelativePath;
                if (ImportOptions.bImportTextures)
                {
                    ImportData.Textures.emplace(Move(Image));
                }
                return Key;
            };

            // FBX has no masked/alpha-test flag and OpenFBX exposes no opacity texture, so alpha-tested foliage
            // would import Opaque (the cutout lives in the base-color alpha). Heuristically flag foliage-like
            // materials as Masked by name; the user can override the blend mode per-material in the editor.
            auto IsFoliageName = [](const FString& Name, const FFixedString& TexKey) -> bool
            {
                static const char* const kKeywords[] = {
                    "leaf", "leaves", "foliage", "tree", "plant", "ivy", "bush",
                    "grass", "branch", "fern", "hedge", "vine", "shrub", "flower" };

                auto Lower = [](const char* S) -> FString
                {
                    FString L(S);
                    for (FString::value_type& C : L) { if (C >= 'A' && C <= 'Z') { C = C - 'A' + 'a'; } }
                    return L;
                };
                const FString N = Lower(Name.c_str());
                const FString T = Lower(TexKey.c_str());
                for (const char* K : kKeywords)
                {
                    if (N.find(K) != FString::npos || T.find(K) != FString::npos)
                    {
                        return true;
                    }
                }
                return false;
            };

            for (int MeshIdx = 0; MeshIdx < MeshCount; ++MeshIdx)
            {
                const ofbx::Mesh* Mesh = FBXScene->getMesh(MeshIdx);
                const int MaterialCount = Mesh->getMaterialCount();
                MeshPartitionToGlobal[MeshIdx].resize(MaterialCount, 0);

                for (int MatIdx = 0; MatIdx < MaterialCount; ++MatIdx)
                {
                    const ofbx::Material* Material = Mesh->getMaterial(MatIdx);

                    int16 GlobalIdx;
                    auto It = MaterialToGlobal.find(Material);
                    if (It != MaterialToGlobal.end())
                    {
                        GlobalIdx = It->second;
                    }
                    else
                    {
                        GlobalIdx = (int16)ImportData.Materials.size();
                        MaterialToGlobal.emplace(Material, GlobalIdx);

                        FMeshImportMaterial Out;
                        Out.Name = (Material->name[0] != '\0')
                            ? FString(Material->name)
                            : FString(FFixedString(FFixedString::CtorSprintf(), "%.*s_Mat%d", (int)FileName.length(), FileName.data(), (int)GlobalIdx).c_str());

                        const ofbx::Color Diffuse  = Material->getDiffuseColor();
                        const ofbx::Color Emissive = Material->getEmissiveColor();
                        const float       Opacity  = (float)Material->getOpacity();
                        const float       EmissiveF = (float)Material->getEmissiveFactor();

                        Out.BaseColorFactor = FVector4(Diffuse.r, Diffuse.g, Diffuse.b, Opacity);
                        Out.MetallicFactor  = 0.0f;
                        Out.RoughnessFactor = Math::Sqrt(2.0f / (Math::Max((float)Material->getShininessExponent(), 0.0f) + 2.0f));
                        Out.AlphaMode       = (Opacity < 0.999f) ? EImportAlphaMode::Blend : EImportAlphaMode::Opaque;

                        Out.BaseColorTexture = ExtractTexture(Material, ofbx::Texture::DIFFUSE,  ETextureColorSpace::SRGB,      GlobalIdx, "D");
                        // Linear (BC7 RGB), not NormalMap: the BC5-packed normal path is currently broken.
                        Out.NormalTexture    = ExtractTexture(Material, ofbx::Texture::NORMAL,   ETextureColorSpace::Linear, GlobalIdx, "N");
                        Out.EmissiveTexture  = ExtractTexture(Material, ofbx::Texture::EMISSIVE, ETextureColorSpace::SRGB,      GlobalIdx, "E");

                        // OpenFBX returns emissive_color=(1,1,1), factor=1 for materials that DON'T author emissive
                        // (those are its struct defaults). Combined with the white default emissive texture, that makes
                        // every non-emissive material glow full white (emissive is additive + unlit). Treat the exact
                        // unauthored default (with no emissive map) as "not emissive"; any authored value/map is kept.
                        const bool bDefaultEmissive = (Emissive.r == 1.0f && Emissive.g == 1.0f && Emissive.b == 1.0f && EmissiveF == 1.0f);
                        Out.EmissiveColor = (Out.EmissiveTexture.empty() && bDefaultEmissive)
                            ? FVector3(0.0f)
                            : FVector3(Emissive.r * EmissiveF, Emissive.g * EmissiveF, Emissive.b * EmissiveF);

                        // Foliage heuristic: promote opaque materials with a foliage-like name to alpha-test
                        // (base-color alpha drives the clip). Already-translucent materials keep their blend.
                        if (Out.AlphaMode == EImportAlphaMode::Opaque && IsFoliageName(Out.Name, Out.BaseColorTexture))
                        {
                            Out.AlphaMode   = EImportAlphaMode::Mask;
                            Out.AlphaCutoff = 0.5f;
                        }

                        ImportData.Materials.push_back(Move(Out));
                    }

                    MeshPartitionToGlobal[MeshIdx][MatIdx] = GlobalIdx;
                }
            }
        }
        
        for (int MeshIdx = 0; MeshIdx < MeshCount; ++MeshIdx)
        {
            const ofbx::Mesh* Mesh = FBXScene->getMesh(MeshIdx);
            const ofbx::Skin* Skin = Mesh->getSkin();

            if (!Skin)
            {
                continue;
            }

            int ClusterCount = Skin->getClusterCount();
            for (int ClusterIdx = 0; ClusterIdx < ClusterCount; ++ClusterIdx)
            {
                const ofbx::Cluster* Cluster = Skin->getCluster(ClusterIdx);
                const ofbx::Object* JointNode = Cluster->getLink();
                
                if (BoneNameToIndex.find(JointNode->name) == BoneNameToIndex.end())
                {
                    int32 BoneIndex = static_cast<int32>(BoneObjects.size());
                    BoneObjects.push_back(JointNode);
                    BoneNameToIndex[JointNode->name] = BoneIndex;
                }
            }
        }
        
        if (!BoneObjects.empty() && ImportOptions.bImportSkeleton)
        {
            auto Skeleton = MakeUnique<FSkeletonResource>();
            Skeleton->Bones.resize(BoneObjects.size());
            Skeleton->Name = FString(FileName) + "_Skeleton";
            
            for (int MeshIdx = 0; MeshIdx < MeshCount; ++MeshIdx)
            {
                const ofbx::Mesh* Mesh = FBXScene->getMesh(MeshIdx);
                const ofbx::Skin* Skin = Mesh->getSkin();
    
                if (!Skin)
                {
                    continue;
                }
    
                int ClusterCount = Skin->getClusterCount();
                for (int ClusterIdx = 0; ClusterIdx < ClusterCount; ++ClusterIdx)
                {
                    const ofbx::Cluster* Cluster = Skin->getCluster(ClusterIdx);
                    const ofbx::Object* JointNode = Cluster->getLink();
        
                    if (!JointNode)
                    {
                        continue;
                    }
        
                    auto It = BoneNameToIndex.find(JointNode->name);
                    if (It == BoneNameToIndex.end())
                    {
                        continue;
                    }
        
                    int32 BoneIndex = It->second;
                    
                    ofbx::DMatrix TransformLinkMatrix = Cluster->getTransformLinkMatrix();
                    FMatrix4 JointWorldTransform = ConvertMatrix(TransformLinkMatrix);

                    if (bConvertHandedness)
                    {
                        ConjugateHandedness(JointWorldTransform);
                    }

                    JointWorldTransform[3][0] *= SceneScale;
                    JointWorldTransform[3][1] *= SceneScale;
                    JointWorldTransform[3][2] *= SceneScale;
                    
                    Skeleton->Bones[BoneIndex].InvBindMatrix = Math::Inverse(JointWorldTransform);
                }
            }
        
            
            for (int32 BoneIndex = 0; BoneIndex < static_cast<int32>(BoneObjects.size()); ++BoneIndex)
            {
                const ofbx::Object* BoneObj = BoneObjects[BoneIndex];
    
                FSkeletonResource::FBoneInfo& BoneInfo = Skeleton->Bones[BoneIndex];
                BoneInfo.Name = BoneObj->name;
    
                const ofbx::Object* ParentObj = BoneObj->getParent();
                BoneInfo.ParentIndex = INDEX_NONE;
    
                if (ParentObj)
                {
                    auto It = BoneNameToIndex.find(ParentObj->name);
                    if (It != BoneNameToIndex.end())
                    {
                        BoneInfo.ParentIndex = It->second;
                    }
                }
    
                ofbx::DMatrix LocalMatrix = BoneObj->getLocalTransform();
                FMatrix4 LocalTransform = ConvertMatrix(LocalMatrix);

                if (bConvertHandedness)
                {
                    ConjugateHandedness(LocalTransform);
                }

                LocalTransform[3][0] *= SceneScale;
                LocalTransform[3][1] *= SceneScale;
                LocalTransform[3][2] *= SceneScale;
                
                BoneInfo.LocalTransform = LocalTransform;
            }
            
            Skeleton->BoneNameToIndex = BoneNameToIndex;
            Skeleton->BuildBindPoseCache();
            ImportData.Skeletons.push_back(Move(Skeleton));
        }
        
        
        ImportData.Resources.reserve(MeshCount);

        LOG_INFO("[FBX] Scene data (anims/skeleton/materials): {} ms",
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - TSceneStart).count());

        if (Progress)
        {
            Progress->EnterProgressFrame(0.10f, "Reading geometry...");
        }

        // Per-mesh extraction in parallel into FFBXMeshResult; serial merge concatenates with rebased offsets.
        struct FFBXMeshResult
        {
            bool                        bSkinned = false;
            TVector<FVertex>            StaticVerts;
            TVector<FSkinnedVertex>     SkinnedVerts;
            TVector<uint32>             Indices;
            TVector<FGeometrySurface>   Surfaces;
        };

        TVector<FFBXMeshResult> Results(MeshCount);

        // The geometry phase owns the final 0.5 of the parse budget, spread per mesh.
        const float GeometryStep = 0.5f / (float)eastl::max<size_t>((size_t)1, (size_t)MeshCount);
        TAtomic<uint32> CompletedMeshes{0};

        Task::ParallelFor((uint32)MeshCount, [&](uint32 MeshIdx)
        {
            const ofbx::Mesh* Mesh = FBXScene->getMesh((int)MeshIdx);
            const ofbx::Skin* OFBXSkin = Mesh->getSkin();
            const bool bSkinned = OFBXSkin != nullptr;

            FFBXMeshResult& Result = Results[MeshIdx];
            Result.bSkinned = bSkinned;

            THashMap<int, uint32> VertexIndexMap;

            const ofbx::GeometryData&    Geometry  = Mesh->getGeometryData();
            const ofbx::Vec3Attributes   Positions = Geometry.getPositions();
            const ofbx::Vec3Attributes   Normals   = Geometry.getNormals();
            const ofbx::Vec2Attributes   UVs       = Geometry.getUVs();
            const ofbx::Vec4Attributes   Colors    = Geometry.getColors();

            // Bake FBX transform: WorldVertex = GlobalTransform * GeometricMatrix * LocalVertex.
            // Per-mesh GeometricMatrix is FBX-specific; ignoring it leaves meshes inconsistently oriented.
            const FMatrix4 GlobalMatrix    = ConvertMatrix(Mesh->getGlobalTransform());
            const FMatrix4 GeometricMatrix = ConvertMatrix(Mesh->getGeometricMatrix());
            const FMatrix4 MeshToWorld     = GlobalMatrix * GeometricMatrix;
            // RH(FBX) -> LH(engine): reflect X. Applied in world space so it converts the whole baked scene; the
            // reflection inverts winding (compensated below) and the normal basis (NormalMatrix uses it too).
            const FMatrix4 HandednessFix   = bConvertHandedness ? Math::Scale(FMatrix4(1.0f), FVector3(-1.0f, 1.0f, 1.0f)) : FMatrix4(1.0f);
            const FMatrix4 MeshToEngine    = HandednessFix * MeshToWorld;
            const FMatrix4 PosMatrix       = Math::Scale(FMatrix4(1.0f), FVector3(SceneScale)) * MeshToEngine;
            const FMatrix3 NormalMatrix    = Math::Transpose(Math::Inverse(FMatrix3(MeshToEngine)));

            // A negative-determinant (mirrored / negative-scale) node reflects the baked geometry, which flips
            // triangle winding. The engine is CCW-front + back-face-cull, so the correct side would be culled and
            // the reversed side shown (mirrored textures, e.g. backwards menu boards). Reverse the winding below.
            const bool     bMirroredXform = Math::Determinant(FMatrix3(MeshToEngine)) < 0.0f;

            const int CPCount = Mesh->getGeometry()->getGeometryData().getPositions().count;

            struct FSkin
            {
                int32 Count = 0;
                TArray<int32, 4> Joints {};
                TArray<float, 4> Weights {};
            };

            TVector<FSkin> Skins(CPCount);
            if (bSkinned)
            {
                const int ClusterCount = OFBXSkin->getClusterCount();

                for (int ClusterIdx = 0; ClusterIdx < ClusterCount; ++ClusterIdx)
                {
                    const ofbx::Cluster* Cluster = OFBXSkin->getCluster(ClusterIdx);

                    if (Cluster->getIndicesCount() == 0)
                    {
                        continue;
                    }

                    if (Cluster->getLink() == nullptr)
                    {
                        continue;
                    }

                    auto BoneIt = BoneNameToIndex.find(Cluster->getLink()->name);
                    if (BoneIt == BoneNameToIndex.end())
                    {
                        continue;
                    }
                    int32 BoneIndex = BoneIt->second;

                    const int*    Indices = Cluster->getIndices();
                    const double* Weights = Cluster->getWeights();
                    int           Count   = Cluster->getIndicesCount();

                    for (int i = 0; i < Count; ++i)
                    {
                        int CPIndex = Indices[i];
                        float Weight = static_cast<float>(Weights[i]);
                        FSkin& Skin = Skins[CPIndex];
                        if (Skin.Count < 4)
                        {
                            Skin.Weights[Skin.Count] = Weight;
                            Skin.Joints[Skin.Count] = BoneIndex;
                            ++Skin.Count;
                        }
                        else
                        {
                            int Min = 0;
                            for (int M = 1; M < 4; ++M)
                            {
                                if (Skin.Weights[M] < Skin.Weights[Min])
                                {
                                    Min = M;
                                }

                                if (Skin.Weights[Min] < Weight)
                                {
                                    Skin.Weights[Min] = Weight;
                                    Skin.Joints[Min]  = BoneIndex;
                                }
                            }
                        }
                    }
                }

                for (FSkin& Skin : Skins)
                {
                    float Sum = 0;
                    for (float Weight : Skin.Weights)
                    {
                        Sum += Weight;
                    }

                    if (Sum == 0.0f)
                    {
                        Skin.Weights[0] = 1.0f;
                        Skin.Weights[1] = Skin.Weights[2] = Skin.Weights[3] = 0.0f;
                        Skin.Joints[0] = Skin.Joints[1] = Skin.Joints[2] = Skin.Joints[3] = 0;
                    }
                    else
                    {
                        for (float& Weight : Skin.Weights)
                        {
                            Weight /= Sum;
                        }
                    }
                }
            }

            for (int PartitionIdx = 0; PartitionIdx < Geometry.getPartitionCount(); ++PartitionIdx)
            {
                uint32 StartIndex = (uint32)Result.Indices.size();

                const ofbx::GeometryPartition& Partition = Geometry.getPartition(PartitionIdx);

                for (int PolygonIdx = 0; PolygonIdx < Partition.polygon_count; ++PolygonIdx)
                {
                    const ofbx::GeometryPartition::Polygon& Polygon = Partition.polygons[PolygonIdx];

                    int TriangleIndices[128];
                    uint32 TriIndexCount = ofbx::triangulate(Geometry, Polygon, TriangleIndices);

                    if (bMirroredXform)
                    {
                        for (uint32 t = 0; t + 2 < TriIndexCount; t += 3)
                        {
                            const int Tmp          = TriangleIndices[t + 1];
                            TriangleIndices[t + 1] = TriangleIndices[t + 2];
                            TriangleIndices[t + 2] = Tmp;
                        }
                    }

                    for (uint32 i = 0; i < TriIndexCount; ++i)
                    {
                        int Index = TriangleIndices[i];

                        uint32 VertexIdx;
                        auto it = VertexIndexMap.find(Index);
                        if (it != VertexIndexMap.end())
                        {
                            VertexIdx = it->second;
                        }
                        else
                        {
                            ofbx::Vec3 Position = Positions.get(Index);
                            FVector3 PosLocal(Position.x, Position.y, Position.z);
                            FVector3 Pos = FVector3(PosMatrix * FVector4(PosLocal, 1.0f));

                            FVector3 Normal = Math::Normalize(NormalMatrix * FVector3(0.0f, 1.0f, 0.0f));
                            if (Normals.values)
                            {
                                ofbx::Vec3 N = Normals.get(Index);
                                Normal = Math::Normalize(NormalMatrix * FVector3(N.x, N.y, N.z));
                            }

                            FVector2 UV(0, 0);
                            if (UVs.values)
                            {
                                ofbx::Vec2 U = UVs.get(Index);

                                // FBX authoring uses the OpenGL bottom-left UV origin; the engine renders with the
                                // Vulkan top-left origin, so convert here (otherwise textures import upside-down).
                                // The bFlipUVs option (applied again in FinalizeMeshImportData) then toggles this
                                // back off for the rare FBX that was authored top-left. glTF is already top-left.
                                U.y = 1.0f - U.y;

                                UV = FVector2(U.x, U.y);
                            }

                            FVector4 Col(1.0f);
                            if (Colors.values)
                            {
                                ofbx::Vec4 Color = Colors.get(Index);
                                Col = FVector4(Color.x, Color.y, Color.z, Color.w);
                            }

                            if (bSkinned)
                            {
                                FSkinnedVertex Vertex;
                                Vertex.Position = Pos;
                                Vertex.Normal   = PackNormal(Normal);
                                Vertex.Tangent  = 0;
                                Vertex.UV       = Math::PackHalf2x16(UV);
                                Vertex.Color    = PackColor(Col);

                                FU8Vector4 JointIndices{};
                                FVector4   JointWeights{};

                                if (Positions.indices)
                                {
                                    const FSkin& Skin = Skins[Positions.indices[Index]];
                                    for (int j = 0; j < Skin.Count; ++j)
                                    {
                                        uint8 BoneIndex = (uint8)Skin.Joints[j];
                                        float Weight    = Skin.Weights[j];

                                        JointIndices[j] = BoneIndex;
                                        JointWeights[j] = Weight;
                                    }
                                }

                                Vertex.JointIndices = JointIndices;
                                Vertex.JointWeights = FU8Vector4(JointWeights * 255.0f);

                                VertexIdx = (uint32)Result.SkinnedVerts.size();
                                Result.SkinnedVerts.push_back(Vertex);
                            }
                            else
                            {
                                FVertex Vertex;
                                Vertex.Position = Pos;
                                Vertex.Normal   = PackNormal(Normal);
                                Vertex.Tangent  = 0;
                                Vertex.UV       = Math::PackHalf2x16(UV);
                                Vertex.Color    = PackColor(Col);

                                VertexIdx = (uint32)Result.StaticVerts.size();
                                Result.StaticVerts.push_back(Vertex);
                            }

                            VertexIndexMap[Index] = VertexIdx;
                        }

                        Result.Indices.push_back(VertexIdx);
                    }
                }

                FGeometrySurface Surface;
                Surface.ID = Mesh->name;
                Surface.StartIndex = StartIndex;
                Surface.IndexCount = (uint32)Result.Indices.size() - StartIndex;
                // Remap the per-mesh partition index to its global material slot (identity fallback when
                // materials weren't extracted, preserving the original partition-index behavior).
                const TVector<int16>& P2G = MeshPartitionToGlobal[MeshIdx];
                Surface.MaterialIndex = (PartitionIdx < (int)P2G.size()) ? P2G[PartitionIdx] : (int16)PartitionIdx;
                Result.Surfaces.push_back(Surface);
            }

            if (Progress)
            {
                const uint32 Done = CompletedMeshes.fetch_add(1) + 1;
                FFixedString Msg(FFixedString::CtorSprintf(), "Reading geometry (%u/%u meshes)...", Done, (uint32)MeshCount);
                Progress->EnterProgressFrame(GeometryStep, Msg);
            }
        });

        // Serial merge: indices and surface StartIndex rebased by running bases, source-order traversal stays deterministic.
        TUniquePtr<FMeshResource> StaticMesh = MakeUnique<FMeshResource>();
        StaticMesh->Name = FString(FileName) + "_Mesh";

        TUniquePtr<FMeshResource> SkinnedMesh = MakeUnique<FMeshResource>();
        SkinnedMesh->bSkinnedMesh = true;
        SkinnedMesh->Name = FString(FileName) + "_SkeletalMesh";

        {
            size_t StaticVertCount = 0, StaticIdxCount = 0, StaticSurfCount = 0;
            size_t SkinnedVertCount = 0, SkinnedIdxCount = 0, SkinnedSurfCount = 0;
            for (const FFBXMeshResult& R : Results)
            {
                if (R.bSkinned)
                {
                    SkinnedVertCount += R.SkinnedVerts.size();
                    SkinnedIdxCount  += R.Indices.size();
                    SkinnedSurfCount += R.Surfaces.size();
                }
                else
                {
                    StaticVertCount += R.StaticVerts.size();
                    StaticIdxCount  += R.Indices.size();
                    StaticSurfCount += R.Surfaces.size();
                }
            }

            StaticMesh->ReserveVertices(StaticVertCount);
            StaticMesh->Indices.reserve(StaticIdxCount);
            StaticMesh->GeometrySurfaces.reserve(StaticSurfCount);

            SkinnedMesh->ReserveVertices(SkinnedVertCount);
            SkinnedMesh->Indices.reserve(SkinnedIdxCount);
            SkinnedMesh->GeometrySurfaces.reserve(SkinnedSurfCount);
        }

        for (FFBXMeshResult& R : Results)
        {
            FMeshResource& Target = R.bSkinned ? *SkinnedMesh : *StaticMesh;

            const uint32 BaseVert = (uint32)Target.GetNumVertices();
            const uint32 BaseIdx  = (uint32)Target.Indices.size();

            if (R.bSkinned)
            {
                for (const FSkinnedVertex& V : R.SkinnedVerts)
                {
                    Target.AppendVertex(V);
                }
            }
            else
            {
                for (const FVertex& V : R.StaticVerts)
                {
                    Target.AppendVertex(V);
                }
            }

            for (uint32 LocalIdx : R.Indices)
            {
                Target.Indices.push_back(LocalIdx + BaseVert);
            }

            for (FGeometrySurface S : R.Surfaces)
            {
                S.StartIndex += BaseIdx;
                Target.GeometrySurfaces.push_back(S);
            }
        }

        TVector<FMeshResource*> ToFinalize;
        if (StaticMesh->GetNumVertices() > 0)
        {
            ToFinalize.push_back(StaticMesh.get());
        }
        if (SkinnedMesh->GetNumVertices() > 0)
        {
            ToFinalize.push_back(SkinnedMesh.get());
        }

        // When bSkipFinalization is set, the dialog runs heavy passes at commit; stats still built for preview.
        if (!ImportOptions.bSkipFinalization)
        {
            Task::ParallelFor((uint32)ToFinalize.size(), [&](uint32 i)
            {
                FMeshResource* Res = ToFinalize[i];
                if (ImportOptions.bOptimize)
                {
                    OptimizeNewlyImportedMesh(*Res);
                }
                GenerateMeshlets(*Res);
            });
        }

        for (FMeshResource* Res : ToFinalize)
        {
            AnalyzeMeshStatistics(*Res, ImportData.MeshStatistics);
        }

        if (StaticMesh->GetNumVertices() > 0)
        {
            ImportData.Resources.push_back(std::move(StaticMesh));
        }
        if (SkinnedMesh->GetNumVertices() > 0)
        {
            ImportData.Resources.push_back(std::move(SkinnedMesh));
        }

        return Move(ImportData);
    }
}
