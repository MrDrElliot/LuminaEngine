#include "pch.h"
#include "Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "FileSystem/FileSystem.h"
#include "Core/Math/Hash/Hash.h"
#include "Memory/MemoryTracking.h"
#include "Paths/Paths.h"
#include "EASTL/sort.h"
#include "Renderer/RenderManager.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "Types/Byte.h"

namespace Lumina
{
    static CMaterial* DefaultMaterial = nullptr;
    static CMaterial* DefaultTerrainMaterial = nullptr;

    namespace
    {
        // The one description of every compiled material shader stage: which serialized SPIR-V blob
        // feeds which library entry, plus the GUID-key suffix and pipeline type. PostLoad and the
        // editor graph compile both walk this, so adding a stage is a one-line change here (plus the
        // EMaterialShaderStage entry it's indexed by).
        struct FMaterialStageDesc
        {
            TVector<uint32> CMaterial::*     Binaries;
            const FShaderEntry* CMaterial::* Entry;
            const char*                      Suffix;
            ERHIShaderType                   Type;
        };

        // Indexed by EMaterialShaderStage.
        const FMaterialStageDesc GMaterialStages[] =
        {
            { &CMaterial::PixelShaderBinaries,                    &CMaterial::PixelShader,                    "_PS",    ERHIShaderType::Fragment },
            { &CMaterial::VertexShaderBinaries,                   &CMaterial::VertexShader,                   "_VS",    ERHIShaderType::Vertex   },
            { &CMaterial::MeshShaderBinaries,                     &CMaterial::MeshShader,                     "_MS",    ERHIShaderType::Mesh     },
            { &CMaterial::VisBufferMeshShaderBinaries,            &CMaterial::VisBufferMeshShader,            "_VBM",   ERHIShaderType::Mesh     },
            { &CMaterial::VisBufferVertexShaderBinaries,          &CMaterial::VisBufferVertexShader,          "_VBV",   ERHIShaderType::Vertex   },
            { &CMaterial::MaskedVisBufferPixelShaderBinaries,     &CMaterial::MaskedVisBufferPixelShader,     "_MVBP",  ERHIShaderType::Fragment },
            { &CMaterial::MaskedVisBufferPixelShaderPrimBinaries, &CMaterial::MaskedVisBufferPixelShaderPrim, "_MVBPP", ERHIShaderType::Fragment },
            { &CMaterial::DeferredShaderBinaries,                 &CMaterial::DeferredShader,                 "_DM",    ERHIShaderType::Fragment },
        };
        static_assert(sizeof(GMaterialStages) / sizeof(GMaterialStages[0]) == (size_t)EMaterialShaderStage::Count,
            "GMaterialStages must cover every EMaterialShaderStage");

#if USING(WITH_EDITOR)
        // Materials whose serialized stages predate the current shader templates. Filled during the
        // (parallel) PostLoad wave, drained on the editor's game thread -> mutex, TObjectPtr for safety
        // against a material dying before the drain reaches it.
        FMutex                        StaleTemplateMutex;
        TVector<TObjectPtr<CMaterial>> StaleTemplateMaterials;

        void QueueStaleTemplateMaterial(CMaterial* Material)
        {
            FScopeLock Lock(StaleTemplateMutex);
            StaleTemplateMaterials.push_back(Material);
        }
#endif
    }

    CMaterial::CMaterial()
    {
        MaterialType = EMaterialType::PBR;
        Memory::Memzero(&MaterialUniforms, sizeof(FMaterialUniforms));
    }

    void CMaterial::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Materials");

        CMaterialInterface::Serialize(Ar);
    }

    void CMaterial::PostCreateCDO()
    {
        if (DefaultMaterial == nullptr)
        {
            CreateDefaultMaterial();
        }
        if (DefaultTerrainMaterial == nullptr)
        {
            CreateDefaultTerrainMaterial();
        }
    }

    void CMaterial::RegisterInstance(CMaterialInstance* Instance)
    {
        if (Instance == nullptr)
        {
            return;
        }
        // Instances of one master PostLoad concurrently on worker fibers (the parallel leaf-first wave),
        // so this back-reference list is touched from many threads at once.
        FScopeLock Lock(InstancesMutex);
        for (CMaterialInstance* Existing : Instances)
        {
            if (Existing == Instance)
            {
                return;
            }
        }
        Instances.push_back(Instance);
    }

    void CMaterial::UnregisterInstance(CMaterialInstance* Instance)
    {
        if (Instance == nullptr)
        {
            return;
        }
        FScopeLock Lock(InstancesMutex);
        for (auto It = Instances.begin(); It != Instances.end(); ++It)
        {
            if (*It == Instance)
            {
                Instances.erase(It);
                return;
            }
        }
    }

    void CMaterial::NotifyInstancesParentChanged()
    {
        TVector<CMaterialInstance*> Snapshot;
        {
            FScopeLock Lock(InstancesMutex);
            Snapshot = Instances;
        }

        for (CMaterialInstance* Instance : Snapshot)
        {
            if (Instance == nullptr || Instance->Material.Get() != this)
            {
                continue;
            }

            Instance->RefreshFromParent();
        }
    }

    void CMaterial::UpdateMaterialUniforms()
    {
        if (MaterialIndex != -1)
        {
            GRenderManager->GetMaterialManager().UpdateMaterialUniforms(&MaterialUniforms, (uint32)MaterialIndex);
        }
    }

    void CMaterial::RebuildParameterLookup()
    {
        ParameterLookup.clear();
        ParameterLookup.reserve(Parameters.size());
        for (const FMaterialParameter& Param : Parameters)
        {
            ParameterLookup[Param.ParameterName] = Param;
        }
    }

    void CMaterial::PostLoad()
    {
        if (!PixelShaderBinaries.empty() && !VertexShaderBinaries.empty())
        {
            // Commit every serialized stage to the library (entries keyed by asset GUID + stage suffix:
            // stable across reloads, refreshed in place on recompile). The merged VS (MeshletVertex.slang)
            // serves base/depth/shadow via the EPass spec constant; mesh/VisBuffer/masked/deferred stages
            // are optional and simply absent (empty) when not compiled for this material.
            for (size_t i = 0; i < (size_t)EMaterialShaderStage::Count; ++i)
            {
                const TVector<uint32>& Binaries = this->*GMaterialStages[i].Binaries;
                if (!Binaries.empty())
                {
                    CommitShaderStage((EMaterialShaderStage)i, TSpan<const uint32>(Binaries.data(), Binaries.size()));
                }
            }

#if USING(WITH_EDITOR)
            // The stage binaries bake whatever shader templates they were compiled from. If the templates
            // changed since (hash mismatch, or a pre-hash legacy asset), queue an editor recompile from the
            // saved graph. The package check skips the procedural default materials -- they compile fresh
            // from source every run and have no graph to recompile.
            if (GetPackage() != nullptr && CompiledTemplateHash != GetShaderTemplateHash())
            {
                QueueStaleTemplateMaterial(this);
            }
#endif

            // FMaterialUniforms isn't serialized; replay defaults from Parameters so authored values survive load.
            for (const FMaterialParameter& Param : Parameters)
            {
                switch (Param.Type)
                {
                case EMaterialParameterType::Scalar:
                    if (Param.Index < MAX_SCALARS)
                    {
                        MaterialUniforms.Scalars[Param.Index] = Param.ScalarDefault;
                    }
                    break;
                case EMaterialParameterType::Vector:
                    if (Param.Index < MAX_VECTORS)
                    {
                        MaterialUniforms.Vectors[Param.Index] = Param.VectorDefault;
                    }
                    break;
                case EMaterialParameterType::Texture:
                    break;
                }
            }
            
            const uint32 NumTextures = (uint32)Math::Min<size_t>(Textures.size(), MAX_TEXTURES);
            for (uint32 i = 0; i < NumTextures; ++i)
            {
                const int32 ResourceID = Textures[i] ? Textures[i]->GetResourceID() : -1;
                MaterialUniforms.Textures[i] = (ResourceID >= 0) ? (uint32)ResourceID : 0u;
            }
            
            EMaterialGPUFlags GPUFlags = EMaterialGPUFlags::None;
            if (BlendMode == EBlendMode::Masked)
            {
                GPUFlags |= EMaterialGPUFlags::Masked;
            }
            if (BlendMode == EBlendMode::Translucent)
            {
                GPUFlags |= EMaterialGPUFlags::Translucent;
            }
            if (BlendMode == EBlendMode::Additive)
            {
                GPUFlags |= EMaterialGPUFlags::Additive;
            }
            if (ShadingModel == EMaterialShadingModel::Unlit)
            {
                GPUFlags |= EMaterialGPUFlags::Unlit;
            }
            MaterialUniforms.Flags = (uint32)GPUFlags;
            MaterialUniforms.OpacityClipValue = OpacityMaskClipValue;

            RebuildParameterLookup();

            if (GetMaterialIndex() == -1)
            {
                GRenderManager->GetMaterialManager().AddMaterial(this);
            }
            else
            {
                UpdateMaterialUniforms();
            }

            SetReadyForRender(true);

            NotifyInstancesParentChanged();

#if !USING(WITH_EDITOR)
            // SPIR-V blobs are dead in cooked builds; editor keeps them for recompile/save.
            auto Drop = [](TVector<uint32>& V) { V.clear(); V.shrink_to_fit(); };
            Drop(VertexShaderBinaries);
            Drop(PixelShaderBinaries);
            Drop(MeshShaderBinaries);
            Drop(VisBufferMeshShaderBinaries);
            Drop(VisBufferVertexShaderBinaries);
            Drop(MaskedVisBufferPixelShaderBinaries);
            Drop(MaskedVisBufferPixelShaderPrimBinaries);
            Drop(DeferredShaderBinaries);
#endif
        }

        // Recompile chokepoint; the material editor calls PostLoad() after committing new stages.
        FMeshResolveCache::BumpEpoch();
    }

    void CMaterial::OnDestroy()
    {
        CMaterialInterface::OnDestroy();

        // Resolves are keyed partly on this pointer; drop them before it can be recycled.
        FMeshResolveCache::BumpEpoch();

        if (GetMaterialIndex() != -1)
        {
            GRenderManager->GetMaterialManager().RemoveMaterial(this);
        }
    }

    bool CMaterial::SetScalarValue(const FName& Name, const float Value)
    {
        auto It = ParameterLookup.find(Name);
        if (It != ParameterLookup.end() && It->second.Type == EMaterialParameterType::Scalar)
        {
            const FMaterialParameter& Param = It->second;
            if (Param.Index < MAX_SCALARS)
            {
                MaterialUniforms.Scalars[Param.Index] = Value;
            }
            return true;
        }

        LOG_ERROR("Failed to find material scalar parameter {}", Name);
        return false;
    }

    bool CMaterial::SetVectorValue(const FName& Name, const FVector4& Value)
    {
        auto It = ParameterLookup.find(Name);
        if (It != ParameterLookup.end() && It->second.Type == EMaterialParameterType::Vector)
        {
            const FMaterialParameter& Param = It->second;
            if (Param.Index < MAX_VECTORS)
            {
                MaterialUniforms.Vectors[Param.Index] = Value;
            }
            return true;
        }

        LOG_ERROR("Failed to find material vector parameter {}", Name);
        return false;
    }

    bool CMaterial::GetParameterValue(EMaterialParameterType Type, const FName& Name, FMaterialParameter& Param)
    {
        Param = {};
        auto It = ParameterLookup.find(Name);
        if (It != ParameterLookup.end() && It->second.Type == Type)
        {
            Param = It->second;
            return true;
        }
        return false;
    }

    CMaterial* CMaterial::GetMaterial() const
    {
        return const_cast<CMaterial*>(this);
    }
    
    const FShaderEntry* CMaterial::GetVertexShader() const
    {
        return VertexShader;
    }

    const FShaderEntry* CMaterial::GetPixelShader() const
    {
        return PixelShader;
    }

    CMaterial* CMaterial::GetDefaultMaterial()
    {
        return DefaultMaterial;
    }

    CMaterial* CMaterial::GetDefaultTerrainMaterial()
    {
        return DefaultTerrainMaterial;
    }

    void CMaterial::CreateDefaultMaterial()
    {
        IShaderCompiler* ShaderCompiler = GShaderCompiler;

        ShaderCompiler->Flush();
        
        if (DefaultMaterial)
        {
            DefaultMaterial->RemoveFromRoot();
            DefaultMaterial->ConditionalBeginDestroy();
            DefaultMaterial = nullptr;
        }
        
        DefaultMaterial = NewObject<CMaterial>(nullptr, "DefaultMaterial");
        DefaultMaterial->AddToRoot();
        
        FString LoadedPixelString;
        if (!VFS::ReadFile(LoadedPixelString, "/Engine/Resources/Shaders/MaterialShader/BasePixelPass.slang"))
        {
            LOG_ERROR("Failed to find BasePixelPass.slang!");
            return;
        }

        const char* Token = "$MATERIAL_INPUTS";
        size_t PixelPos = LoadedPixelString.find(Token);

        FString PixelReplacement;
        
        PixelReplacement += "\tFMaterialPixelInputs Material;\n";
        PixelReplacement += "\tMaterial.Diffuse               = float3(1.0);\n";
        PixelReplacement += "\tMaterial.Metallic              = 0.0;\n";
        PixelReplacement += "\tMaterial.Roughness             = 1.0;\n";
        PixelReplacement += "\tMaterial.Specular              = 0.5;\n";
        PixelReplacement += "\tMaterial.Emissive              = float3(0.0);\n";
        PixelReplacement += "\tMaterial.AmbientOcclusion      = 1.0;\n";
        PixelReplacement += "\tMaterial.Normal                = float3(0.0, 0.0, 1.0);\n";
        PixelReplacement += "\tMaterial.Opacity               = 1.0;\n";
        
        if (PixelPos != FString::npos)
        {
            LoadedPixelString.replace(PixelPos, strlen(Token), PixelReplacement);
        }
        else
        {
            LOG_ERROR("Missing [$MATERIAL_INPUTS] in base shader!");
        }
        
        FString LoadedVertexString;
        if (!VFS::ReadFile(LoadedVertexString, "/Engine/Resources/Shaders/MaterialShader/MeshletVertex.slang"))
        {
            LOG_ERROR("Failed to find MeshletVertex.slang!");
            return;
        }

        // Default material: no-op WPO substitution.
        const char* VertexToken = "$MATERIAL_VERTEX_INPUTS";
        size_t VertexPos = LoadedVertexString.find(VertexToken);
        FString VertexReplacement = "Material.WorldPositionOffset = float3(0.0);\n";
        if (VertexPos != FString::npos)
        {
            LoadedVertexString.replace(VertexPos, strlen(VertexToken), VertexReplacement);
        }
        else
        {
            LOG_ERROR("Missing [$MATERIAL_VERTEX_INPUTS] in base vertex shader!");
        }

        ShaderCompiler->CompilerShaderRaw(Move(LoadedPixelString), {}, [](const FShaderHeader& Header) mutable
        {
            DefaultMaterial->PixelShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
        });

        ShaderCompiler->CompilerShaderRaw(Move(LoadedVertexString), {}, [](const FShaderHeader& Header) mutable
        {
            DefaultMaterial->VertexShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
        });

        // Mesh-shader variant of the default material (same no-op WPO substitution). Always compiled so the
        // cooked asset is portable; only used at runtime when the device supports mesh shaders.
        {
            FString LoadedMeshString;
            if (VFS::ReadFile(LoadedMeshString, "/Engine/Resources/Shaders/MaterialShader/MeshletMesh.slang"))
            {
                size_t MeshPos = LoadedMeshString.find(VertexToken);
                if (MeshPos != FString::npos)
                {
                    LoadedMeshString.replace(MeshPos, strlen(VertexToken), VertexReplacement);
                    ShaderCompiler->CompilerShaderRaw(Move(LoadedMeshString), {}, [](const FShaderHeader& Header) mutable
                    {
                        DefaultMaterial->MeshShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
                    });
                }
            }

            FString LoadedVisString;
            if (VFS::ReadFile(LoadedVisString, "/Engine/Resources/Shaders/MaterialShader/MeshletVisBuffer.slang"))
            {
                size_t VisPos = LoadedVisString.find(VertexToken);
                if (VisPos != FString::npos)
                {
                    LoadedVisString.replace(VisPos, strlen(VertexToken), VertexReplacement);
                    ShaderCompiler->CompilerShaderRaw(Move(LoadedVisString), {}, [](const FShaderHeader& Header) mutable
                    {
                        DefaultMaterial->VisBufferMeshShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
                    });
                }
            }

            FString LoadedVisVSString;
            if (VFS::ReadFile(LoadedVisVSString, "/Engine/Resources/Shaders/MaterialShader/MeshletVisBufferVS.slang"))
            {
                size_t VisVSPos = LoadedVisVSString.find(VertexToken);
                if (VisVSPos != FString::npos)
                {
                    LoadedVisVSString.replace(VisVSPos, strlen(VertexToken), VertexReplacement);
                    ShaderCompiler->CompilerShaderRaw(Move(LoadedVisVSString), {}, [](const FShaderHeader& Header) mutable
                    {
                        DefaultMaterial->VisBufferVertexShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
                    });
                }
            }

            // Deferred material pixel shader: BOTH tokens (WPO for reconstruction + the pixel graph).
            FString LoadedDeferredString;
            if (VFS::ReadFile(LoadedDeferredString, "/Engine/Resources/Shaders/MaterialShader/DeferredMaterial.slang"))
            {
                size_t DefVPos = LoadedDeferredString.find(VertexToken);
                if (DefVPos != FString::npos)
                {
                    LoadedDeferredString.replace(DefVPos, strlen(VertexToken), VertexReplacement);
                }
                size_t DefPPos = LoadedDeferredString.find(Token);
                if (DefPPos != FString::npos)
                {
                    LoadedDeferredString.replace(DefPPos, strlen(Token), PixelReplacement);
                    ShaderCompiler->CompilerShaderRaw(Move(LoadedDeferredString), {}, [](const FShaderHeader& Header) mutable
                    {
                        DefaultMaterial->DeferredShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
                    });
                }
            }
        }

        ShaderCompiler->Flush();

        DefaultMaterial->PostLoad();
    }

    void CMaterial::CreateDefaultTerrainMaterial()
    {
        IShaderCompiler* ShaderCompiler = GShaderCompiler;

        ShaderCompiler->Flush();
        
        if (DefaultTerrainMaterial)
        {
            DefaultTerrainMaterial->RemoveFromRoot();
            DefaultTerrainMaterial->ConditionalBeginDestroy();
            DefaultTerrainMaterial = nullptr;
        }

        DefaultTerrainMaterial = NewObject<CMaterial>(nullptr, "DefaultTerrainMaterial");
        DefaultTerrainMaterial->AddToRoot();
        DefaultTerrainMaterial->MaterialType = EMaterialType::Terrain;

        FString LoadedPixelString;
        if (!VFS::ReadFile(LoadedPixelString, "/Engine/Resources/Shaders/MaterialShader/TerrainBasePixelPass.slang"))
        {
            LOG_ERROR("Failed to find TerrainBasePixelPass.slang!");
            return;
        }

        // Default terrain: 4-layer weighted albedo so unassigned terrain reads as distinct painted regions.
        const char* Token = "$MATERIAL_INPUTS";
        size_t PixelPos = LoadedPixelString.find(Token);

        // Blend via the shared TerrainData.slang helper so the formula isn't duplicated here.
        FString PixelReplacement;
        PixelReplacement += "\tfloat3 _TerrainAlbedo = BlendTerrainLayers4(float3(0.45, 0.40, 0.30),\n";
        PixelReplacement += "\t                                           float3(0.25, 0.45, 0.15),\n";
        PixelReplacement += "\t                                           float3(0.55, 0.55, 0.55),\n";
        PixelReplacement += "\t                                           float3(0.85, 0.80, 0.60), HeightUV);\n";
        PixelReplacement += "\tFMaterialPixelInputs Material;\n";
        PixelReplacement += "\tMaterial.Diffuse               = _TerrainAlbedo;\n";
        PixelReplacement += "\tMaterial.Metallic              = 0.0;\n";
        PixelReplacement += "\tMaterial.Roughness             = 0.9;\n";
        PixelReplacement += "\tMaterial.Specular              = 0.5;\n";
        PixelReplacement += "\tMaterial.Emissive              = float3(0.0);\n";
        PixelReplacement += "\tMaterial.AmbientOcclusion      = 1.0;\n";
        PixelReplacement += "\tMaterial.Normal                = float3(0.0, 0.0, 1.0);\n";
        PixelReplacement += "\tMaterial.Opacity               = 1.0;\n";

        if (PixelPos != FString::npos)
        {
            LoadedPixelString.replace(PixelPos, strlen(Token), PixelReplacement);
        }
        else
        {
            LOG_ERROR("Missing [$MATERIAL_INPUTS] in terrain base shader!");
        }

        FString LoadedVertexString;
        if (!VFS::ReadFile(LoadedVertexString, "/Engine/Resources/Shaders/MaterialShader/TerrainBaseVertexPass.slang"))
        {
            LOG_ERROR("Failed to find TerrainBaseVertexPass.slang!");
            return;
        }

        // Default terrain: no WPO; zero-init vertex token.
        const char* VertexToken = "$MATERIAL_VERTEX_INPUTS";
        size_t VertexPos = LoadedVertexString.find(VertexToken);
        FString VertexReplacement = "Material.WorldPositionOffset = float3(0.0);\n";
        if (VertexPos != FString::npos)
        {
            LoadedVertexString.replace(VertexPos, strlen(VertexToken), VertexReplacement);
        }
        else
        {
            LOG_ERROR("Missing [$MATERIAL_VERTEX_INPUTS] in terrain base vertex shader!");
        }

        ShaderCompiler->CompilerShaderRaw(Move(LoadedPixelString), {}, [](const FShaderHeader& Header) mutable
        {
            DefaultTerrainMaterial->PixelShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
        });

        ShaderCompiler->CompilerShaderRaw(Move(LoadedVertexString), {}, [](const FShaderHeader& Header) mutable
        {
            DefaultTerrainMaterial->VertexShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
        });

        ShaderCompiler->Flush();

        DefaultTerrainMaterial->PostLoad();
    }

    void CMaterial::CommitShaderStage(EMaterialShaderStage Stage, TSpan<const uint32> Spirv)
    {
        const FMaterialStageDesc& Desc = GMaterialStages[(size_t)Stage];

        TVector<uint32>& Binaries = this->*Desc.Binaries;
        if (Binaries.data() != Spirv.data())
        {
            Binaries.assign(Spirv.data(), Spirv.data() + Spirv.size());
        }

        this->*Desc.Entry = FShaderLibrary::Commit(FName((GetGUID().ToString() + Desc.Suffix).c_str()),
            Desc.Type, Spirv);
    }

    void CMaterial::ClearShaderStage(EMaterialShaderStage Stage)
    {
        const FMaterialStageDesc& Desc = GMaterialStages[(size_t)Stage];
        (this->*Desc.Binaries).clear();
        this->*Desc.Entry = nullptr;
    }

    const TVector<uint32>& CMaterial::GetShaderStageBinaries(EMaterialShaderStage Stage) const
    {
        return this->*GMaterialStages[(size_t)Stage].Binaries;
    }

    uint64 CMaterial::GetShaderTemplateHash()
    {
        // Computed once per run; template edits require a restart to be noticed, which matches how the
        // shader library loads engine shaders. Deterministic: per-file content hashes folded in path order.
        static const uint64 CachedHash = []() -> uint64
        {
            struct FEntry
            {
                FString Path;
                uint64  ContentHash;
            };
            TVector<FEntry> Files;

            auto Gather = [&Files](FStringView Directory)
            {
                VFS::RecursiveDirectoryIterator(Directory, [&Files](const VFS::FFileInfo& Info)
                {
                    if (Info.IsDirectory() || Info.GetExt() != ".slang")
                    {
                        return;
                    }
                    FString Source;
                    if (VFS::ReadFile(Source, Info.VirtualPath.c_str()))
                    {
                        Files.push_back({ FString(Info.VirtualPath.c_str()), Hash::XXHash::GetHash64(Source) });
                    }
                });
            };
            // Everything a material template can reach: the templates themselves + the shared includes.
            Gather("/Engine/Resources/Shaders/MaterialShader");
            Gather("/Engine/Resources/Shaders/Includes");

            eastl::sort(Files.begin(), Files.end(), [](const FEntry& A, const FEntry& B)
            {
                return A.Path < B.Path;
            });

            size_t Result = Files.size();
            for (const FEntry& File : Files)
            {
                Hash::HashCombine(Result, (size_t)Hash::GetHash64(File.Path));
                Hash::HashCombine(Result, (size_t)File.ContentHash);
            }
            // 0 is the serialized "legacy asset / never compiled" sentinel; never collide with it.
            return Result != 0 ? (uint64)Result : 1ull;
        }();
        return CachedHash;
    }

#if USING(WITH_EDITOR)
    TObjectPtr<CMaterial> CMaterial::PopStaleTemplateMaterial()
    {
        FScopeLock Lock(StaleTemplateMutex);
        if (StaleTemplateMaterials.empty())
        {
            return nullptr;
        }
        TObjectPtr<CMaterial> Material = StaleTemplateMaterials.back();
        StaleTemplateMaterials.pop_back();
        return Material;
    }
#endif
}
