#include "RuntimePCH.h"
#include "Material.h"
#include "Core/Engine/Engine.h"
#include "Core/ArtifactCache/ArtifactCache.h"
#include "Renderer/SpirvStrip.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Material/MaterialParameterCollection.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/Package/Package.h"
#include "FileSystem/FileSystem.h"
#include "Core/Math/Hash/Hash.h"
#include "Core/Object/Cast.h"
#include "Memory/MemoryTracking.h"
#include "Paths/Paths.h"
#include "Renderer/RenderManager.h"
#include "Renderer/RHI.h"
#include "Renderer/RHITexture.h"
#include "Renderer/ShaderCache.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
#include "Renderer/ShaderPaths.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"
#include "Types/Byte.h"
#include "Log/Log.h"
#include <cstdlib>

namespace Lumina
{
    namespace
    {
        CMaterial* GDefaultMaterial = nullptr;
        CMaterial* GErrorMaterial = nullptr;
        CMaterial* GDefaultTerrainMaterial = nullptr;

        constexpr const char* kMaterialShaderDirectory = "/Engine/Resources/Shaders/MaterialShader/";

        // Folded into every material artifact key, so bumping it orphans entries whose layout or codegen no longer matches.
        constexpr uint64 kMaterialArtifactVersion = 1;

        // Stage count, then per stage its id, word count and words.
        TVector<uint8> PackStageBlobs(const TVector<SMaterialStageBlob>& Blobs)
        {
            TVector<uint8> Bytes;
            auto Append = [&Bytes](const void* Data, size_t Size)
            {
                const uint8* Begin = (const uint8*)Data;
                Bytes.Append(Begin, IntCastChecked<int32>(Size));
            };

            const uint32 Count = (uint32)Blobs.Num();
            Append(&Count, sizeof(Count));
            for (const SMaterialStageBlob& Blob : Blobs)
            {
                const uint32 Words = (uint32)Blob.Spirv.Num();
                Append(&Blob.Stage, sizeof(Blob.Stage));
                Append(&Words, sizeof(Words));
                Append(Blob.Spirv.GetData(), Words * sizeof(uint32));
            }
            return Bytes;
        }

        bool UnpackStageBlobs(const TVector<uint8>& Bytes, TVector<SMaterialStageBlob>& OutBlobs)
        {
            int32 Offset = 0;
            auto Read = [&Bytes, &Offset](void* Out, size_t Size)
            {
                if (Size > static_cast<size_t>(Bytes.Num() - Offset))
                {
                    return false;
                }
                Memory::Memcpy(Out, Bytes.GetData() + Offset, Size);
                Offset += static_cast<int32>(Size);
                return true;
            };

            uint32 Count = 0;
            if (!Read(&Count, sizeof(Count)) || Count > (uint32)EMaterialShaderStage::Count)
            {
                return false;
            }

            OutBlobs.Reset();
            OutBlobs.Reserve(Count);
            for (uint32 i = 0; i < Count; ++i)
            {
                SMaterialStageBlob& Blob = OutBlobs.EmplaceGetRef();
                uint32 Words = 0;
                if (!Read(&Blob.Stage, sizeof(Blob.Stage)) || !Read(&Words, sizeof(Words)) || Blob.Stage >= (uint8)EMaterialShaderStage::Count)
                {
                    return false;
                }
                Blob.Spirv.SetNum(Words);
                if (!Read(Blob.Spirv.GetData(), Words * sizeof(uint32)))
                {
                    return false;
                }
            }
            return Offset == Bytes.Num();
        }

        // Test hosts run without an engine, and they simply compile every time.
        bool LoadStageBlobs(uint64 Key, TVector<SMaterialStageBlob>& OutBlobs)
        {
            TVector<uint8> Bytes;
            return Global::GEngine != nullptr && Global::GEngine->GetArtifactCache().Load(Key, Bytes) && UnpackStageBlobs(Bytes, OutBlobs);
        }

        void StoreStageBytes(uint64 Key, const TVector<uint8>& Bytes)
        {
            if (Global::GEngine != nullptr)
            {
                Global::GEngine->GetArtifactCache().Store(Key, Bytes);
            }
        }
        constexpr const char* kPixelInputsToken        = "$MATERIAL_INPUTS";
        constexpr const char* kVertexInputsToken       = "$MATERIAL_VERTEX_INPUTS";

        // The library suffix and shader type of each stage, indexed by EMaterialShaderStage.
        struct FMaterialStageDesc
        {
            const char*      Suffix;
            ERHIShaderType   Type;
        };

        constexpr FMaterialStageDesc GMaterialStages[] =
        {
            { "_PS",   ERHIShaderType::Fragment },
            { "_VS",   ERHIShaderType::Vertex   },
            { "_MS",   ERHIShaderType::Mesh     },
            { "_MSB",  ERHIShaderType::Mesh     },
            { "_VBM",  ERHIShaderType::Mesh     },
            { "_VBMM", ERHIShaderType::Mesh     },
            { "_MVBP", ERHIShaderType::Fragment },
            { "_DM",   ERHIShaderType::Compute  },
            { "_MSSM", ERHIShaderType::Mesh     },
            { "_SMP",  ERHIShaderType::Fragment },
            { "_MSV",  ERHIShaderType::Vertex   },
            { "_MSBV", ERHIShaderType::Vertex   },
            { "_VBV",  ERHIShaderType::Vertex   },
            { "_VBVM", ERHIShaderType::Vertex   },
            { "_MSSMV", ERHIShaderType::Vertex  },
            { "_MVBPV", ERHIShaderType::Fragment },
        };
        static_assert(std::size(GMaterialStages) == (size_t)EMaterialShaderStage::Count, "GMaterialStages must cover every EMaterialShaderStage");

        const FMaterialStageDesc& StageDesc(EMaterialShaderStage Stage)
        {
            return GMaterialStages[(size_t)Stage];
        }

        SMaterialStageBlob* FindStageBlob(TVector<SMaterialStageBlob>& Blobs, EMaterialShaderStage Stage)
        {
            auto It = Algo::FindIf(Blobs, [Stage](const SMaterialStageBlob& Blob) { return Blob.Stage == (uint8)Stage; });
            return It != Blobs.end() ? &*It : nullptr;
        }

        const TVector<uint32>& StageBinaries(const TVector<SMaterialStageBlob>& Blobs, EMaterialShaderStage Stage)
        {
            static const TVector<uint32> Empty;
            auto It = Algo::FindIf(Blobs, [Stage](const SMaterialStageBlob& Blob) { return Blob.Stage == (uint8)Stage; });
            return It != Blobs.end() ? It->Spirv : Empty;
        }

        // Copies Spirv into Stage's blob. The source may already be that blob, which PostLoad commits in place.
        void StoreStageBinaries(TVector<SMaterialStageBlob>& Blobs, EMaterialShaderStage Stage, TSpan<const uint32> Spirv)
        {
            SMaterialStageBlob* Blob = FindStageBlob(Blobs, Stage);
            if (Blob == nullptr)
            {
                Blob = &Blobs.EmplaceGetRef();
                Blob->Stage = (uint8)Stage;
            }
            if (Blob->Spirv.GetData() != Spirv.GetData())
            {
                Blob->Spirv.Reset();
                Blob->Spirv.Append(Spirv.GetData(), Spirv.GetData() + Spirv.Num());
            }
        }

        void ReleaseEntries(TSpan<FShaderH> Entries)
        {
            for (FShaderH& Entry : Entries)
            {
                FShaderLibrary::Release(Entry);
                Entry = {};
            }
        }

        // A placeholder for anything not yet resident, since a negative ID would index the heap out of range.
        uint32 BindlessIDOf(const CTexture* Texture)
        {
            const int32 ResourceID = Texture != nullptr ? Texture->GetResourceID() : -1;
            return ResourceID >= 0 ? (uint32)ResourceID : RHI::Textures::DefaultResourceID();
        }

        // Only reachable from an asset saved before the graph compile refused over-budget slots.
        void ReportParameterOverBudget(const CMaterial* Material, const SMaterialParameter& Param, const char* SlotKind, uint32 Capacity)
        {
            LOG_ERROR("Material '{}' declares {} parameter '{}' at index {}, past the {} slot budget of {}. Its value is dropped "
                      "and the shader samples an unrelated field. Recompile the material and remove a {} parameter.",
                      Material->GetName(), SlotKind, Param.ParameterName, Param.Index, SlotKind, Capacity, SlotKind);
        }

        // A default material short of a required stage stops every world from extracting, silently.
        void ReportBuiltinReadiness(const CMaterial* Material, const char* What)
        {
            if (Material == nullptr || Material->IsReadyForRender())
            {
                return;
            }

            FString Missing;
            for (size_t s = 0; s < (size_t)EMaterialShaderStage::Count; ++s)
            {
                const EMaterialShaderStage Stage = (EMaterialShaderStage)s;
                if (Material->IsStageRequired(Stage) && Material->GetStage(Stage) == FShaderH{})
                {
                    Missing += Missing.IsEmpty() ? "" : ", ";
                    Missing += GMaterialStages[s].Suffix;
                }
            }

            LOG_ERROR("The {} finished creation without these required stages [{}]. No world can extract a frame while it is "
                      "unready, so every viewport stays on its clear color. Rebuild it from File, Shaders, Recompile Default "
                      "Material once the shader error above is fixed.", What, Missing);
        }

        // Every occurrence, because DeferredMaterial evaluates the vertex graph at t and at t-1.
        void ReplaceAllTokens(FString& Source, const char* Token, const FString& Replacement)
        {
            const size_t TokenLength = strlen(Token);
            for (int32 Pos = Source.Find(Token); Pos != Constants::kIndexNone; Pos = Source.Find(Token, Pos + Replacement.Len()))
            {
                Source.RemoveAt(Pos, IntCastChecked<int32>(TokenLength)).InsertAt(Pos, Replacement);
            }
        }

        struct FTemplateToken
        {
            const char*    Token;
            const FString& Replacement;
        };

        // Reads a material template and splices every token, false after logging what was missing.
        bool LoadMaterialTemplate(const FString& Path, std::initializer_list<FTemplateToken> Tokens, FString& OutSource, const char* Owner)
        {
            if (!VFS::ReadFile(OutSource, Path))
            {
                LOG_ERROR("Failed to read '{}' for built-in material '{}'; nothing will render with it.", Path, Owner);
                return false;
            }

            for (const FTemplateToken& Token : Tokens)
            {
                if (OutSource.Find(Token.Token) == Constants::kIndexNone)
                {
                    LOG_ERROR("'{}' is missing [{}]; built-in material '{}' cannot build that stage.", Path, Token.Token, Owner);
                    return false;
                }
                ReplaceAllTokens(OutSource, Token.Token, Token.Replacement);
            }
            return true;
        }

        // Compiles one builtin stage from a template. The result lands as binaries, which PostLoad commits.
        void CompileBuiltinStage(IShaderCompiler& Compiler, CMaterial* Material, EMaterialShaderStage Stage, const char* TemplateFile,
                                 const char* Define, std::initializer_list<FTemplateToken> Tokens, const char* EntryPoint = nullptr)
        {
            const FString TemplatePath = FString(kMaterialShaderDirectory) + TemplateFile;

            FString Source;
            if (!LoadMaterialTemplate(TemplatePath, Tokens, Source, *Material->GetName()))
            {
                return;
            }

            FShaderCompileOptions Options;
            Options.TemplateVirtualPath = TemplatePath;
            if (Define != nullptr)
            {
                Options.MacroDefinitions.Emplace(Define);
            }
            if (EntryPoint != nullptr)
            {
                Options.EntryPoint = EntryPoint;
            }

            Compiler.CompilerShaderRaw(Move(Source), Move(Options), [Material, Stage](const FShaderHeader& Header) mutable
            {
                Material->SetStageBinaries(Stage, TSpan<const uint32>(Header.Binaries.GetData(), Header.Binaries.Num()));
            });
        }

        // A geometry template carries both paths, MeshMain for Stage and VertexMain for its vertex-path twin.
        void CompileBuiltinGeometryStage(IShaderCompiler& Compiler, CMaterial* Material, EMaterialShaderStage Stage, const char* TemplateFile,
                                         const char* Define, std::initializer_list<FTemplateToken> Tokens)
        {
            CompileBuiltinStage(Compiler, Material, Stage, TemplateFile, Define, Tokens, "MeshMain");
            CompileBuiltinStage(Compiler, Material, VertexPathStage(Stage), TemplateFile, Define, Tokens, "VertexMain");
        }

        // Builtins are rebuilt in place, so a compile still in flight lands before the old one is unrooted. Null when there is no compiler.
        IShaderCompiler* BeginBuiltinRebuild(CMaterial*& Slot, const char* What)
        {
            IShaderCompiler* Compiler = Global::GShaderCompiler;
            if (Compiler == nullptr)
            {
                if (!Global::GIsHeadless)
                {
                    LOG_WARN("CMaterial: no shader compiler (the renderer is not initialized); the {} was not created.", What);
                }
                return nullptr;
            }

            Compiler->Flush();

            // The root set holds the only strong reference, so unrooting is what destroys it.
            if (Slot != nullptr)
            {
                Slot->RemoveFromRoot();
                Slot = nullptr;
            }
            return Compiler;
        }

        // The neutral surface every builtin starts from, so a new field is never left uninitialized.
        FString MakeSurfaceInputs(const char* Diffuse, float Roughness)
        {
            FString Inputs;
            Inputs += "\tFMaterialPixelInputs Material = DefaultMaterialInputs();\n";
            Inputs += Format("\tMaterial.Diffuse          = {};\n", Diffuse);
            Inputs += "\tMaterial.Metallic         = 0.0;\n";
            Inputs += Format("\tMaterial.Roughness        = {:.2f};\n", Roughness);
            Inputs += "\tMaterial.Specular         = 0.5;\n";
            Inputs += "\tMaterial.Emissive         = float3(0.0);\n";
            Inputs += "\tMaterial.AmbientOcclusion = 1.0;\n";
            Inputs += "\tMaterial.Normal           = float3(0.0, 0.0, 1.0);\n";
            Inputs += "\tMaterial.Opacity          = 1.0;\n";
            return Inputs;
        }

#if USING(WITH_EDITOR)
        // A tool or test host without a renderer compiles nothing, so the material simply stays on its fallback.
        void QueueEditorRecompile(CMaterial* Material)
        {
            if (FRenderManager* Renderer = TryRender())
            {
                Renderer->GetMaterialCompileQueue().QueueRecompile(Material);
            }
        }
#endif
    }

    void CMaterial::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Materials");

        // A legacy asset keeps its bytecode until a compile gives it the source hash its cache entries are keyed by.
        if (!Ar.IsWriting() || (!Ar.IsCooking() && SourceHash == 0))
        {
            CMaterialInterface::Serialize(Ar);
            return;
        }

        // Bytecode is derived data, so an editor save leaves it to the artifact cache and the asset only changes when the material does.
        if (!Ar.IsCooking())
        {
            FRecursiveScopeLock Lock(ShaderStageMutex);
            TVector<SMaterialStageBlob> CompiledStages = Move(Stages);
            TVector<SMaterialShaderPermutation> CompiledPermutations = Move(Permutations);
            const uint64 CompiledHash = CompiledTemplateHash;
            Stages.Reset();
            Permutations.Reset();
            CompiledTemplateHash = 0;

            CMaterialInterface::Serialize(Ar);

            Stages = Move(CompiledStages);
            Permutations = Move(CompiledPermutations);
            CompiledTemplateHash = CompiledHash;
            return;
        }

        // The shipped game never debugs its shaders, and the embedded source and line tables are a third of each binary.
        FRecursiveScopeLock Lock(ShaderStageMutex);
        TVector<SMaterialStageBlob> EditorStages = Stages;
        TVector<SMaterialShaderPermutation> EditorPermutations = Permutations;

        auto StripBlobs = [](TVector<SMaterialStageBlob>& Blobs)
        {
            for (SMaterialStageBlob& Blob : Blobs)
            {
                Spirv::StripDebugInfo(Blob.Spirv);
            }
        };
        StripBlobs(Stages);
        for (SMaterialShaderPermutation& Permutation : Permutations)
        {
            StripBlobs(Permutation.Stages);
        }

        CMaterialInterface::Serialize(Ar);

        Stages = Move(EditorStages);
        Permutations = Move(EditorPermutations);
    }

    // The engine publishes GShaderCompiler before the first CDO and tests do not, so the creators check for it.
    void CMaterial::PostCreateCDO()
    {
        if (GDefaultMaterial == nullptr)
        {
            CreateDefaultMaterial();
        }
        if (GDefaultTerrainMaterial == nullptr)
        {
            CreateDefaultTerrainMaterial();
        }
    }

    void CMaterial::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Materials");

        // A compiled material has a source hash, bytecode, or the template stamp of an asset saved before either existed.
        const bool bHasCompiledStage = HasCompiledStage();
        if (bHasCompiledStage || CompiledTemplateHash != 0 || SourceHash != 0)
        {
            // Bytecode already in memory came from a cook, an older asset or the compile that just ran, and is current when its stamp matches.
            bool bStale = GetPackage() != nullptr && (CompiledTemplateHash != GetShaderTemplateHash() || !bHasCompiledStage);
            if (bStale && SourceHash != 0 && LoadShadersFromArtifactCache())
            {
                bStale = false;
            }

            if (!bStale)
            {
                CommitSerializedShaders();
#if USING(WITH_EDITOR)
                // Recompiled once in the background so the asset gains a source hash and can drop its embedded bytecode.
                if (SourceHash == 0 && GetPackage() != nullptr)
                {
                    QueueEditorRecompile(this);
                }
#endif
            }
            else
            {
#if USING(WITH_EDITOR)
                QueueEditorRecompile(this);
#else
                LOG_ERROR("Material '{}' has no shaders for templates {:016x}. Open the project in the editor once so they compile into the artifact cache, or cook the content.",
                          *GetPackage()->GetPackagePath(), GetShaderTemplateHash());
                SetCompileFailed(true);
#endif
            }

            ApplyParameterDefaults();

            // A load from a previous compile can still land while the block is rebuilt.
            {
                FRecursiveScopeLock TextureLock(TextureSlotMutex);

                BindTextureSlots();
                StampRenderFlags();
                BindParameterCollections();
                RebuildParameterLookup();
                AcquireOrUploadMaterialSlot();

                // Stale templates committed nothing, and a corrupt asset can be missing a stage, so this is derived.
                const bool bReady = !bStale && HasRequiredStages();
                SetReadyForRender(bReady);
                if (bReady)
                {
                    SetCompileFailed(false);
                }

                PropagateToChildren();
            }

#if !USING(WITH_EDITOR)
            DropCommittedBinaries();
#endif
        }
        else if (GetPackage() != nullptr)
        {
            // Saved without a compile that ever succeeded, so nothing will produce shaders until it is fixed.
            SetCompileFailed(true);
        }

        // Only entries that resolved a surface against this material need rebuilding, and surfaces record both levels.
        FMeshResolveCache::InvalidateDependency(this);
    }

    void CMaterial::OnDestroy()
    {
        CMaterialInterface::OnDestroy();

        {
            FRecursiveScopeLock StageLock(ShaderStageMutex);
            ReleaseEntries(DefaultEntries);
        }
        ClearPermutations();

        // Resolves are keyed partly on this pointer, so they go before it can be recycled.
        FMeshResolveCache::InvalidateDependency(this);
        ReleaseMaterialSlot();
    }

    void CMaterial::PostPropertyChange(FProperty* ChangedProperty)
    {
        Super::PostPropertyChange(ChangedProperty);
        NormalizeRenderStateForDomain();
    }

    void CMaterial::OnReferencesReplaced()
    {
        // A nulled texture leaves its index in the block, and the heap hands that index to the next texture made.
        {
            FRecursiveScopeLock Lock(TextureSlotMutex);
            const uint32 NumTextures = Math::Min<uint32>(ResolvedTextures.Num(), MAX_TEXTURES);
            for (uint32 i = 0; i < NumTextures; ++i)
            {
                if (ResolvedTextures[i] == nullptr)
                {
                    MaterialUniforms.Textures[i] = RHI::Textures::DefaultResourceID();
                }
            }
        }

        RefreshTextureBindings(nullptr);
        FMeshResolveCache::InvalidateDependency(this);
    }

    void CMaterial::NormalizeRenderStateForDomain()
    {
        if (MaterialType == EMaterialType::None)
        {
            LOG_WARN("Material '{}': None is not a drawable domain, using PBR.", GetName());
            MaterialType = EMaterialType::PBR;
        }
        if (!MaterialDomain::SupportsBlendMode(MaterialType, BlendMode))
        {
            BlendMode = EBlendMode::Opaque;
        }
        if (!MaterialDomain::SupportsShadingModel(MaterialType))
        {
            ShadingModel = EMaterialShadingModel::Lit;
        }
    }

    bool CMaterial::SetScalarValue(const FName& Name, const float Value)
    {
        SMaterialParameter Param;
        if (!GetParameterValue(EMaterialParameterType::Scalar, Name, Param))
        {
            LOG_WARN("Material '{}' has no scalar parameter '{}'.", GetName(), Name);
            return false;
        }

        WriteScalarSlot(Param.Index, Value);
        PropagateParameterToChildren(EMaterialParameterType::Scalar, Name, Param.Index);
        return true;
    }

    bool CMaterial::SetVectorValue(const FName& Name, const FVector4& Value)
    {
        SMaterialParameter Param;
        if (!GetParameterValue(EMaterialParameterType::Vector, Name, Param))
        {
            LOG_WARN("Material '{}' has no vector parameter '{}'.", GetName(), Name);
            return false;
        }

        WriteVectorSlot(Param.Index, Value);
        PropagateParameterToChildren(EMaterialParameterType::Vector, Name, Param.Index);
        return true;
    }

    bool CMaterial::GetParameterValue(EMaterialParameterType Type, const FName& Name, SMaterialParameter& Param)
    {
        Param = {};
        auto It = ParameterLookup.Find(Name);
        if (It == nullptr || It->Type != Type)
        {
            return false;
        }
        Param = (*It);
        return true;
    }

    CTexture* CMaterial::GetTextureParameterTexture(const FName& Name, uint32 Index)
    {
        FRecursiveScopeLock Lock(TextureSlotMutex);
        if (Index >= (uint32)Textures.Num())
        {
            return nullptr;
        }
        ResolveTextureSlot(Index);
        return ResolvedTextures[Index].Get();
    }

    uint32 CMaterial::ResolveTextureSlot(uint32 Index)
    {
        FRecursiveScopeLock Lock(TextureSlotMutex);

        if (Index >= (uint32)Textures.Num())
        {
            return RHI::Textures::DefaultResourceID();
        }

        ResolvedTextures.SetNum(Textures.Num());
        if (ResolvedTextures[Index] == nullptr && Textures[Index].IsValid())
        {
            ResolvedTextures[Index] = Textures[Index].LoadSynchronous();
            if (ResolvedTextures[Index] == nullptr)
            {
                const FStringView Path = Textures[Index].GetPath();
                LOG_WARN("Material '{}': texture slot {} failed to load (path '{}'); slot falls back to the placeholder.",
                         GetName(), Index, Path.IsEmpty() ? FStringView("<empty>") : Path);
            }
        }

        CTexture* Texture = ResolvedTextures[Index].Get();

        // Soft refs sit outside the leaf-first load order, so a texture can be loaded and still have no heap slot.
        if (Texture != nullptr && Texture->GetResourceID() < 0)
        {
            LOG_WARN("Material '{}': texture slot {} ('{}') loaded but is not GPU-resident (no heap slot); slot falls back to the placeholder.",
                     GetName(), Index, Texture->GetName());
        }

        // The usual caller writes only an instance's block, so the root would otherwise keep its placeholder.
        const uint32 SlotID = BindlessIDOf(Texture);
        if (Index < MAX_TEXTURES && MaterialUniforms.Textures[Index] != SlotID)
        {
            MaterialUniforms.Textures[Index] = SlotID;
            UploadMaterialUniforms();
        }
        return SlotID;
    }

    bool CMaterial::RequestTexturesResolved()
    {
        FRecursiveScopeLock Lock(TextureSlotMutex);

        const uint32 NumTextures = Math::Min<uint32>(Textures.Num(), MAX_TEXTURES);
        if (NumTextures == 0)
        {
            return true;
        }
        ResolvedTextures.SetNum(Textures.Num());

        // Resolved, empty, or failed for good, none of which change by asking again.
        auto IsSettled = [this](uint32 i)
        {
            return ResolvedTextures[i] != nullptr || !Textures[i].IsValid() || (UnresolvableTextureMask & (1ull << i)) != 0;
        };

        bool bAllSettled = true;
        for (uint32 i = 0; i < NumTextures; ++i)
        {
            bAllSettled &= IsSettled(i);
        }

        if (bAllSettled)
        {
            // A slot baked before its texture was resident holds the placeholder, and nothing else fixes it.
            bool bStaleBlock  = false;
            bool bAllResident = true;
            for (uint32 i = 0; i < NumTextures; ++i)
            {
                if (const CTexture* Texture = ResolvedTextures[i].Get())
                {
                    bAllResident &= Texture->GetResourceID() >= 0;
                    bStaleBlock  |= MaterialUniforms.Textures[i] != BindlessIDOf(Texture);
                }
            }

            if (bStaleBlock)
            {
                RefreshTextureBindings(nullptr);
            }
            return bAllResident;
        }

        if (bTextureLoadRequested)
        {
            return false;
        }
        bTextureLoadRequested = true;

        // Weak, since a material can die while its textures are in flight and the callbacks are unordered.
        TWeakObjectPtr<CMaterial> WeakSelf(this);
        for (uint32 i = 0; i < NumTextures; ++i)
        {
            if (IsSettled(i))
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

                {
                    FRecursiveScopeLock LoadLock(Self->TextureSlotMutex);

                    CTexture* Texture = Cast<CTexture>(Loaded);
                    if (i < (uint32)Self->ResolvedTextures.Num())
                    {
                        Self->ResolvedTextures[i] = Texture;
                    }

                    // Settled on the placeholder, or the surface would sit on the default material forever.
                    if (Texture == nullptr)
                    {
                        Self->UnresolvableTextureMask |= (1ull << i);
                        const FStringView Path = i < (uint32)Self->Textures.Num() ? Self->Textures[i].GetPath() : FStringView();
                        LOG_WARN("Material '{}': texture slot {} failed to load (path '{}'); it stays on the placeholder.",
                                 Self->GetName(), i, Path.IsEmpty() ? FStringView("<empty>") : Path);
                    }
                }

                // Wakes the surfaces that fell back to the default material while this loaded.
                Self->RefreshTextureBindings(nullptr);
                FMeshResolveCache::InvalidateDependency(Self);
            });
        }
        return false;
    }

    bool CMaterial::ReferencesTexture(const CTexture* ChangedTexture) const
    {
        FRecursiveScopeLock Lock(TextureSlotMutex);
        return Algo::AnyOf(ResolvedTextures, [ChangedTexture](const TStrongObjectPtr<CTexture>& Texture) { return Texture.Get() == ChangedTexture; });
    }

    void CMaterial::WarnMissingParameterOnce(const char* Kind, const FName& Name)
    {
        if (!ReportedMissingParameters.Lock()->TryAdd(Name))
        {
            return;
        }
        LOG_WARN("Material '{}': no {} parameter named '{}'.", GetName(), Kind, Name);
    }

    bool CMaterial::RefreshTextureBindings(const CTexture* ChangedTexture)
    {
        if (ChangedTexture != nullptr && !ReferencesTexture(ChangedTexture))
        {
            return false;
        }

        // Every slot baked while unresolved is wrong the same way, and unresolved ones are left alone.
        bool bChanged = false;
        {
            FRecursiveScopeLock Lock(TextureSlotMutex);
            const uint32 NumTextures = Math::Min<uint32>(ResolvedTextures.Num(), MAX_TEXTURES);
            for (uint32 i = 0; i < NumTextures; ++i)
            {
                if (const CTexture* Texture = ResolvedTextures[i].Get())
                {
                    const uint32 SlotID = BindlessIDOf(Texture);
                    bChanged |= MaterialUniforms.Textures[i] != SlotID;
                    MaterialUniforms.Textures[i] = SlotID;
                }
            }
        }

        UploadMaterialUniforms();

        // Instances copied this block wholesale, and nothing else rewrites the slots they inherit.
        PropagateInheritedTextureSlots();

        // A slot that just became resident is what a surface parked on the default material is waiting for.
        if (bChanged)
        {
            FMeshResolveCache::InvalidateDependency(this);
        }
        return true;
    }

    uint32 RefreshMaterialsReferencingTexture(const CTexture* ChangedTexture)
    {
        // Roots first, since instances inherit the slots the roots just re-baked.
        uint32 Refreshed = 0;
        for (TObjectIterator<CMaterial> It; It; ++It)
        {
            CMaterial* Material = *It;
            Refreshed += (Material != nullptr && Material->RefreshTextureBindings(ChangedTexture)) ? 1 : 0;
        }
        for (TObjectIterator<CMaterialInstance> It; It; ++It)
        {
            CMaterialInstance* Instance = *It;
            Refreshed += (Instance != nullptr && Instance->RefreshTextureBindings(ChangedTexture)) ? 1 : 0;
        }
        return Refreshed;
    }

    EMaterialShaderStage ActiveMeshletStage(EMaterialShaderStage Stage)
    {
        return RHI::SupportsMeshShaders() ? Stage : VertexPathStage(Stage);
    }

    bool CMaterial::IsStageRequired(EMaterialShaderStage Stage) const
    {
        const bool bMeshlet = MaterialDomain::IsMeshlet(MaterialType);
        const bool bMasked  = BlendMode == EBlendMode::Masked;

        switch (Stage)
        {
        case EMaterialShaderStage::Pixel:                return true;
        case EMaterialShaderStage::Vertex:               return MaterialDomain::UsesVertexStage(MaterialType);
        case EMaterialShaderStage::MeshShadow:
        case EMaterialShaderStage::MeshBase:
        case EMaterialShaderStage::VisBufferMesh:
        case EMaterialShaderStage::MeshShadowVertex:
        case EMaterialShaderStage::MeshBaseVertex:
        case EMaterialShaderStage::VisBufferVertex:
        case EMaterialShaderStage::Deferred:             return bMeshlet;
        case EMaterialShaderStage::VisBufferMeshMasked:
        case EMaterialShaderStage::MaskedVisBufferPixel:
        case EMaterialShaderStage::MeshShadowMasked:
        case EMaterialShaderStage::VisBufferVertexMasked:
        case EMaterialShaderStage::MaskedVisBufferPixelVertex:
        case EMaterialShaderStage::MeshShadowMaskedVertex:
        case EMaterialShaderStage::ShadowMaskedPixel:    return bMeshlet && bMasked;
        default:                                         return false;
        }
    }

    bool CMaterial::HasRequiredStages() const
    {
        FRecursiveScopeLock Lock(ShaderStageMutex);
        for (size_t s = 0; s < (size_t)EMaterialShaderStage::Count; ++s)
        {
            if (IsStageRequired((EMaterialShaderStage)s) && DefaultEntries[s] == nullptr)
            {
                return false;
            }
        }
        return true;
    }

    FShaderH CMaterial::GetStage(EMaterialShaderStage Stage) const
    {
        FRecursiveScopeLock Lock(ShaderStageMutex);
        return DefaultEntries[(size_t)Stage];
    }

    FShaderH CMaterial::GetStageForKey(EMaterialShaderStage Stage, uint64 Key) const
    {
        FRecursiveScopeLock Lock(ShaderStageMutex);

        // A switch can gate a stage's only content, and a null entry would draw nothing at all.
        if (Key != GetDefaultStaticSwitchKey())
        {
            if (const SMaterialShaderPermutation* Permutation = FindPermutation(Key); Permutation != nullptr && Permutation->Entries[(size_t)Stage] != nullptr)
            {
                return Permutation->Entries[(size_t)Stage];
            }
        }
        return DefaultEntries[(size_t)Stage];
    }

    void CMaterial::CommitShaderStage(EMaterialShaderStage Stage, TSpan<const uint32> Spirv)
    {
        FRecursiveScopeLock Lock(ShaderStageMutex);
        StoreStageBinaries(Stages, Stage, Spirv);
        SwapStageEntry(DefaultEntries[(size_t)Stage], MakeStageEntryName(Stage, 0, false), Stage, Spirv);
    }

    void CMaterial::SetStageBinaries(EMaterialShaderStage Stage, TSpan<const uint32> Spirv)
    {
        FRecursiveScopeLock Lock(ShaderStageMutex);
        StoreStageBinaries(Stages, Stage, Spirv);
    }

    void CMaterial::ClearShaderStage(EMaterialShaderStage Stage)
    {
        FRecursiveScopeLock Lock(ShaderStageMutex);
        Stages.RemoveAll([Stage](const SMaterialStageBlob& Blob) { return Blob.Stage == (uint8)Stage; });
        ReleaseEntries(TSpan<FShaderH>(&DefaultEntries[(size_t)Stage], 1));
    }

    const TVector<uint32>& CMaterial::GetShaderStageBinaries(EMaterialShaderStage Stage) const
    {
        FRecursiveScopeLock Lock(ShaderStageMutex);
        return StageBinaries(Stages, Stage);
    }

    bool CMaterial::HasPermutation(uint64 Key) const
    {
        FRecursiveScopeLock Lock(ShaderStageMutex);
        return Key == GetDefaultStaticSwitchKey() || FindPermutation(Key) != nullptr;
    }

    void CMaterial::CommitPermutationStage(uint64 Key, EMaterialShaderStage Stage, TSpan<const uint32> Spirv)
    {
        FRecursiveScopeLock Lock(ShaderStageMutex);

        SMaterialShaderPermutation* Permutation = FindPermutation(Key);
        if (Permutation == nullptr)
        {
            Permutation = &Permutations.EmplaceGetRef();
            Permutation->Key = Key;
        }

        StoreStageBinaries(Permutation->Stages, Stage, Spirv);
        SwapStageEntry(Permutation->Entries[(size_t)Stage], MakeStageEntryName(Stage, Key, true), Stage, Spirv);
    }

    bool CMaterial::CommitPermutationStageIfCurrent(uint64 Key, uint32 Generation, EMaterialShaderStage Stage, TSpan<const uint32> Spirv)
    {
        // One lock over the test and the commit, or a clear between them would revive the permutation it dropped.
        FRecursiveScopeLock Lock(ShaderStageMutex);
        if (Generation != PermutationGeneration)
        {
            return false;
        }
        CommitPermutationStage(Key, Stage, Spirv);
        return true;
    }

    const TVector<uint32>& CMaterial::GetPermutationStageBinaries(uint64 Key, EMaterialShaderStage Stage) const
    {
        static const TVector<uint32> Empty;
        FRecursiveScopeLock Lock(ShaderStageMutex);
        const SMaterialShaderPermutation* Permutation = FindPermutation(Key);
        return Permutation != nullptr ? StageBinaries(Permutation->Stages, Stage) : Empty;
    }

    void CMaterial::ClearPermutation(uint64 Key)
    {
        FRecursiveScopeLock Lock(ShaderStageMutex);

        auto It = Algo::FindIf(Permutations, [Key](const SMaterialShaderPermutation& Permutation) { return Permutation.Key == Key; });
        if (It == Permutations.end())
        {
            return;
        }

        ReleaseEntries(It->Entries);
        Permutations.RemoveAt(static_cast<int32>(It - Permutations.begin()));
        BumpShaderRevision();
    }

    void CMaterial::ClearPermutations()
    {
        FRecursiveScopeLock Lock(ShaderStageMutex);

        // Moved unconditionally, since a compile in flight was dispatched against the outgoing numbering.
        ++PermutationGeneration;

        if (Permutations.IsEmpty())
        {
            return;
        }
        for (SMaterialShaderPermutation& Permutation : Permutations)
        {
            ReleaseEntries(Permutation.Entries);
        }
        Permutations.Reset();
        BumpShaderRevision();
    }

    SMaterialShaderPermutation* CMaterial::FindPermutation(uint64 Key)
    {
        auto It = Algo::FindIf(Permutations, [Key](const SMaterialShaderPermutation& Permutation) { return Permutation.Key == Key; });
        return It != Permutations.end() ? &*It : nullptr;
    }

    const SMaterialShaderPermutation* CMaterial::FindPermutation(uint64 Key) const
    {
        return const_cast<CMaterial*>(this)->FindPermutation(Key);
    }

    FName CMaterial::MakeStageEntryName(EMaterialShaderStage Stage, uint64 Key, bool bPermutation) const
    {
        const FString Base = GetName().ToString() + "_" + GetGUID().ToString() + StageDesc(Stage).Suffix;
        return FName(bPermutation ? *Format("{}_P{:016X}", Base, Key) : *Base);
    }

    void CMaterial::SwapStageEntry(FShaderH& Entry, const FName& EntryName, EMaterialShaderStage Stage, TSpan<const uint32> Spirv)
    {
        // Commit hands back a counted reference, and identical bytecode returns the entry already held.
        const FShaderH Committed = FShaderLibrary::Commit(EntryName, StageDesc(Stage).Type, Spirv);
        if (Committed == Entry)
        {
            FShaderLibrary::Release(Committed);
            return;
        }

        // A shared entry stays live when one owner re-points, so a weak handle cannot notice and the revision says so.
        FShaderLibrary::Release(Entry);
        Entry = Committed;
        BumpShaderRevision();
    }

    void CMaterial::BumpShaderRevision()
    {
        ++ShaderRevision;
        FMeshResolveCache::InvalidateDependency(this);
    }

    void CMaterial::SetCompileFailed(bool bFailed)
    {
        // A failed compile keeps the previous shaders loaded, so surfaces resolved against them only switch when told.
        if (bCompileFailed.exchange(bFailed, std::memory_order_acq_rel) != bFailed)
        {
            FMeshResolveCache::InvalidateDependency(this);
        }
    }

    int32 CMaterial::FindStaticSwitchBit(const FName& ParameterName) const
    {
        auto It = Algo::FindIf(StaticSwitches, [&ParameterName](const SMaterialStaticSwitch& Switch) { return Switch.ParameterName == ParameterName; });
        return It != StaticSwitches.end() ? (int32)It->BitIndex : Constants::kIndexNone;
    }

    uint64 CMaterial::MakeStaticSwitchKey(const THashMap<FName, bool>& Overrides) const
    {
        uint64 Key = 0;
        for (const SMaterialStaticSwitch& Switch : StaticSwitches)
        {
            const auto Override = Overrides.Find(Switch.ParameterName);
            if (Override != nullptr ? (*Override) : Switch.bDefaultValue)
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

    bool CMaterial::HasCompiledStage() const
    {
        // A compile dispatched before this one landed can still be writing Stages from a worker.
        FRecursiveScopeLock Lock(ShaderStageMutex);
        return Algo::AnyOf(Stages, [](const SMaterialStageBlob& Blob) { return !Blob.Spirv.IsEmpty(); });
    }

    void CMaterial::CommitSerializedShaders()
    {
        FRecursiveScopeLock Lock(ShaderStageMutex);

        for (const SMaterialStageBlob& Blob : Stages)
        {
            if (!Blob.Spirv.IsEmpty() && Blob.Stage < (uint8)EMaterialShaderStage::Count)
            {
                CommitShaderStage((EMaterialShaderStage)Blob.Stage, TSpan<const uint32>(Blob.Spirv.GetData(), Blob.Spirv.Num()));
            }
        }

        // By index, since a commit looks the permutation up again and must not hold an iterator across it.
        for (int32 p = 0; p < Permutations.Num(); ++p)
        {
            for (int32 s = 0; s < Permutations[p].Stages.Num(); ++s)
            {
                const SMaterialStageBlob& Blob = Permutations[p].Stages[s];
                if (!Blob.Spirv.IsEmpty() && Blob.Stage < (uint8)EMaterialShaderStage::Count)
                {
                    CommitPermutationStage(Permutations[p].Key, (EMaterialShaderStage)Blob.Stage, TSpan<const uint32>(Blob.Spirv.GetData(), Blob.Spirv.Num()));
                }
            }
        }
    }

    uint64 CMaterial::MakeArtifactKey(uint64 PermutationKey) const
    {
        const uint64 Parts[] = { SourceHash, GetShaderTemplateHash(), kMaterialArtifactVersion, PermutationKey };
        return Hash::GetHash64(Parts, sizeof(Parts), 0x4D41544C41525446ull);
    }

    bool CMaterial::LoadShadersFromArtifactCache()
    {
        TVector<SMaterialStageBlob> Blobs;
        if (!LoadStageBlobs(MakeArtifactKey(GetDefaultStaticSwitchKey()), Blobs))
        {
            return false;
        }

        FRecursiveScopeLock Lock(ShaderStageMutex);
        Stages = Move(Blobs);
        CompiledTemplateHash = GetShaderTemplateHash();
        return HasCompiledStage();
    }

    bool CMaterial::LoadPermutationFromArtifactCache(uint64 Key)
    {
        if (SourceHash == 0 || HasPermutation(Key))
        {
            return HasPermutation(Key);
        }

        TVector<SMaterialStageBlob> Blobs;
        if (!LoadStageBlobs(MakeArtifactKey(Key), Blobs))
        {
            return false;
        }

        FRecursiveScopeLock Lock(ShaderStageMutex);
        for (const SMaterialStageBlob& Blob : Blobs)
        {
            CommitPermutationStage(Key, (EMaterialShaderStage)Blob.Stage, TSpan<const uint32>(Blob.Spirv.GetData(), Blob.Spirv.Num()));
        }
        return true;
    }

    void CMaterial::StoreShadersInArtifactCache()
    {
        if (SourceHash == 0)
        {
            return;
        }

        TVector<uint8> Bytes;
        {
            FRecursiveScopeLock Lock(ShaderStageMutex);
            Bytes = PackStageBlobs(Stages);
        }
        StoreStageBytes(MakeArtifactKey(GetDefaultStaticSwitchKey()), Bytes);
    }

    void CMaterial::StorePermutationInArtifactCache(uint64 Key)
    {
        if (SourceHash == 0)
        {
            return;
        }

        TVector<uint8> Bytes;
        {
            FRecursiveScopeLock Lock(ShaderStageMutex);
            const SMaterialShaderPermutation* Permutation = FindPermutation(Key);
            if (Permutation == nullptr)
            {
                return;
            }
            Bytes = PackStageBlobs(Permutation->Stages);
        }
        StoreStageBytes(MakeArtifactKey(Key), Bytes);
    }

    void CMaterial::ApplyParameterDefaults()
    {
        // The uniform block is not serialized, so the authored defaults are replayed into it.
        for (const SMaterialParameter& Param : Parameters)
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
                    ReportParameterOverBudget(this, Param, "scalar", MAX_SCALARS);
                }
                break;

            case EMaterialParameterType::Vector:
                if (Param.Index < MAX_VECTORS)
                {
                    MaterialUniforms.Vectors[Param.Index] = Param.VectorDefault;
                }
                else
                {
                    ReportParameterOverBudget(this, Param, "vector", MAX_VECTORS);
                }
                break;

            case EMaterialParameterType::Texture:
                if (Param.Index >= MAX_TEXTURES)
                {
                    ReportParameterOverBudget(this, Param, "texture", MAX_TEXTURES);
                }
                break;
            }
        }
    }

    void CMaterial::BindTextureSlots()
    {
        // Resized rather than assigned, since the graph compile fills it before calling PostLoad.
        ResolvedTextures.SetNum(Textures.Num());

        // A recompile replaces the whole texture table, and a latch left set would suppress the async loads.
        bTextureLoadRequested   = false;
        UnresolvableTextureMask = 0;

        // Only untouched slots start on the placeholder, waiting for a consumer to demand them.
        const uint32 NumTextures = Math::Min<uint32>(Textures.Num(), MAX_TEXTURES);
        for (uint32 i = 0; i < NumTextures; ++i)
        {
            MaterialUniforms.Textures[i] = BindlessIDOf(ResolvedTextures[i].Get());
        }
    }

    void CMaterial::StampRenderFlags()
    {
        EMaterialGPUFlags Flags = EMaterialGPUFlags::None;
        Flags |= BlendMode == EBlendMode::Masked      ? EMaterialGPUFlags::Masked      : EMaterialGPUFlags::None;
        Flags |= BlendMode == EBlendMode::Translucent ? EMaterialGPUFlags::Translucent : EMaterialGPUFlags::None;
        Flags |= BlendMode == EBlendMode::Additive    ? EMaterialGPUFlags::Additive    : EMaterialGPUFlags::None;
        Flags |= bReceivesDecals                      ? EMaterialGPUFlags::None        : EMaterialGPUFlags::NoDecals;

        // A 3-bit field, so the GBuffer stamps the model at runtime rather than specializing the shader.
        const uint32 ShadingModelBits = ((uint32)ShadingModel & kMaterialShadingModelMask) << kMaterialShadingModelShift;

        MaterialUniforms.Flags            = (uint32)Flags | ShadingModelBits;
        MaterialUniforms.OpacityClipValue = OpacityMaskClipValue;
    }

    void CMaterial::BindParameterCollections()
    {
        // Slot 0 is the reserved zero collection, so an unbound entry reads zeros without a sentinel.
        for (uint32 i = 0; i < MAX_MATERIAL_COLLECTIONS; ++i)
        {
            CMaterialParameterCollection* Collection = i < (uint32)ParameterCollections.Num() ? ParameterCollections[i].Get() : nullptr;
            int32 Slot = 0;
            if (IsValid(Collection))
            {
                // Its own PostLoad may not have run yet, and the slot is what the shader indexes with.
                if (!CPackage::ConditionalPostLoad(Collection) && !Collection->HasAnyFlag(OF_WasLoaded))
                {
                    Collection->PostLoad();
                }
                Slot = Math::Max(Collection->GetCollectionIndex(), 0);
            }
            MaterialUniforms.CollectionIndices[i] = (uint32)Slot;
        }
    }

    void CMaterial::RebuildParameterLookup()
    {
        ParameterLookup.Reset();
        ParameterLookup.Reserve(Parameters.Num());
        for (const SMaterialParameter& Param : Parameters)
        {
            ParameterLookup[Param.ParameterName] = Param;
        }
    }

    void CMaterial::DropCommittedBinaries()
    {
        FRecursiveScopeLock Lock(ShaderStageMutex);

        auto Drop = [](TVector<SMaterialStageBlob>& Blobs)
        {
            for (SMaterialStageBlob& Blob : Blobs)
            {
                Blob.Spirv.Reset();
                Blob.Spirv.Shrink();
            }
        };
        Drop(Stages);
        for (SMaterialShaderPermutation& Permutation : Permutations)
        {
            Drop(Permutation.Stages);
        }
    }

    CMaterial* CMaterial::GetDefaultMaterial()
    {
        return GDefaultMaterial;
    }

    CMaterial* CMaterial::GetErrorMaterial()
    {
        return GErrorMaterial;
    }

    CMaterial* CMaterial::GetDefaultTerrainMaterial()
    {
        return GDefaultTerrainMaterial;
    }

    void CMaterial::CreateDefaultMaterial()
    {
        if (BeginBuiltinRebuild(GDefaultMaterial, "default material") == nullptr)
        {
            return;
        }

        // Procedural geometry colors itself per vertex, and an imported mesh without a color stream reads white.
        GDefaultMaterial = CreateBuiltinMaterial("DefaultMaterial", MakeSurfaceInputs("VertexColor.rgb", 1.0f));

        if (BeginBuiltinRebuild(GErrorMaterial, "error material") == nullptr)
        {
            return;
        }

        // ERROR in a 5x7 bitmap font, tiled in world space on the axis the surface faces, so it reads on any mesh with or without UVs.
        FString ErrorInputs = R"(
    const uint _ErrorGlyphRows[21] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F,
                                       0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11,
                                       0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E };
    float3 _ErrorAxis = abs(WorldNormal);
    float2 _ErrorPlane = (_ErrorAxis.x > _ErrorAxis.y && _ErrorAxis.x > _ErrorAxis.z) ? float2(sign(WorldNormal.x) * WorldPosition.z, WorldPosition.y)
                       : (_ErrorAxis.z > _ErrorAxis.y) ? float2(-sign(WorldNormal.z) * WorldPosition.x, WorldPosition.y)
                       : float2(-WorldPosition.x, -sign(WorldNormal.y) * WorldPosition.z);
    // Shifted by a multiple of 180 so the row and column math never sees a negative cell and the tiling is unchanged.
    uint2 _ErrorCell = uint2(int2(floor(_ErrorPlane.x / 0.015), floor(-_ErrorPlane.y / 0.015)) + 1048680);
    uint _ErrorRow = _ErrorCell.y % 10u;
    uint _ErrorColumn = (_ErrorCell.x + ((_ErrorCell.y / 10u) & 1u) * 18u) % 36u;
    bool _ErrorInk = false;
    if (_ErrorRow < 7u && _ErrorColumn < 30u && _ErrorColumn % 6u < 5u)
    {
        uint _ErrorLetter = _ErrorColumn / 6u;
        uint _ErrorGlyph = _ErrorLetter == 0u ? 0u : (_ErrorLetter == 3u ? 2u : 1u);
        _ErrorInk = ((_ErrorGlyphRows[_ErrorGlyph * 7u + _ErrorRow] >> (4u - _ErrorColumn % 6u)) & 1u) != 0u;
    }
    float3 _ErrorColor = _ErrorInk ? float3(0.02, 0.02, 0.02) : float3(1.0, 0.0, 1.0);
)";
        ErrorInputs += MakeSurfaceInputs("_ErrorColor", 1.0f);
        ErrorInputs += "\tMaterial.Emissive = _ErrorColor * 0.25;\n";
        GErrorMaterial = CreateBuiltinMaterial("ErrorMaterial", ErrorInputs);
    }

    CMaterial* CMaterial::CreateBuiltinMaterial(const FName& Name, const FString& PixelInputs, TSpan<const SMaterialParameter> InParameters, EBlendMode InBlendMode)
    {
        IShaderCompiler* Compiler = Global::GShaderCompiler;
        if (Compiler == nullptr)
        {
            if (!Global::GIsHeadless)
            {
                LOG_WARN("CMaterial: no shader compiler (the renderer is not initialized); the built-in material '{}' was not created.", *Name);
            }
            return nullptr;
        }

        CMaterial* Material = NewObject<CMaterial>(nullptr, Name);
        Material->AddToRoot();
        Material->BlendMode = InBlendMode;
        Material->Parameters.Reset();
        Material->Parameters.Append(InParameters.begin(), InParameters.end());

        // A built-in material moves no vertices, so its world position offset is a no-op.
        const FString NoOffset = "\tMaterial.WorldPositionOffset = float3(0.0, 0.0, 0.0);\n";

        CompileBuiltinGeometryStage(*Compiler, Material, EMaterialShaderStage::MeshShadow,          "MeshletMesh.slang",      nullptr,                      { { kVertexInputsToken, NoOffset } });
        CompileBuiltinGeometryStage(*Compiler, Material, EMaterialShaderStage::MeshBase,            "MeshletMesh.slang",      "MESHLET_MESH_BASE",          { { kVertexInputsToken, NoOffset } });
        CompileBuiltinGeometryStage(*Compiler, Material, EMaterialShaderStage::VisBufferMesh,       "MeshletVisBuffer.slang", nullptr,                      { { kVertexInputsToken, NoOffset } });
        CompileBuiltinGeometryStage(*Compiler, Material, EMaterialShaderStage::VisBufferMeshMasked, "MeshletVisBuffer.slang", "VISBUFFER_MASKED_GEOM",      { { kVertexInputsToken, NoOffset } });
        CompileBuiltinGeometryStage(*Compiler, Material, EMaterialShaderStage::MeshShadowMasked,    "MeshletMesh.slang",      "MESHLET_MESH_MASKED_SHADOW", { { kVertexInputsToken, NoOffset } });
        CompileBuiltinStage(*Compiler, Material, EMaterialShaderStage::Pixel,               "BasePixelPass.slang",    nullptr,                      { { kPixelInputsToken, PixelInputs } });
        if (InBlendMode == EBlendMode::Masked)
        {
            CompileBuiltinStage(*Compiler, Material, EMaterialShaderStage::MaskedVisBufferPixel,       "VisBufferMaskedPixel.slang", nullptr,               { { kPixelInputsToken, PixelInputs } });
            CompileBuiltinStage(*Compiler, Material, EMaterialShaderStage::MaskedVisBufferPixelVertex, "VisBufferMaskedPixel.slang", "MESHLET_VERTEX_PATH", { { kPixelInputsToken, PixelInputs } });
            CompileBuiltinStage(*Compiler, Material, EMaterialShaderStage::ShadowMaskedPixel,          "ShadowMaskedPixel.slang",    nullptr,               { { kPixelInputsToken, PixelInputs } });
        }

        // The deferred compute shader carries both graphs, the offset reconstruction and the pixel inputs.
        CompileBuiltinStage(*Compiler, Material, EMaterialShaderStage::Deferred, "DeferredMaterial.slang", nullptr,
                            { { kVertexInputsToken, NoOffset }, { kPixelInputsToken, PixelInputs } });

        Compiler->Flush();
        Material->PostLoad();
        ReportBuiltinReadiness(Material, *Name);
        return Material;
    }

    void CMaterial::CreateDefaultTerrainMaterial()
    {
        IShaderCompiler* Compiler = BeginBuiltinRebuild(GDefaultTerrainMaterial, "default terrain material");
        if (Compiler == nullptr)
        {
            return;
        }

        CMaterial* Material = NewObject<CMaterial>(nullptr, "DefaultTerrainMaterial");
        Material->AddToRoot();
        Material->MaterialType = EMaterialType::Terrain;
        GDefaultTerrainMaterial = Material;

        // A four-layer weighted albedo through the shared TerrainData.slang blend, so unpainted terrain reads as distinct regions.
        FString PixelInputs;
        PixelInputs += "\tfloat3 _TerrainAlbedo = BlendTerrainLayers4(float3(0.45, 0.40, 0.30),\n";
        PixelInputs += "\t                                           float3(0.25, 0.45, 0.15),\n";
        PixelInputs += "\t                                           float3(0.55, 0.55, 0.55),\n";
        PixelInputs += "\t                                           float3(0.85, 0.80, 0.60), HeightUV);\n";
        PixelInputs += MakeSurfaceInputs("_TerrainAlbedo", 0.9f);

        const FString NoOffset = "Material.WorldPositionOffset = float3(0.0);\n";

        CompileBuiltinStage(*Compiler, Material, EMaterialShaderStage::Pixel,  "TerrainBasePixelPass.slang",  nullptr, { { kPixelInputsToken, PixelInputs } });
        CompileBuiltinStage(*Compiler, Material, EMaterialShaderStage::Vertex, "TerrainBaseVertexPass.slang", nullptr, { { kVertexInputsToken, NoOffset } });

        Compiler->Flush();
        Material->PostLoad();
        ReportBuiltinReadiness(Material, "default terrain material");
    }

    uint64 CMaterial::GetShaderTemplateHash()
    {
        // Computed once per run, folded in path order from per-file content hashes.
        static const uint64 CachedHash = []() -> uint64
        {
#if !USING(WITH_EDITOR)
            // A cooked game never recompiles, so it trusts the hash the cooker built its materials against.
            FString Stamped;
            if (VFS::ReadFile(Stamped, CookedShaderTemplateHashPath))
            {
                if (const uint64 Value = std::strtoull(*Stamped, nullptr, 16); Value != 0)
                {
                    return Value;
                }
            }
#endif

            // Everything a material template can reach, the templates themselves plus the shared includes.
            constexpr FStringView Directories[] = { "/Engine/Resources/Shaders/MaterialShader", "/Engine/Resources/Shaders/Includes" };
            return Shaders::HashSourceDirectories(Directories);
        }();
        return CachedHash;
    }

}
