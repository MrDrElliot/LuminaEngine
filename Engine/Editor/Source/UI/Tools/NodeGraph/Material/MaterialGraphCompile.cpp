#include "MaterialGraphCompile.h"
#include "MaterialNodeGraph.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Memory/Memory.h"
#include "Paths/Paths.h"
#include "Renderer/MaterialTypes.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        bool IsStageRequired(const CMaterial* Material, EMaterialShaderStage Stage)
        {
            return Material->IsStageRequired(Stage);
        }

        const char* StageDisplayName(EMaterialShaderStage Stage)
        {
            switch (Stage)
            {
            case EMaterialShaderStage::Pixel:                return "Pixel";
            case EMaterialShaderStage::Vertex:               return "Vertex";
            case EMaterialShaderStage::MeshShadow:           return "Shadow Geometry";
            case EMaterialShaderStage::MeshBase:             return "Base Geometry";
            case EMaterialShaderStage::VisBufferMesh:        return "VisBuffer Geometry";
            case EMaterialShaderStage::VisBufferMeshMasked:  return "Masked VisBuffer Geometry";
            case EMaterialShaderStage::MaskedVisBufferPixel: return "Masked VisBuffer Pixel";
            case EMaterialShaderStage::Deferred:             return "Deferred";
            case EMaterialShaderStage::MomentPixel:          return "Moment Pixel";
            case EMaterialShaderStage::MeshShadowMasked:     return "Masked Shadow Geometry";
            case EMaterialShaderStage::ShadowMaskedPixel:    return "Masked Shadow Pixel";
            default:                                         return "Unknown";
            }
        }
    }

    bool MakeMaterialPermutationTarget(const CMaterial* Material, uint64 Key, FMaterialCompileTarget& OutTarget)
    {
        if (Material == nullptr || Material->StaticSwitches.empty())
        {
            return false;
        }

        OutTarget.bPermutation = true;
        OutTarget.Key          = Key;
        OutTarget.Generation   = Material->GetPermutationGeneration();
        OutTarget.StaticSwitchOverrides.clear();

        for (const FMaterialStaticSwitch& Switch : Material->StaticSwitches)
        {
            OutTarget.StaticSwitchOverrides[Switch.ParameterName] = (Key & (1ull << Switch.BitIndex)) != 0;
        }

        return true;
    }

    bool BeginMaterialGraphCompile(CMaterial* Material, CMaterialNodeGraph* Graph, FMaterialCompiler& Compiler,
        FMaterialGraphCompileResult& Result, const FMaterialCompileTarget& Target)
    {
        if (Material == nullptr || Graph == nullptr)
        {
            return false;
        }

        // A programmatic type change (importer, script) never went through PostPropertyChange.
        Material->NormalizeRenderStateForDomain();

        Compiler.SetMaterialType(Material->GetMaterialType());
        Compiler.SetMasked(Material->GetBlendMode() == EBlendMode::Masked);

        if (Target.bPermutation)
        {
            Compiler.SetStaticSwitchOverrides(Target.StaticSwitchOverrides);

            // Resolved first, or a null slot fails to dedupe and binds its texture a second time.
            TVector<TObjectPtr<CTexture>> Resolved;
            Resolved.reserve(Material->Textures.size());
            for (uint32 i = 0; i < (uint32)Material->Textures.size(); ++i)
            {
                Material->ResolveTextureSlot(i);
                Resolved.push_back(i < (uint32)Material->ResolvedTextures.size() ? Material->ResolvedTextures[i] : nullptr);
            }
            Compiler.SeedManifest(Material->Parameters, Material->MaterialUniforms, Resolved,
                Material->ParameterCollections);

            // Rebuilt from nothing, so a stage the new graph no longer emits cannot linger.
            Material->ClearPermutation(Target.Key);
        }

        Graph->CompileGraph(Compiler);

        if (!Target.bPermutation)
        {
            // A recompile renumbers switch bits, so every key minted against the old manifest is void.
            Material->ClearPermutations();
            Material->SetReadyForRender(false);
        }

        // The failure path keeps warnings, so one does not vanish until the error beside it is fixed.
        Result.Warnings = Compiler.GetWarnings();

        if (Compiler.HasErrors())
        {
            Result.Errors   = Compiler.GetErrors();
            Result.Stats    = Compiler.GetStats();
            Result.bSuccess = false;
            return false;
        }

        // BuildShaders yields both the pixel and vertex source with the $MATERIAL_INPUTS tokens substituted.
        Compiler.BuildShaders(Result.PixelSource, Result.VertexSource, Material->GetMaterialType());
        Result.Stats = Compiler.GetStats();

        IShaderCompiler* ShaderCompiler = GShaderCompiler;

        // Crash-dump-friendly shader names, the material name plus its stage.
        const FString MatName = Material->GetName().c_str();

        FShaderCompileOptions Options;
        Options.DebugName = MatName + " [PS]";
        if (Material->IsMomentResolved())
        {
            Options.MacroDefinitions.emplace_back("TRANSLUCENT");
        }
        if (Material->GetBlendMode() == EBlendMode::AlphaComposite)
        {
            Options.MacroDefinitions.emplace_back("ALPHA_COMPOSITE");
        }
        if (Material->GetBlendMode() == EBlendMode::Modulate)
        {
            Options.MacroDefinitions.emplace_back("MODULATE");
        }
        if (Material->GetBlendMode() == EBlendMode::Masked)
        {
            Options.MacroDefinitions.emplace_back("MASKED");
        }
        // Every model reaches the shader through runtime flags, so an instance can override back to Lit.

        FShaderCompileOptions VSOptions;
        VSOptions.DebugName = MatName + " [VS]";

        // CommitShaderStage owns the blob store and library commit, so this file repeats no suffixes.
        const bool   bPermutation = Target.bPermutation;
        const uint64 TargetKey    = Target.Key;
        const uint32 TargetGen    = Target.Generation;
        auto CommitStage = [Material, bPermutation, TargetKey, TargetGen](EMaterialShaderStage Stage)
        {
            return [Material, Stage, bPermutation, TargetKey, TargetGen](const FShaderHeader& Header)
            {
                const TSpan<const uint32> Spirv(Header.Binaries.data(), Header.Binaries.size());
                if (bPermutation)
                {
                    Material->CommitPermutationStageIfCurrent(TargetKey, TargetGen, Stage, Spirv);
                }
                else
                {
                    Material->CommitShaderStage(Stage, Spirv);
                }
            };
        };

        // A permutation was dropped whole above, so per-stage clears would only touch the master's.
        auto ClearStage = [Material, bPermutation](EMaterialShaderStage Stage)
        {
            if (!bPermutation)
            {
                Material->ClearShaderStage(Stage);
            }
        };

        // Dropped up front, so a domain switch cannot leave the previous domain's stages behind.
        for (size_t s = 0; s < (size_t)EMaterialShaderStage::Count; ++s)
        {
            if (!IsStageRequired(Material, (EMaterialShaderStage)s))
            {
                ClearStage((EMaterialShaderStage)s);
            }
        }

        if (IsStageRequired(Material, EMaterialShaderStage::Vertex))
        {
            ShaderCompiler->CompilerShaderRaw(Result.VertexSource, Move(VSOptions), CommitStage(EMaterialShaderStage::Vertex));
        }

        // Separate compiles rather than spec-constant variants, since they declare different OUTPUT types.
        if (MaterialDomain::IsMeshlet(Material->GetMaterialType()))
        {
            const FString MeshShaderDir = Paths::GetEngineResourceDirectory() + "/Shaders/MaterialShader/";

            const FString MeshSource = Compiler.BuildVertexShaderFromTemplate(MeshShaderDir + "MeshletMesh.slang", EMaterialType::PBR);
            const FString VisSource  = Compiler.BuildVertexShaderFromTemplate(MeshShaderDir + "MeshletVisBuffer.slang", EMaterialType::PBR);

            struct FGeometryStage { const FString* Source; const char* Define; const char* Tag; EMaterialShaderStage Stage; };
            const FGeometryStage GeometryStages[] =
            {
                { &MeshSource, nullptr,             "MS",   EMaterialShaderStage::MeshShadow    },
                { &MeshSource, "MESHLET_MESH_BASE", "MSB",  EMaterialShaderStage::MeshBase      },
                { &VisSource,  nullptr,             "VBM",  EMaterialShaderStage::VisBufferMesh },
            };

            for (const FGeometryStage& Geo : GeometryStages)
            {
                FShaderCompileOptions CompileOptions;
                CompileOptions.DebugName = MatName + " [" + Geo.Tag + "]";
                if (Geo.Define != nullptr)
                {
                    CompileOptions.MacroDefinitions.emplace_back(Geo.Define);
                }
                ShaderCompiler->CompilerShaderRaw(*Geo.Source, Move(CompileOptions), CommitStage(Geo.Stage));
            }

            if (IsStageRequired(Material, EMaterialShaderStage::VisBufferMeshMasked))
            {
                // Masked geometry widens the output back to the full interpolant set its pixel shader reads.
                FShaderCompileOptions VisMaskedOptions; VisMaskedOptions.DebugName = MatName + " [VBMM]";
                VisMaskedOptions.MacroDefinitions.emplace_back("VISBUFFER_MASKED_GEOM");
                ShaderCompiler->CompilerShaderRaw(VisSource, Move(VisMaskedOptions), CommitStage(EMaterialShaderStage::VisBufferMeshMasked));

                const FString MaskedPSSource = Compiler.BuildPixelShaderFromTemplate(MeshShaderDir + "VisBufferMaskedPixel.slang");
                FShaderCompileOptions MaskedPSOptions; MaskedPSOptions.DebugName = MatName + " [MVBP]";
                MaskedPSOptions.MacroDefinitions.emplace_back("VISBUFFER_PRIMID");
                ShaderCompiler->CompilerShaderRaw(MaskedPSSource, Move(MaskedPSOptions), CommitStage(EMaterialShaderStage::MaskedVisBufferPixel));

                // Same widening for the shadow lane, so a cut-out casts its own silhouette and not its quad.
                FShaderCompileOptions ShadowMaskedOptions; ShadowMaskedOptions.DebugName = MatName + " [MSSM]";
                ShadowMaskedOptions.MacroDefinitions.emplace_back("MESHLET_MESH_MASKED_SHADOW");
                ShaderCompiler->CompilerShaderRaw(MeshSource, Move(ShadowMaskedOptions), CommitStage(EMaterialShaderStage::MeshShadowMasked));

                const FString ShadowPSSource = Compiler.BuildPixelShaderFromTemplate(MeshShaderDir + "ShadowMaskedPixel.slang");
                FShaderCompileOptions ShadowPSOptions; ShadowPSOptions.DebugName = MatName + " [SMP]";
                ShaderCompiler->CompilerShaderRaw(ShadowPSSource, Move(ShadowPSOptions), CommitStage(EMaterialShaderStage::ShadowMaskedPixel));
            }

            const FString DeferredSource = Compiler.BuildDeferredShaderFromTemplate(MeshShaderDir + "DeferredMaterial.slang", EMaterialType::PBR);
            FShaderCompileOptions DeferredOptions; DeferredOptions.DebugName = MatName + " [DM]";

            // The runtime model check lives in ShadeGBuffer, so forward and deferred cannot drift apart.
            ShaderCompiler->CompilerShaderRaw(DeferredSource, Move(DeferredOptions), CommitStage(EMaterialShaderStage::Deferred));
        }

        ShaderCompiler->CompilerShaderRaw(Result.PixelSource, Move(Options), CommitStage(EMaterialShaderStage::Pixel));

        if (IsStageRequired(Material, EMaterialShaderStage::MomentPixel))
        {
            FShaderCompileOptions MomentOptions;
            MomentOptions.DebugName = MatName + " [MOM]";
            MomentOptions.MacroDefinitions.emplace_back("TRANSLUCENT");
            MomentOptions.MacroDefinitions.emplace_back("MOMENT_GENERATION");
            // ComputeSurfaceCoverage tests the model at runtime, so the moment pass needs no unlit define.
            ShaderCompiler->CompilerShaderRaw(Result.PixelSource, Move(MomentOptions), CommitStage(EMaterialShaderStage::MomentPixel));
        }

        return true;
    }

    void FinishMaterialGraphCompile(CMaterial* Material, FMaterialCompiler& Compiler,
        FMaterialGraphCompileResult& Result, const FMaterialCompileTarget& Target)
    {
        if (Material == nullptr)
        {
            return;
        }

        // The master recompiled under this permutation, renumbering the bits its key was minted against.
        if (Target.bPermutation && Target.Generation != Material->GetPermutationGeneration())
        {
            Material->ClearPermutation(Target.Key);
            Result.bSuccess = false;
            return;
        }

        // Checked against the permutation's own bytecode, since GetStageForKey would fall back and pass.
        auto StageEmpty = [&](EMaterialShaderStage Stage) -> bool
        {
            const TVector<uint32>& Binaries = Target.bPermutation
                                            ? Material->GetPermutationStageBinaries(Target.Key, Stage)
                                            : Material->GetShaderStageBinaries(Stage);
            if (!Binaries.empty())
            {
                return false;
            }
            EdNodeGraph::FError Error;
            Error.Name        = "Shader Stage Failed";
            Error.Description  = FString("The ") + StageDisplayName(Stage) + " shader stage produced no output (compile failed).";
            Result.Errors.push_back(Error);
            return true;
        };

        // No required stage has a fallback, so a missing one draws nothing rather than taking another path.
        bool bStageFailed = false;
        for (size_t s = 0; s < (size_t)EMaterialShaderStage::Count; ++s)
        {
            const EMaterialShaderStage Stage = (EMaterialShaderStage)s;
            if (IsStageRequired(Material, Stage))
            {
                bStageFailed |= StageEmpty(Stage);
            }
        }
        if (bStageFailed)
        {
            if (Target.bPermutation)
            {
                // Half a permutation draws one surface out of two shader sets; keep the whole fallback.
                Material->ClearPermutation(Target.Key);
            }
            Result.Stats    = Compiler.GetStats();
            Result.bSuccess = false;
            return;   // leave the material not-ready; the caller skips it rather than saving a ghost.
        }

        // The GUID comes from the live object, so a later resolve skips the registry and survives a rename.
        {
            FRecursiveScopeLock TextureLock(Material->TextureSlotMutex);

            TVector<TObjectPtr<CTexture>> BoundTextures;
            Compiler.GetBoundTextures(BoundTextures);

            Material->Textures.clear();
            Material->Textures.reserve(BoundTextures.size());
            Material->ResolvedTextures.clear();
            Material->ResolvedTextures.reserve(BoundTextures.size());

            for (const TObjectPtr<CTexture>& Texture : BoundTextures)
            {
                if (Texture == nullptr)
                {
                    Material->Textures.emplace_back();
                    Material->ResolvedTextures.emplace_back();
                    continue;
                }

                const FGuid Guid = Texture->GetGUID();

                // An empty path is unrecoverable, since the soft ref looks valid but resolves magenta forever.
                FStringView Path;
                if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByGUID(Guid))
                {
                    Path = FStringView(Data->Path.c_str(), Data->Path.size());
                }

                FFixedString PackagePath;
                if (Path.empty() && Texture->GetPackage() != nullptr)
                {
                    PackagePath = Texture->GetPackage()->GetPackagePath();
                    Path = FStringView(PackagePath.c_str(), PackagePath.size());
                }

                if (Path.empty())
                {
                    LOG_WARN("Material '{}': texture '{}' has no resolvable asset path; its slot will "
                             "fall back to the placeholder on any future load.",
                             Material->GetName(), Texture->GetName());
                }

                Material->Textures.emplace_back(FSoftObjectPath(Path, Guid));

                // Round-tripping an already-resident texture through the asset manager rendered it magenta.
                Material->ResolvedTextures.emplace_back(Texture);
            }
        }

        // Slot order is what the shader compiled against, so this is an assign, never a merge.
        Compiler.GetBoundCollections(Material->ParameterCollections);

        Memory::Memzero(&Material->MaterialUniforms, sizeof(FMaterialUniforms));
        Material->Parameters.clear();
        Compiler.GetParameters(Material->Parameters, Material->MaterialUniforms);

        // A permutation never reaches a switch nested under a dropped branch, so it must not renumber.
        if (!Target.bPermutation)
        {
            Compiler.GetStaticSwitches(Material->StaticSwitches);
        }

        // Stamped before PostLoad, which compares it and queues stale materials for auto-recompile.
        Material->CompiledTemplateHash = CMaterial::GetShaderTemplateHash();

        Material->PostLoad();

        Result.bSuccess = true;
    }

    FMaterialGraphCompileResult CompileMaterialGraph(CMaterial* Material, CMaterialNodeGraph* Graph)
    {
        FMaterialGraphCompileResult Result;
        FMaterialCompiler Compiler;

        if (BeginMaterialGraphCompile(Material, Graph, Compiler, Result))
        {
            GShaderCompiler->Flush();
            FinishMaterialGraphCompile(Material, Compiler, Result);
        }

        // Stamped even on failure, since the compile ran against this exact graph.
        if (Graph != nullptr)
        {
            Graph->MarkCompiled();
        }

        return Result;
    }

    FMaterialGraphCompileResult CompileMaterialPermutation(CMaterial* Material, CMaterialNodeGraph* Graph, uint64 Key)
    {
        FMaterialGraphCompileResult Result;
        FMaterialCompileTarget      Target;

        if (!MakeMaterialPermutationTarget(Material, Key, Target))
        {
            return Result;
        }

        FMaterialCompiler Compiler;
        if (BeginMaterialGraphCompile(Material, Graph, Compiler, Result, Target))
        {
            GShaderCompiler->Flush();
            FinishMaterialGraphCompile(Material, Compiler, Result, Target);
        }

        // Deliberately not MarkCompiled, since the graph is unchanged and its default stages still current.
        return Result;
    }

    namespace
    {
        // Dispatches then POLLS across frames, since Flush would park the game thread for the compile.
        struct FPendingStaleRecompile
        {
            // The per-stage commit callbacks capture the material RAW, so this ref has to outlive the wait.
            TObjectPtr<CMaterial>         Material;
            // Finish reads bound textures and parameters back off it, so it outlives the dispatch.
            TUniquePtr<FMaterialCompiler> Compiler;
            FMaterialGraphCompileResult   Result;
        };

        FPendingStaleRecompile GPendingStaleRecompile;

        struct FPendingPermutationCompile
        {
            TObjectPtr<CMaterial>         Material;
            TUniquePtr<FMaterialCompiler> Compiler;
            FMaterialGraphCompileResult   Result;
            FMaterialCompileTarget        Target;
        };

        FPendingPermutationCompile GPendingPermutation;

        // The graph is saved beside the material in its own package, under a fixed name.
        CMaterialNodeGraph* LoadMaterialGraph(CMaterial* Material)
        {
            CPackage* Package = (Material != nullptr) ? Material->GetPackage() : nullptr;
            if (Package == nullptr)
            {
                return nullptr;
            }

            FString GraphName = "AssetMaterialGraph";
            CMaterialNodeGraph* Graph = Cast<CMaterialNodeGraph>(Package->LoadObjectByName(GraphName));
            if (Graph == nullptr)
            {
                return nullptr;
            }

            // The graph's PostLoad already restored wiring, and Initialize only creates the drawing context.
            Graph->SetMaterial(Material);
            Graph->ValidateGraph();
            return Graph;
        }
    }

    void ProcessMaterialPermutationRequests()
    {
        // One at a time, since a permutation is a full multi-stage compile like any other.
        if (GPendingPermutation.Compiler != nullptr)
        {
            if (GShaderCompiler->HasPendingRequests())
            {
                return;
            }

            CMaterial* Material = GPendingPermutation.Material.Get();
            FinishMaterialGraphCompile(Material, *GPendingPermutation.Compiler, GPendingPermutation.Result,
                GPendingPermutation.Target);

            CPackage* Package   = (Material != nullptr) ? Material->GetPackage() : nullptr;
            const FString Name  = (Material != nullptr) ? FString(Material->GetName().c_str()) : FString("<destroyed>");
            const bool bSuccess = GPendingPermutation.Result.bSuccess;

            // A recompile that superseded this permutation fails it with no errors, and says so itself.
            const bool bReportable = !GPendingPermutation.Result.Errors.empty();

            GPendingPermutation = {};

            if (bSuccess && Package != nullptr)
            {
                // The permutation is stored on the master, so it is the master that has to be saved.
                Package->MarkDirty();
            }
            else if (!bSuccess && bReportable)
            {
                ImGuiX::Notifications::NotifyError("Material '{0}': a static switch permutation failed to compile", Name.c_str());
            }
            return;
        }

        TObjectPtr<CMaterial> Material;
        uint64                Key = 0;
        if (!CMaterial::PopPermutationRequest(Material, Key))
        {
            return;
        }

        // Requested before the material finished loading or recompiling, or satisfied while queued.
        if (!Material.IsValid() || !Material->IsReadyForRender() || Material->HasPermutation(Key))
        {
            return;
        }

        FMaterialCompileTarget Target;
        if (!MakeMaterialPermutationTarget(Material.Get(), Key, Target))
        {
            return;
        }

        CMaterialNodeGraph* Graph = LoadMaterialGraph(Material.Get());
        if (Graph == nullptr)
        {
            LOG_WARN("Material '{0}': an instance selects a static switch permutation this material has not "
                     "compiled, and it has no saved graph to compile it from. The instance draws with every "
                     "switch at its default.", Material->GetName().c_str());
            return;
        }

        TUniquePtr<FMaterialCompiler> Compiler = MakeUnique<FMaterialCompiler>();
        FMaterialGraphCompileResult   Result;

        if (!BeginMaterialGraphCompile(Material.Get(), Graph, *Compiler, Result, Target))
        {
            ImGuiX::Notifications::NotifyError("Material '{0}': a static switch permutation failed to compile",
                Material->GetName().c_str());
            return;
        }

        GPendingPermutation.Material = Material;
        GPendingPermutation.Compiler = Move(Compiler);
        GPendingPermutation.Result   = Move(Result);
        GPendingPermutation.Target   = Move(Target);
    }

    void ProcessStaleMaterialRecompiles()
    {
        // One material at a time keeps the swarm's work bounded and the poll below unambiguous.
        if (GPendingStaleRecompile.Compiler != nullptr)
        {
            // Global across every compile, so an unrelated burst delays this finish but never commits wrong.
            if (GShaderCompiler->HasPendingRequests())
            {
                return;
            }

            CMaterial* Material = GPendingStaleRecompile.Material.Get();
            FinishMaterialGraphCompile(Material, *GPendingStaleRecompile.Compiler, GPendingStaleRecompile.Result);

            // Read before the reset, since the material may be the only thing still naming the package.
            CPackage* Package = (Material != nullptr) ? Material->GetPackage() : nullptr;
            const FString Name = (Material != nullptr) ? FString(Material->GetName().c_str()) : FString("<destroyed>");
            const bool bSuccess = GPendingStaleRecompile.Result.bSuccess;

            GPendingStaleRecompile = {};

            if (bSuccess && Package != nullptr)
            {
                // Dirtied so the user can save and stop paying the recompile every session, but never auto-saved.
                Package->MarkDirty();
                ImGuiX::Notifications::NotifyInfo("Material '{0}' recompiled (shader templates changed) - save to keep", Name.c_str());
            }
            else
            {
                ImGuiX::Notifications::NotifyError("Material '{0}' failed to recompile against the current shader templates", Name.c_str());
            }
            return;
        }

        // Spread out, or a template edit that staled many materials becomes one long hitch.
        TObjectPtr<CMaterial> Material = CMaterial::PopStaleTemplateMaterial();
        if (!Material.IsValid())
        {
            return;
        }

        CMaterialNodeGraph* Graph = LoadMaterialGraph(Material.Get());
        if (Graph == nullptr)
        {
            LOG_WARN("Material '{0}' was compiled against older shader templates but has no saved graph to recompile from", Material->GetName().c_str());
            return;
        }

        TUniquePtr<FMaterialCompiler> Compiler = MakeUnique<FMaterialCompiler>();
        FMaterialGraphCompileResult   Result;

        // A graph that failed up front has nothing to wait for and would hold the slot until unrelated work.
        if (!BeginMaterialGraphCompile(Material.Get(), Graph, *Compiler, Result))
        {
            ImGuiX::Notifications::NotifyError("Material '{0}' failed to recompile against the current shader templates", Material->GetName().c_str());
            return;
        }

        GPendingStaleRecompile.Material = Material;
        GPendingStaleRecompile.Compiler = Move(Compiler);
        GPendingStaleRecompile.Result   = Move(Result);
    }
}
