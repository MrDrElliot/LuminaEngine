#include "RuntimePCH.h"
#include "Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Material/MaterialParameterCollection.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Object/ObjectIterator.h"
#include "FileSystem/FileSystem.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Object/Cast.h"
#include "Memory/MemoryTracking.h"
#include "Paths/Paths.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHITexture.h"
#include "Renderer/ShaderCache.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "Types/Byte.h"
#include "Log/Log.h"

namespace Lumina
{
    static CMaterial* DefaultMaterial = nullptr;
    static CMaterial* DefaultTerrainMaterial = nullptr;

    namespace
    {
        // PostLoad and the editor compile both walk this, so adding a stage is a one-line change.
        struct FMaterialStageDesc
        {
            TVector<uint32> CMaterial::*     Binaries;
            FShaderH CMaterial::*            Entry;
            const char*                      Suffix;
            ERHIShaderType                   Type;
        };

        // Indexed by EMaterialShaderStage.
        const FMaterialStageDesc GMaterialStages[] =
        {
            { &CMaterial::PixelShaderBinaries,                &CMaterial::PixelShader,                "_PS",   ERHIShaderType::Fragment },
            { &CMaterial::VertexShaderBinaries,               &CMaterial::VertexShader,               "_VS",   ERHIShaderType::Vertex   },
            { &CMaterial::MeshShaderShadowBinaries,           &CMaterial::MeshShaderShadow,           "_MS",   ERHIShaderType::Mesh     },
            { &CMaterial::MeshShaderBaseBinaries,             &CMaterial::MeshShaderBase,             "_MSB",  ERHIShaderType::Mesh     },
            { &CMaterial::VisBufferMeshShaderBinaries,        &CMaterial::VisBufferMeshShader,        "_VBM",  ERHIShaderType::Mesh     },
            { &CMaterial::VisBufferMeshShaderMaskedBinaries,  &CMaterial::VisBufferMeshShaderMasked,  "_VBMM", ERHIShaderType::Mesh     },
            { &CMaterial::MaskedVisBufferPixelShaderBinaries, &CMaterial::MaskedVisBufferPixelShader, "_MVBP", ERHIShaderType::Fragment },
            { &CMaterial::DeferredShaderBinaries,             &CMaterial::DeferredShader,             "_DM",   ERHIShaderType::Compute  },
            { &CMaterial::MomentPixelShaderBinaries,          &CMaterial::MomentPixelShader,          "_MOM",  ERHIShaderType::Fragment },
            { &CMaterial::MeshShaderShadowMaskedBinaries,     &CMaterial::MeshShaderShadowMasked,     "_MSSM", ERHIShaderType::Mesh     },
            { &CMaterial::ShadowMaskedPixelShaderBinaries,    &CMaterial::ShadowMaskedPixelShader,    "_SMP",  ERHIShaderType::Fragment },
        };
        static_assert(std::size(GMaterialStages) == (size_t)EMaterialShaderStage::Count,
            "GMaterialStages must cover every EMaterialShaderStage");

        // Only reachable from an asset saved before the graph compiler started refusing over-budget slots.
        void ReportParameterOverBudget(const CMaterial* Material, const FName& ParameterName,
                                       const char* SlotKind, uint16 Index, uint32 Capacity)
        {
            LOG_ERROR("Material '{}' declares {} parameter '{}' at index {}, past the {} slot budget of {}. "
                      "Its value is dropped and the shader samples an unrelated field. Recompile the material "
                      "and remove a {} parameter.",
                      Material->GetName(), SlotKind, ParameterName, Index, SlotKind, Capacity, SlotKind);
        }

#if USING(WITH_EDITOR)
        // Filled during the parallel PostLoad wave and drained on the game thread, hence the mutex.
        FMutex                        StaleTemplateMutex;
        TVector<TObjectPtr<CMaterial>> StaleTemplateMaterials;

        void QueueStaleTemplateMaterial(CMaterial* Material)
        {
            FScopeLock Lock(StaleTemplateMutex);
            StaleTemplateMaterials.push_back(Material);
        }

        struct FPermutationRequest
        {
            TObjectPtr<CMaterial> Material;
            uint64                Key = 0;
        };

        // Same threading story as the stale queue, since instances request from the parallel PostLoad wave.
        FMutex                      PermutationRequestMutex;
        TVector<FPermutationRequest> PermutationRequests;
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

    // The engine publishes GShaderCompiler before the first CDO, and tests do not, so the creators say so.
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

    CTexture* CMaterial::GetTextureParameterTexture(const FName& Name, uint32 Index)
    {
        if (Index >= (uint32)Textures.size())
        {
            return nullptr;
        }

        ResolveTextureSlot(Index);
        return ResolvedTextures[Index].Get();
    }

    void CMaterial::UpdateMaterialUniforms()
    {
        // A slot is only ever handed out by the material manager, so holding one implies a renderer.
        if (MaterialIndex != -1)
        {
            Render().GetMaterialManager().UpdateMaterialUniforms(&MaterialUniforms, (uint32)MaterialIndex);
        }
    }

    uint32 RefreshMaterialsReferencingTexture(const CTexture* ChangedTexture)
    {
        uint32 Refreshed = 0;

        for (TObjectIterator<CMaterial> Itr; Itr; ++Itr)
        {
            if (CMaterial* Master = *Itr; Master != nullptr && Master->RefreshTextureBindings(ChangedTexture))
            {
                ++Refreshed;
            }
        }

        for (TObjectIterator<CMaterialInstance> Itr; Itr; ++Itr)
        {
            if (CMaterialInstance* Instance = *Itr; Instance != nullptr && Instance->RefreshTextureBindings(ChangedTexture))
            {
                ++Refreshed;
            }
        }

        return Refreshed;
    }

    uint32 CMaterial::ResolveTextureSlot(uint32 Index)
    {
        if (Index >= (uint32)Textures.size())
        {
            return RHI::Textures::DefaultResourceID();
        }

        if ((uint32)ResolvedTextures.size() != (uint32)Textures.size())
        {
            ResolvedTextures.resize(Textures.size());
        }

        if (ResolvedTextures[Index] == nullptr && Textures[Index].IsValid())
        {
            ResolvedTextures[Index] = Textures[Index].LoadSynchronous();

            // A slot resolved synchronously here leaves the block correct, so nothing calls the refresh.

            if (ResolvedTextures[Index] == nullptr)
            {
                const FStringView Path = Textures[Index].GetPath();
                LOG_WARN("Material '{}': texture slot {} failed to load (path '{}'); slot falls back to "
                         "the placeholder.", GetName(), Index,
                         Path.empty() ? FStringView("<empty>") : Path);
            }
        }

        CTexture* Texture = ResolvedTextures[Index].Get();
        const int32 ResourceID = (Texture != nullptr) ? Texture->GetResourceID() : -1;

        // Soft refs sit outside the leaf-first load order, so this bakes a placeholder nothing fixes.
        if (Texture != nullptr && ResourceID < 0)
        {
            LOG_WARN("Material '{}': texture slot {} ('{}') loaded but is not GPU-resident (no heap slot); "
                     "slot falls back to the placeholder.", GetName(), Index, Texture->GetName());
        }

        const uint32 SlotID = (ResourceID >= 0) ? (uint32)ResourceID : RHI::Textures::DefaultResourceID();

        // The usual caller writes only the instance block, so the master would keep its placeholder.
        if (Index < MAX_TEXTURES && MaterialUniforms.Textures[Index] != SlotID)
        {
            MaterialUniforms.Textures[Index] = SlotID;
            UpdateMaterialUniforms();
        }

        return SlotID;
    }

    bool CMaterial::RequestTexturesResolved()
    {
        const uint32 NumTextures = (uint32)Math::Min<size_t>(Textures.size(), MAX_TEXTURES);
        if (NumTextures == 0)
        {
            return true;
        }

        if ((uint32)ResolvedTextures.size() != (uint32)Textures.size())
        {
            ResolvedTextures.resize(Textures.size());
        }

        bool bAllResolved = true;
        for (uint32 i = 0; i < NumTextures; ++i)
        {
            if (ResolvedTextures[i] != nullptr || !Textures[i].IsValid())
            {
                continue;   // resolved, or empty and never going to resolve
            }
            bAllResolved = false;
        }

        if (bAllResolved)
        {
            // A slot baked before its texture was resident holds the placeholder, and nothing fixes it later.
            bool bStaleBlock  = false;
            bool bAllResident = true;
            for (uint32 i = 0; i < NumTextures; ++i)
            {
                CTexture* Texture = ResolvedTextures[i].Get();
                if (Texture == nullptr)
                {
                    continue;   // empty slot; permanently the placeholder by design
                }

                const int32  ResourceID = Texture->GetResourceID();
                const uint32 SlotID = (ResourceID >= 0) ? (uint32)ResourceID : RHI::Textures::DefaultResourceID();

                bAllResident &= (ResourceID >= 0);
                bStaleBlock  |= (MaterialUniforms.Textures[i] != SlotID);
            }

            if (bStaleBlock)
            {
                RefreshTextureBindings(nullptr);
            }

            // The only point reliably reached once the texture set settles, and queued since this is a worker.

            return bAllResident;
        }

        // Extract asks every frame until the load lands, so re-issuing would queue the same texture dozens of times.
        if (bTextureLoadRequested)
        {
            return false;
        }
        bTextureLoadRequested = true;

        // Weak, since a material can die while its textures are in flight and the callback is unordered.
        TWeakObjectPtr<CMaterial> WeakSelf(this);

        for (uint32 i = 0; i < NumTextures; ++i)
        {
            if (ResolvedTextures[i] != nullptr || !Textures[i].IsValid())
            {
                continue;
            }

            Textures[i].GetSoftPath().LoadAsync([WeakSelf, i](CObject* Loaded)
            {
                CMaterial* Self = WeakSelf.Get();
                if (Self == nullptr)
                {
                    return;
                }

                if (i < (uint32)Self->ResolvedTextures.size())
                {
                    Self->ResolvedTextures[i] = Cast<CTexture>(Loaded);
                }

                // Wakes the surfaces that fell back to the default material while this was loading.
                Self->RefreshTextureBindings(nullptr);
                FMeshResolveCache::InvalidateDependency(Self);
            });
        }

        return false;
    }

    bool CMaterial::ReferencesTexture(const CTexture* ChangedTexture) const
    {
        // Never resolves to answer, since this runs for every material on a texture reimport.
        return Algo::AnyOf(ResolvedTextures,
            [ChangedTexture](const TObjectPtr<CTexture>& Texture) { return Texture.Get() == ChangedTexture; });
    }

    bool CMaterial::RefreshTextureBindings(const CTexture* ChangedTexture)
    {
        if (ChangedTexture != nullptr && !ReferencesTexture(ChangedTexture))
        {
            return false;
        }

        // Any other slot baked while unresolved is wrong the same way, and unresolved ones are left alone.
        const uint32 NumTextures = (uint32)Math::Min<size_t>(ResolvedTextures.size(), MAX_TEXTURES);
        for (uint32 i = 0; i < NumTextures; ++i)
        {
            CTexture* Texture = ResolvedTextures[i].Get();
            if (Texture == nullptr)
            {
                continue;
            }
            const int32 ResourceID = Texture->GetResourceID();
            MaterialUniforms.Textures[i] = (ResourceID >= 0) ? (uint32)ResourceID : RHI::Textures::DefaultResourceID();
        }

        UpdateMaterialUniforms();

        // Marked, not published, since this runs where ResolvedTextures is being written.

        // Instances copied this block wholesale and nothing else rewrites the slots they inherit.
        PropagateInheritedTextureSlots();

        return true;
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
        LUMINA_MEMORY_SCOPE("Materials");
        // PBR geometry compiles no vertex stage, so keying on that would misclassify every surface material.
        bool bHasCompiledStage = false;
        for (size_t i = 0; i < (size_t)EMaterialShaderStage::Count && !bHasCompiledStage; ++i)
        {
            bHasCompiledStage = !(this->*GMaterialStages[i].Binaries).empty();
        }

        if (bHasCompiledStage)
        {
            const bool bStale = GetPackage() != nullptr && CompiledTemplateHash != GetShaderTemplateHash();

            if (bStale)
            {
                #if USING(WITH_EDITOR)
                QueueStaleTemplateMaterial(this);
                #else
                LOG_ERROR("Material '{}' was compiled against different shader templates or an older shader "
                          "cache version and cannot be used by this build. Recook the content.",
                          GetPackage()->GetPackagePath().c_str());
                #endif
            }
            else
            {
                for (size_t i = 0; i < (size_t)EMaterialShaderStage::Count; ++i)
                {
                    const TVector<uint32>& Binaries = this->*GMaterialStages[i].Binaries;
                    if (!Binaries.empty())
                    {
                        CommitShaderStage((EMaterialShaderStage)i, TSpan<const uint32>(Binaries.data(), Binaries.size()));
                    }
                }

                // Iterated by index, since CommitPermutationStage may grow the vector it reads from.
                for (size_t p = 0; p < Permutations.size(); ++p)
                {
                    const uint64 Key = Permutations[p].Key;
                    for (size_t s = 0; s < Permutations[p].Stages.size(); ++s)
                    {
                        const FMaterialStageBlob& Blob = Permutations[p].Stages[s];
                        if (Blob.Spirv.empty() || Blob.Stage >= (uint8)EMaterialShaderStage::Count)
                        {
                            continue;
                        }
                        CommitPermutationStage(Key, (EMaterialShaderStage)Blob.Stage,
                            TSpan<const uint32>(Blob.Spirv.data(), Blob.Spirv.size()));
                    }
                }
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
                    else
                    {
                        ReportParameterOverBudget(this, Param.ParameterName, "scalar", Param.Index, MAX_SCALARS);
                    }
                    break;
                case EMaterialParameterType::Vector:
                    if (Param.Index < MAX_VECTORS)
                    {
                        MaterialUniforms.Vectors[Param.Index] = Param.VectorDefault;
                    }
                    else
                    {
                        ReportParameterOverBudget(this, Param.ParameterName, "vector", Param.Index, MAX_VECTORS);
                    }
                    break;
                case EMaterialParameterType::Texture:
                    if (Param.Index >= MAX_TEXTURES)
                    {
                        ReportParameterOverBudget(this, Param.ParameterName, "texture", Param.Index, MAX_TEXTURES);
                    }
                    break;
                }
            }
            
            // RESIZE, never assign, since the editor compile path fills this before calling PostLoad.
            ResolvedTextures.resize(Textures.size());

            // A recompile replaces the whole texture table; a latch left set would suppress the async kick.
            bTextureLoadRequested = false;

            const uint32 NumTextures = (uint32)Math::Min<size_t>(Textures.size(), MAX_TEXTURES);
            for (uint32 i = 0; i < NumTextures; ++i)
            {
                // Only untouched slots start on the placeholder, waiting for a consumer to demand them.
                CTexture* Resolved = ResolvedTextures[i].Get();
                const int32 ResourceID = (Resolved != nullptr) ? Resolved->GetResourceID() : -1;
                MaterialUniforms.Textures[i] = (ResourceID >= 0) ? (uint32)ResourceID : RHI::Textures::DefaultResourceID();
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
            if (!bReceivesDecals)
            {
                GPUFlags |= EMaterialGPUFlags::NoDecals;
            }
            // A 3-bit field so the GBuffer stamps the model at RUNTIME rather than specializing the shader.
            const uint32 ShadingModelBits =
                ((uint32)ShadingModel & kMaterialShadingModelMask) << kMaterialShadingModelShift;

            MaterialUniforms.Flags = (uint32)GPUFlags | ShadingModelBits;
            MaterialUniforms.OpacityClipValue = OpacityMaskClipValue;

            // Slot 0 is the reserved zero collection, so an unbound entry reads zeros without a sentinel.
            for (uint32 i = 0; i < MAX_MATERIAL_COLLECTIONS; ++i)
            {
                CMaterialParameterCollection* Collection = (i < (uint32)ParameterCollections.size())
                                                         ? ParameterCollections[i].Get() : nullptr;
                int32 Slot = 0;
                if (IsValid(Collection))
                {
                    // Its own PostLoad may not have run, and the slot is what the shader indexes with.
                    Collection->PostLoad();
                    Slot = Math::Max(Collection->GetCollectionIndex(), 0);
                }
                MaterialUniforms.CollectionIndices[i] = (uint32)Slot;
            }

            RebuildParameterLookup();

            // Headless has no material table, and every consumer already treats -1 as no GPU parameters.
            if (GetMaterialIndex() == -1)
            {
                if (FRenderManager* RenderManager = TryRender())
                {
                    RenderManager->GetMaterialManager().AddMaterial(this);
                }
            }
            else
            {
                UpdateMaterialUniforms();
            }

            SetReadyForRender(true);

            PropagateToChildren();

#if !USING(WITH_EDITOR)
            // Driven off the stage table, since the hand-written list kept dropping and missing stages.
            for (const FMaterialStageDesc& Desc : GMaterialStages)
            {
                TVector<uint32>& Blob = this->*Desc.Binaries;
                Blob.clear();
                Blob.shrink_to_fit();
            }

            // The library holds the bytecode now, and a cooked build never recompiles a permutation.
            for (FMaterialShaderPermutation& Permutation : Permutations)
            {
                for (FMaterialStageBlob& Blob : Permutation.Stages)
                {
                    Blob.Spirv.clear();
                    Blob.Spirv.shrink_to_fit();
                }
            }
#endif
        }

        // Only entries that resolved a surface against this master need rebuilding, and surfaces record both.
        FMeshResolveCache::InvalidateDependency(this);
    }

    void CMaterial::OnDestroy()
    {
        CMaterialInterface::OnDestroy();

        // Before MaterialIndex can be recycled by the next material.

        for (const FMaterialStageDesc& Desc : GMaterialStages)
        {
            FShaderLibrary::Release(this->*Desc.Entry);
            this->*Desc.Entry = {};
        }

        ClearPermutations();

        // Resolves are keyed partly on this pointer; drop them before it can be recycled.
        FMeshResolveCache::InvalidateDependency(this);

        if (GetMaterialIndex() != -1)
        {
            if (FRenderManager* RenderManager = TryRender())
            {
                RHI::FRenderRelease Release;
                Release.MaterialSlot = GetMaterialIndex();
                RenderManager->GetReleaseQueue().Post(Release);
            }
            
            SetMaterialIndex(-1);
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
                UploadUniformField(ScalarFieldOffset(Param.Index), &MaterialUniforms.Scalars[Param.Index], sizeof(float));
                PropagateParameterToChildren(EMaterialParameterType::Scalar, Name, Param.Index);
            }
            return true;
        }

        LOG_WARN("Failed to find material scalar parameter {}", Name);
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
                UploadUniformField(VectorFieldOffset(Param.Index), &MaterialUniforms.Vectors[Param.Index], sizeof(FVector4));
                PropagateParameterToChildren(EMaterialParameterType::Vector, Name, Param.Index);
            }
            return true;
        }

        LOG_WARN("Failed to find material vector parameter {}", Name);
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
    
    FShaderH CMaterial::GetVertexShader() const
    {
        return VertexShader;
    }

    FShaderH CMaterial::GetPixelShader() const
    {
        return PixelShader;
    }

    int32 CMaterial::FindStaticSwitchBit(const FName& ParameterName) const
    {
        for (const FMaterialStaticSwitch& Switch : StaticSwitches)
        {
            if (Switch.ParameterName == ParameterName)
            {
                return (int32)Switch.BitIndex;
            }
        }
        return INDEX_NONE;
    }

    uint64 CMaterial::MakeStaticSwitchKey(const THashMap<FName, bool>& Overrides) const
    {
        uint64 Key = 0;
        for (const FMaterialStaticSwitch& Switch : StaticSwitches)
        {
            const auto Override = Overrides.find(Switch.ParameterName);
            const bool bValue = Override != Overrides.end() ? Override->second : Switch.bDefaultValue;
            if (bValue)
            {
                Key |= (1ull << Switch.BitIndex);
            }
        }
        return Key;
    }

    uint64 CMaterial::GetDefaultStaticSwitchKey() const
    {
        return MakeStaticSwitchKey({});
    }

    CMaterial* CMaterial::GetDefaultMaterial()
    {
        return DefaultMaterial;
    }

    CMaterial* CMaterial::GetDefaultTerrainMaterial()
    {
        return DefaultTerrainMaterial;
    }

    // Every occurrence, because DeferredMaterial evaluates the vertex graph twice, at t and at t-1.
    static void ReplaceAllTokens(FString& Source, const char* Token, const FString& Replacement)
    {
        const size_t TokenLen = strlen(Token);
        size_t Pos = Source.find(Token);
        while (Pos != FString::npos)
        {
            Source.replace(Pos, TokenLen, Replacement);
            Pos = Source.find(Token, Pos + Replacement.size());
        }
    }

    void CMaterial::CreateDefaultMaterial()
    {
        IShaderCompiler* ShaderCompiler = GShaderCompiler;
        if (ShaderCompiler == nullptr)
        {
            LOG_WARN("CMaterial: no shader compiler (the renderer is not initialized); "
                     "the default material was not created.");
            return;
        }

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
        
        // Starts from the shared neutral surface, so a new field is never left uninitialized here.
        PixelReplacement += "\tFMaterialPixelInputs Material = DefaultMaterialInputs();\n";
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
            ReplaceAllTokens(LoadedPixelString, Token, PixelReplacement);
        }
        else
        {
            LOG_ERROR("Missing [$MATERIAL_INPUTS] in base shader!");
        }
        
        const char* VertexToken = "$MATERIAL_VERTEX_INPUTS";
        // The default material moves no vertices, so its WPO is a no-op.
        const FString VertexReplacement = "\tMaterial.WorldPositionOffset = float3(0.0, 0.0, 0.0);\n";

        {
            // Written as a loop, so a new variant is one row rather than a copy-paste of a near-identical block.
            struct FGeometryStage { const char* Path; const char* Define; TVector<uint32> CMaterial::* Out; };
            const FGeometryStage GeometryStages[] =
            {
                { "MeshletMesh.slang",      nullptr,                 &CMaterial::MeshShaderShadowBinaries          },
                { "MeshletMesh.slang",      "MESHLET_MESH_BASE",     &CMaterial::MeshShaderBaseBinaries            },
                { "MeshletVisBuffer.slang", nullptr,                 &CMaterial::VisBufferMeshShaderBinaries       },
                { "MeshletVisBuffer.slang", "VISBUFFER_MASKED_GEOM", &CMaterial::VisBufferMeshShaderMaskedBinaries },
                { "MeshletMesh.slang",      "MESHLET_MESH_MASKED_SHADOW", &CMaterial::MeshShaderShadowMaskedBinaries },
            };

            for (const FGeometryStage& Stage : GeometryStages)
            {
                FString Source;
                if (!VFS::ReadFile(Source, FString("/Engine/Resources/Shaders/MaterialShader/") + Stage.Path))
                {
                    continue;
                }

                if (Source.find(VertexToken) == FString::npos)
                {
                    continue;
                }
                ReplaceAllTokens(Source, VertexToken, VertexReplacement);

                FShaderCompileOptions Options;
                Options.TemplateVirtualPath = FString("/Engine/Resources/Shaders/MaterialShader/") + Stage.Path;
                if (Stage.Define != nullptr)
                {
                    Options.MacroDefinitions.emplace_back(Stage.Define);
                }

                TVector<uint32> CMaterial::* Out = Stage.Out;
                ShaderCompiler->CompilerShaderRaw(Move(Source), Move(Options), [Out](const FShaderHeader& Header) mutable
                {
                    (DefaultMaterial->*Out).assign(Header.Binaries.begin(), Header.Binaries.end());
                });
            }
        }

        {
            // The deferred compute shader needs BOTH tokens, the WPO reconstruction and the pixel graph.
            FString LoadedDeferredString;
            if (VFS::ReadFile(LoadedDeferredString, "/Engine/Resources/Shaders/MaterialShader/DeferredMaterial.slang"))
            {
                ReplaceAllTokens(LoadedDeferredString, VertexToken, VertexReplacement);
                size_t DefPPos = LoadedDeferredString.find(Token);
                if (DefPPos != FString::npos)
                {
                    ReplaceAllTokens(LoadedDeferredString, Token, PixelReplacement);
                    FShaderCompileOptions DeferredOptions;
                    DeferredOptions.TemplateVirtualPath = "/Engine/Resources/Shaders/MaterialShader/DeferredMaterial.slang";
                    ShaderCompiler->CompilerShaderRaw(Move(LoadedDeferredString), Move(DeferredOptions), [](const FShaderHeader& Header) mutable
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
        if (ShaderCompiler == nullptr)
        {
            LOG_WARN("CMaterial: no shader compiler (the renderer is not initialized); "
                     "the default terrain material was not created.");
            return;
        }

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

        constexpr const char* TerrainPixelTemplate = "/Engine/Resources/Shaders/MaterialShader/TerrainBasePixelPass.slang";

        FString LoadedPixelString;
        if (!VFS::ReadFile(LoadedPixelString, TerrainPixelTemplate))
        {
            LOG_ERROR("Failed to find TerrainBasePixelPass.slang!");
            return;
        }

        // A four-layer weighted albedo, so unassigned terrain reads as distinct painted regions.
        const char* Token = "$MATERIAL_INPUTS";
        size_t PixelPos = LoadedPixelString.find(Token);

        // Blend via the shared TerrainData.slang helper so the formula isn't duplicated here.
        FString PixelReplacement;
        PixelReplacement += "\tfloat3 _TerrainAlbedo = BlendTerrainLayers4(float3(0.45, 0.40, 0.30),\n";
        PixelReplacement += "\t                                           float3(0.25, 0.45, 0.15),\n";
        PixelReplacement += "\t                                           float3(0.55, 0.55, 0.55),\n";
        PixelReplacement += "\t                                           float3(0.85, 0.80, 0.60), HeightUV);\n";
        // Starts from the shared neutral surface, so a new field is never left uninitialized here.
        PixelReplacement += "\tFMaterialPixelInputs Material = DefaultMaterialInputs();\n";
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
            ReplaceAllTokens(LoadedPixelString, Token, PixelReplacement);
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

        // Default terrain has no WPO, so the vertex token is zero-initialized.
        const char* VertexToken = "$MATERIAL_VERTEX_INPUTS";
        size_t VertexPos = LoadedVertexString.find(VertexToken);
        FString VertexReplacement = "Material.WorldPositionOffset = float3(0.0);\n";
        if (VertexPos != FString::npos)
        {
            ReplaceAllTokens(LoadedVertexString, VertexToken, VertexReplacement);
        }
        else
        {
            LOG_ERROR("Missing [$MATERIAL_VERTEX_INPUTS] in terrain base vertex shader!");
        }

        FShaderCompileOptions TerrainPixelOptions;
        TerrainPixelOptions.TemplateVirtualPath = TerrainPixelTemplate;
        ShaderCompiler->CompilerShaderRaw(Move(LoadedPixelString), Move(TerrainPixelOptions), [](const FShaderHeader& Header) mutable
        {
            DefaultTerrainMaterial->PixelShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
        });

        FShaderCompileOptions TerrainVertexOptions;
        TerrainVertexOptions.TemplateVirtualPath = "/Engine/Resources/Shaders/MaterialShader/TerrainBaseVertexPass.slang";
        ShaderCompiler->CompilerShaderRaw(Move(LoadedVertexString), Move(TerrainVertexOptions), [](const FShaderHeader& Header) mutable
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

        // Commit returns a counted reference, while caches hold deliberately uncounted weak handles.
        const FShaderH Previous  = this->*Desc.Entry;
        const FShaderH Committed = FShaderLibrary::Commit(FName((GetGUID().ToString() + Desc.Suffix).c_str()),
            Desc.Type, Spirv);
        this->*Desc.Entry = Committed;

        if (Previous != Committed)
        {
            // It only actually frees when no OTHER material shares it, since identical bytecode is one entry.
            FShaderLibrary::Release(Previous);

            // A shared entry stays live when one owner re-points, so a weak handle cannot notice the change.
            ++ShaderRevision;
        }
        else
        {
            // Same entry, so Commit's +1 is a duplicate of the reference already held.
            FShaderLibrary::Release(Committed);
        }
    }

    FShaderH CMaterial::GetStageForKey(EMaterialShaderStage Stage, uint64 Key) const
    {
        if (Key != GetDefaultStaticSwitchKey())
        {
            for (const FMaterialShaderPermutation& Permutation : Permutations)
            {
                if (Permutation.Key != Key)
                {
                    continue;
                }

                // A switch can gate a stage's only content, and null would draw nothing at all.
                if (Permutation.Entries[(size_t)Stage] != FShaderH{})
                {
                    return Permutation.Entries[(size_t)Stage];
                }
                break;
            }
        }

        return this->*GMaterialStages[(size_t)Stage].Entry;
    }

    bool CMaterial::HasPermutation(uint64 Key) const
    {
        if (Key == GetDefaultStaticSwitchKey())
        {
            return true;
        }

        return Algo::AnyOf(Permutations, [Key](const FMaterialShaderPermutation& P) { return P.Key == Key; });
    }

    void CMaterial::CommitPermutationStage(uint64 Key, EMaterialShaderStage Stage, TSpan<const uint32> Spirv)
    {
        auto Found = Algo::FindIf(Permutations, [Key](const FMaterialShaderPermutation& P) { return P.Key == Key; });
        if (Found == Permutations.end())
        {
            Permutations.emplace_back();
            Permutations.back().Key = Key;
            Found = Permutations.end() - 1;
        }
        FMaterialShaderPermutation& Permutation = *Found;

        auto FoundBlob = Algo::FindIf(Permutation.Stages,
            [Stage](const FMaterialStageBlob& B) { return B.Stage == (uint8)Stage; });
        if (FoundBlob == Permutation.Stages.end())
        {
            Permutation.Stages.emplace_back();
            Permutation.Stages.back().Stage = (uint8)Stage;
            FoundBlob = Permutation.Stages.end() - 1;
        }
        FMaterialStageBlob& Blob = *FoundBlob;

        if (Blob.Spirv.data() != Spirv.data())
        {
            Blob.Spirv.assign(Spirv.data(), Spirv.data() + Spirv.size());
        }

        const FMaterialStageDesc& Desc = GMaterialStages[(size_t)Stage];

        // Keyed by permutation too, or two permutations of one material collide on the library name.
        const FName EntryName = FName(Format("{}{}_P{:016X}", GetGUID().ToString(), Desc.Suffix, Key).c_str());

        const FShaderH Previous  = Permutation.Entries[(size_t)Stage];
        const FShaderH Committed = FShaderLibrary::Commit(EntryName, Desc.Type, Spirv);
        Permutation.Entries[(size_t)Stage] = Committed;

        if (Previous != Committed)
        {
            FShaderLibrary::Release(Previous);
            ++ShaderRevision;
        }
        else
        {
            FShaderLibrary::Release(Committed);
        }
    }

    const TVector<uint32>& CMaterial::GetPermutationStageBinaries(uint64 Key, EMaterialShaderStage Stage) const
    {
        static const TVector<uint32> Empty;

        for (const FMaterialShaderPermutation& Permutation : Permutations)
        {
            if (Permutation.Key != Key)
            {
                continue;
            }
            for (const FMaterialStageBlob& Blob : Permutation.Stages)
            {
                if (Blob.Stage == (uint8)Stage)
                {
                    return Blob.Spirv;
                }
            }
            break;
        }

        return Empty;
    }

    void CMaterial::ClearPermutation(uint64 Key)
    {
        auto Found = Algo::FindIf(Permutations, [Key](const FMaterialShaderPermutation& P) { return P.Key == Key; });
        if (Found == Permutations.end())
        {
            return;
        }

        for (FShaderH& Entry : Found->Entries)
        {
            FShaderLibrary::Release(Entry);
        }

        Permutations.erase(Found);
        ++ShaderRevision;
    }

    bool CMaterial::CommitPermutationStageIfCurrent(uint64 Key, uint32 Generation, EMaterialShaderStage Stage,
        TSpan<const uint32> Spirv)
    {
        if (Generation != PermutationGeneration)
        {
            return false;
        }

        CommitPermutationStage(Key, Stage, Spirv);
        return true;
    }

    void CMaterial::ClearPermutations()
    {
        // Moved unconditionally, since a compile in flight was dispatched against the outgoing numbering.
        ++PermutationGeneration;

        for (FMaterialShaderPermutation& Permutation : Permutations)
        {
            for (FShaderH& Entry : Permutation.Entries)
            {
                FShaderLibrary::Release(Entry);
                Entry = {};
            }
        }

        if (!Permutations.empty())
        {
            Permutations.clear();
            ++ShaderRevision;
        }
    }

    void CMaterial::ClearShaderStage(EMaterialShaderStage Stage)
    {
        const FMaterialStageDesc& Desc = GMaterialStages[(size_t)Stage];
        (this->*Desc.Binaries).clear();
        FShaderLibrary::Release(this->*Desc.Entry);
        this->*Desc.Entry = {};
    }

    const TVector<uint32>& CMaterial::GetShaderStageBinaries(EMaterialShaderStage Stage) const
    {
        return this->*GMaterialStages[(size_t)Stage].Binaries;
    }

    uint64 CMaterial::GetShaderTemplateHash()
    {
        // Computed once per run, deterministic from per-file content hashes folded in path order.
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
            // Everything a material template can reach, the templates themselves plus the shared includes.
            Gather("/Engine/Resources/Shaders/MaterialShader");
            Gather("/Engine/Resources/Shaders/Includes");
            
            constexpr uint64 Seed = (uint64)FShaderCache::kShaderCacheVersion;

            Algo::Sort(Files, [](const FEntry& A, const FEntry& B)
            {
                return A.Path < B.Path;
            });

            size_t Result = Files.size();
            Hash::HashCombine(Result, (size_t)Seed);
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

    void CMaterial::RequestPermutation(CMaterial* Material, uint64 Key)
    {
        // A cooked material has no graph to compile from, and the default permutation always exists.
        if (!IsValid(Material) || Material->GetPackage() == nullptr || Material->HasPermutation(Key))
        {
            return;
        }

        FScopeLock Lock(PermutationRequestMutex);
        for (const FPermutationRequest& Request : PermutationRequests)
        {
            if (Request.Material == Material && Request.Key == Key)
            {
                return;
            }
        }
        PermutationRequests.push_back({ Material, Key });
    }

    bool CMaterial::PopPermutationRequest(TObjectPtr<CMaterial>& OutMaterial, uint64& OutKey)
    {
        FScopeLock Lock(PermutationRequestMutex);
        if (PermutationRequests.empty())
        {
            return false;
        }
        OutMaterial = PermutationRequests.back().Material;
        OutKey      = PermutationRequests.back().Key;
        PermutationRequests.pop_back();
        return true;
    }
#endif
}
