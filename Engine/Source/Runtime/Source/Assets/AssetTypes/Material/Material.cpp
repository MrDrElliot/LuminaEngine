#include "RuntimePCH.h"
#include "Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Object/ObjectIterator.h"
#include "FileSystem/FileSystem.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Object/Cast.h"
#include "Memory/MemoryTracking.h"
#include "Paths/Paths.h"
#include "EASTL/sort.h"
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
        // Which serialized SPIR-V blob feeds which library entry, plus the GUID-key suffix and pipeline
        // type. PostLoad and the editor compile both walk this, so adding a stage is a one-line change.
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
        };
        static_assert(std::size(GMaterialStages) == (size_t)EMaterialShaderStage::Count,
            "GMaterialStages must cover every EMaterialShaderStage");

#if USING(WITH_EDITOR)
        // Filled during the parallel PostLoad wave, drained on the game thread -- hence the mutex, and
        // TObjectPtr in case a material dies before the drain reaches it.
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

    // Both defaults are generated from shader source, so this needs a live compiler. The engine
    // publishes GShaderCompiler before the first CDO; tests and tooling do not, and the creators say so.
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

            // The set the streamer was told about is now out of date. This path is the one that made the
            // old publish-on-RefreshTextureBindings-only scheme miss: a slot resolved synchronously here
            // leaves the uniform block correct, so nothing downstream ever calls RefreshTextureBindings.

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

        // Loaded but with no heap slot means its PostLoad has not run. Soft refs are outside the load
        // graph's leaf-first ordering, so this is loud: it bakes a placeholder nothing comes back to fix.
        if (Texture != nullptr && ResourceID < 0)
        {
            LOG_WARN("Material '{}': texture slot {} ('{}') loaded but is not GPU-resident (no heap slot); "
                     "slot falls back to the placeholder.", GetName(), Index, Texture->GetName());
        }

        const uint32 SlotID = (ResourceID >= 0) ? (uint32)ResourceID : RHI::Textures::DefaultResourceID();

        // Keep this material's OWN block in step with what it just resolved. The usual caller writes only
        // the instance block, so without this the master keeps its placeholder and renders magenta.
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
            // "Resolved" is not "the block says so": a slot baked while its texture was not yet GPU-resident
            // holds the placeholder and no load completion will fix it. Report not-ready so the surface retries.
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

            // Every frame, but a no-op after the first: this is the only point that is reliably reached
            // once a material's texture set has settled, whatever route resolved it. Queued rather than
            // published -- this gate runs on a worker fiber inside Extract.

            return bAllResident;
        }

        // One request per material, not per frame: Extract asks again every frame until the load lands,
        // and re-issuing would queue the same texture dozens of times before the first one completed.
        if (bTextureLoadRequested)
        {
            return false;
        }
        bTextureLoadRequested = true;

        // Weak, because a material can be destroyed while its textures are still in flight -- the
        // callback fires on the loader's completion, which is not ordered against OnDestroy.
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

                // Re-push the block with whatever has landed so far, then wake the surfaces that fell
                // back to the default material while this was loading.
                Self->RefreshTextureBindings(nullptr);
                FMeshResolveCache::InvalidateDependency(Self);
            });
        }

        return false;
    }

    bool CMaterial::ReferencesTexture(const CTexture* ChangedTexture) const
    {
        // Compares RESOLVED slots only and must never resolve to answer -- this runs for every material on
        // a texture reimport. A slot nobody demanded cannot be displaying the changed texture anyway.
        for (const TObjectPtr<CTexture>& Texture : ResolvedTextures)
        {
            if (Texture.Get() == ChangedTexture)
            {
                return true;
            }
        }
        return false;
    }

    bool CMaterial::RefreshTextureBindings(const CTexture* ChangedTexture)
    {
        if (ChangedTexture != nullptr && !ReferencesTexture(ChangedTexture))
        {
            return false;
        }

        // Every resolved slot, not just the changed one: any other slot baked while unresolved is wrong the
        // same way. Unresolved slots are left alone rather than loading defaults nothing asked for.
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

        // Same trigger as the block re-push: this is exactly the point where the resolved texture set is
        // known to have changed, so it is where the streamer's mapping is marked stale.
        //
        // Marked, not published: this runs from the async load completion (see the LoadAsync callback below)
        // and from the resolve gate's worker fiber, and publishing walks ResolvedTextures -- which the very
        // completion that got us here is writing. The streamer drains the queue on the game thread.

        // Instances copied this block wholesale and nothing else rewrites the slots they inherit. Worst for
        // a slot bound without a parameter, which every parameter-driven path on the instance is blind to.
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
        // Asked of the whole stage table, not two named stages: PBR geometry is task + mesh and compiles no
        // vertex stage, so keying on that would classify every surface material as never-compiled.
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
            
            // Textures are deliberately NOT resolved here; slots start on the placeholder and are demanded per
            // slot. RESIZE, never assign -- the editor compile path fills this before calling PostLoad.
            ResolvedTextures.resize(Textures.size());

            // A recompile replaces the whole texture table; a latch left set would suppress the async kick.
            bTextureLoadRequested = false;

            const uint32 NumTextures = (uint32)Math::Min<size_t>(Textures.size(), MAX_TEXTURES);
            for (uint32 i = 0; i < NumTextures; ++i)
            {
                // Already-resolved slots keep their real ID; only the untouched ones start on the
                // placeholder, waiting for a consumer to demand them.
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
            // Shading model rides the flags as a 3-bit field so the GBuffer pass stamps it at RUNTIME instead
            // of specializing the shader per model. Lit is 0, so an ordinary material contributes nothing.
            const uint32 ShadingModelBits =
                ((uint32)ShadingModel & kMaterialShadingModelMask) << kMaterialShadingModelShift;

            MaterialUniforms.Flags = (uint32)GPUFlags | ShadingModelBits;
            MaterialUniforms.OpacityClipValue = OpacityMaskClipValue;

            RebuildParameterLookup();

            // Headless has no material table, so the slot stays -1 and every consumer of it already
            // treats that as "no GPU parameters" -- a dedicated server loads materials but renders none.
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
            // SPIR-V blobs are dead in cooked builds; the editor keeps them for recompile/save. Driven off the
            // stage table because the hand-written list kept dropping stages and missing new ones.
            for (const FMaterialStageDesc& Desc : GMaterialStages)
            {
                TVector<uint32>& Blob = this->*Desc.Binaries;
                Blob.clear();
                Blob.shrink_to_fit();
            }
#endif
        }

        // Recompile chokepoint. Only entries that resolved a surface against this master (directly or via
        // an instance) need rebuilding, and surfaces record both.
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
        
        // Start from the shared neutral surface so a field added to FMaterialPixelInputs is never left
        // uninitialized here; the overrides below are what makes this material what it is.
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
            LoadedPixelString.replace(PixelPos, strlen(Token), PixelReplacement);
        }
        else
        {
            LOG_ERROR("Missing [$MATERIAL_INPUTS] in base shader!");
        }
        
        const char* VertexToken = "$MATERIAL_VERTEX_INPUTS";
        // No-op WPO: the default material moves no vertices.
        const FString VertexReplacement = "\tMaterial.WorldPositionOffset = float3(0.0, 0.0, 0.0);\n";

        {
            // Every meshlet geometry stage is the same two templates compiled with a macro. Written as a
            // loop rather than four near-identical blocks so a new variant is one row, not a copy-paste.
            struct FGeometryStage { const char* Path; const char* Define; TVector<uint32> CMaterial::* Out; };
            const FGeometryStage GeometryStages[] =
            {
                { "MeshletMesh.slang",      nullptr,                 &CMaterial::MeshShaderShadowBinaries          },
                { "MeshletMesh.slang",      "MESHLET_MESH_BASE",     &CMaterial::MeshShaderBaseBinaries            },
                { "MeshletVisBuffer.slang", nullptr,                 &CMaterial::VisBufferMeshShaderBinaries       },
                { "MeshletVisBuffer.slang", "VISBUFFER_MASKED_GEOM", &CMaterial::VisBufferMeshShaderMaskedBinaries },
            };

            for (const FGeometryStage& Stage : GeometryStages)
            {
                FString Source;
                if (!VFS::ReadFile(Source, FString("/Engine/Resources/Shaders/MaterialShader/") + Stage.Path))
                {
                    continue;
                }

                const size_t Pos = Source.find(VertexToken);
                if (Pos == FString::npos)
                {
                    continue;
                }
                Source.replace(Pos, strlen(VertexToken), VertexReplacement);

                FShaderCompileOptions Options;
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
            // Deferred material compute shader: BOTH tokens (WPO for reconstruction + the pixel graph).
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
        // Start from the shared neutral surface so a field added to FMaterialPixelInputs is never left
        // uninitialized here; the overrides below are what makes this material what it is.
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

        // A material stage is a STRONG reference: Commit returns +1 and this object owns it until the stage
        // is re-committed or cleared. Caches hold weak FShaderH and are deliberately uncounted.
        const FShaderH Previous  = this->*Desc.Entry;
        const FShaderH Committed = FShaderLibrary::Commit(FName((GetGUID().ToString() + Desc.Suffix).c_str()),
            Desc.Type, Spirv);
        this->*Desc.Entry = Committed;

        if (Previous != Committed)
        {
            // The old entry loses this owner. It only actually frees if no OTHER material shares it --
            // content keying means identical bytecode is one entry with many owners.
            FShaderLibrary::Release(Previous);

            // Only on a real swap. Commit is content-keyed, so an unchanged recompile hands back the same
            // entry -- bumping unconditionally would make every load re-resolve every dynamic mesh for
            // nothing. NOTE this is still required with handles: a shared entry stays LIVE when one of its
            // owners re-points, so a weak handle to it keeps resolving and cannot notice the change.
            ++ShaderRevision;
        }
        else
        {
            // Same entry, so Commit's +1 is a duplicate of the reference already held.
            FShaderLibrary::Release(Committed);
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
            
            constexpr uint64 Seed = (uint64)FShaderCache::SHADER_CACHE_VERSION;

            eastl::sort(Files.begin(), Files.end(), [](const FEntry& A, const FEntry& B)
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
#endif
}
