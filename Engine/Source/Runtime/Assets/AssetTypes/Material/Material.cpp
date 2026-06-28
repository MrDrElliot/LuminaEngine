#include "pch.h"
#include "Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "FileSystem/FileSystem.h"
#include "Memory/MemoryTracking.h"
#include "Paths/Paths.h"
#include "Renderer/RenderManager.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
#include "Types/Byte.h"

namespace Lumina
{
    static CMaterial* DefaultMaterial = nullptr;
    static CMaterial* DefaultTerrainMaterial = nullptr;

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
            // Library entries keyed by asset GUID: stable across reloads, refreshed in place on recompile.
            const FString Guid = GetGUID().ToString();
            VertexShader = FShaderLibrary::Commit(FName((Guid + "_VS").c_str()), ERHIShaderType::Vertex,
                TSpan<const uint32>(VertexShaderBinaries.data(), VertexShaderBinaries.size()));
            PixelShader  = FShaderLibrary::Commit(FName((Guid + "_PS").c_str()), ERHIShaderType::Fragment,
                TSpan<const uint32>(PixelShaderBinaries.data(), PixelShaderBinaries.size()));

            // The single merged VS (MeshletVertex.slang) serves base/depth/shadow via the EPass spec constant.
            // Optional mesh-shader geometry stage (same passes); the renderer picks it per r.MeshShaders.
            if (!MeshShaderBinaries.empty())
            {
                MeshShader = FShaderLibrary::Commit(FName((Guid + "_MS").c_str()), ERHIShaderType::Mesh,
                    TSpan<const uint32>(MeshShaderBinaries.data(), MeshShaderBinaries.size()));
            }
            if (!VisBufferMeshShaderBinaries.empty())
            {
                VisBufferMeshShader = FShaderLibrary::Commit(FName((Guid + "_VBM").c_str()), ERHIShaderType::Mesh,
                    TSpan<const uint32>(VisBufferMeshShaderBinaries.data(), VisBufferMeshShaderBinaries.size()));
            }
            if (!VisBufferVertexShaderBinaries.empty())
            {
                VisBufferVertexShader = FShaderLibrary::Commit(FName((Guid + "_VBV").c_str()), ERHIShaderType::Vertex,
                    TSpan<const uint32>(VisBufferVertexShaderBinaries.data(), VisBufferVertexShaderBinaries.size()));
            }
            if (!MaskedVisBufferPixelShaderBinaries.empty())
            {
                MaskedVisBufferPixelShader = FShaderLibrary::Commit(FName((Guid + "_MVBP").c_str()), ERHIShaderType::Fragment,
                    TSpan<const uint32>(MaskedVisBufferPixelShaderBinaries.data(), MaskedVisBufferPixelShaderBinaries.size()));
            }
            if (!MaskedVisBufferPixelShaderPrimBinaries.empty())
            {
                MaskedVisBufferPixelShaderPrim = FShaderLibrary::Commit(FName((Guid + "_MVBPP").c_str()), ERHIShaderType::Fragment,
                    TSpan<const uint32>(MaskedVisBufferPixelShaderPrimBinaries.data(), MaskedVisBufferPixelShaderPrimBinaries.size()));
            }
            if (!DeferredShaderBinaries.empty())
            {
                DeferredShader = FShaderLibrary::Commit(FName((Guid + "_DM").c_str()), ERHIShaderType::Fragment,
                    TSpan<const uint32>(DeferredShaderBinaries.data(), DeferredShaderBinaries.size()));
            }

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
    }

    void CMaterial::OnDestroy()
    {
        CMaterialInterface::OnDestroy();
        
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
}
