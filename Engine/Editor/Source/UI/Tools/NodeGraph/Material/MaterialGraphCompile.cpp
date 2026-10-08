#include "MaterialGraphCompile.h"
#include "Containers/StringFormat.h"
#include "MaterialNodeGraph.h"
#include "MaterialFunctionGraph.h"
#include "Nodes/MaterialNode_Grass.h"
#include "Nodes/MaterialNode_Function.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/MaterialFunction/MaterialFunction.h"
#include "Assets/AssetManager/AssetManager.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Core/CoreEditorDelegates.h"
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
        void RefreshFunctionCallPins(CMaterialNodeGraph* Graph, THashSet<FGuid>& VisitedFunctions)
        {
            for (const TStrongObjectPtr<CEdGraphNode>& Node : Graph->Nodes)
            {
                CMaterialExpression_MaterialFunctionCall* Call = Cast<CMaterialExpression_MaterialFunctionCall>(Node.Get());
                if (Call == nullptr)
                {
                    continue;
                }

                if (Call->NeedsPinRebuild())
                {
                    Call->RebuildPins();
                    if (CPackage* Package = Graph->GetPackage())
                    {
                        Package->MarkDirty();
                    }
                }

                CMaterialFunction* Function = Call->Function.Get();
                if (Function == nullptr || !VisitedFunctions.insert(Function->GetGUID()).second)
                {
                    continue;
                }

                CPackage* FunctionPackage = Function->GetPackage();
                CMaterialNodeGraph* FunctionGraph = FunctionPackage != nullptr
                    ? Cast<CMaterialNodeGraph>(FunctionPackage->LoadObjectByName(FName(GMaterialFunctionGraphObjectName)))
                    : nullptr;
                if (FunctionGraph != nullptr)
                {
                    RefreshFunctionCallPins(FunctionGraph, VisitedFunctions);
                }
            }
        }

        // GrassOutput nodes sit outside the emit closure, so the only way their data reaches the asset is
        // a direct scan of the graph. Assign rather than merge: a removed node must drop its species.
        void CollectGrassOutputs(CMaterial* Material, CMaterialNodeGraph* Graph)
        {
            Material->GrassOutputs.clear();

            if (Graph == nullptr)
            {
                return;
            }

            for (const TStrongObjectPtr<CEdGraphNode>& Node : Graph->Nodes)
            {
                if (!Node.IsValid())
                {
                    continue;
                }

                CMaterialExpression_GrassOutput* GrassNode = Cast<CMaterialExpression_GrassOutput>(Node.Get());
                if (GrassNode == nullptr)
                {
                    continue;
                }

                for (const FGrassOutputEntry& Entry : GrassNode->Entries)
                {
                    if (Entry.GrassType == nullptr)
                    {
                        continue;
                    }

                    FGrassOutput Output;
                    Output.GrassType    = Entry.GrassType;
                    Output.LayerIndex   = Entry.LayerIndex;
                    Output.DensityScale = Entry.DensityScale;
                    Material->GrassOutputs.push_back(Output);
                }
            }
        }

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
            case EMaterialShaderStage::MeshShadowMasked:     return "Masked Shadow Geometry";
            case EMaterialShaderStage::ShadowMaskedPixel:    return "Masked Shadow Pixel";
            default:                                         return "Unknown";
            }
        }

        struct FMaskedInterpolants
        {
            bool bUV1        = false;
            bool bColor      = false;
            bool bNormal     = false;
            bool bTangent    = false;
            bool bViewPos    = false;
            bool bWorldNoWPO = false;
            bool bInstance   = false;
            bool bFull       = false;
        };

        bool IsIdentifierChar(char C, bool bFirst)
        {
            return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_' || (!bFirst && C >= '0' && C <= '9');
        }

        // Conservative, since the whole pixel graph is scanned and not only the opacity chain feeding the masked lanes.
        FMaskedInterpolants ScanMaskedInterpolants(const FString& PixelGraph)
        {
            FMaskedInterpolants IO;
            const FStringView Source = PixelGraph.View();
            size_t i = 0;

            auto NextIdentifier = [&](size_t& Pos) -> FStringView
            {
                while (Pos < Source.size() && Source[Pos] == ' ')
                {
                    ++Pos;
                }
                const size_t Begin = Pos;
                while (Pos < Source.size() && IsIdentifierChar(Source[Pos], Pos == Begin))
                {
                    ++Pos;
                }
                return FStringView(Source.data() + Begin, Pos - Begin);
            };

            while (i < Source.size())
            {
                if (!IsIdentifierChar(Source[i], true) || (i > 0 && IsIdentifierChar(Source[i - 1], false)))
                {
                    ++i;
                    continue;
                }

                const FStringView Token = NextIdentifier(i);
                if (Token == "Input")
                {
                    size_t Pos = i;
                    while (Pos < Source.size() && Source[Pos] == ' ')
                    {
                        ++Pos;
                    }
                    if (Pos >= Source.size() || Source[Pos] != '.')
                    {
                        IO.bFull = true;
                        continue;
                    }
                    ++Pos;
                    const FStringView Member = NextIdentifier(Pos);
                    i = Pos;
                    if (Member == "InstanceIndex")
                    {
                        IO.bInstance = true;
                    }
                    else if (Member != "Position" && Member != "UV" && Member != "MaterialIndex")
                    {
                        IO.bFull = true;
                    }
                }
                else if (Token == "UV1" || Token == "UV1_DDX" || Token == "UV1_DDY")
                {
                    IO.bUV1 = true;
                }
                else if (Token == "VertexColor" || Token == "VertexColor_DDX" || Token == "VertexColor_DDY")
                {
                    IO.bColor = true;
                }
                else if (Token == "WorldNormal")
                {
                    IO.bNormal = true;
                }
                else if (Token == "WorldTangent")
                {
                    IO.bTangent = true;
                }
                else if (Token == "WorldPositionExcludingOffsets" || Token == "WorldPositionExcludingOffsets_DDX"
                      || Token == "WorldPositionExcludingOffsets_DDY")
                {
                    IO.bWorldNoWPO = true;
                }
                else if (Token == "WorldPosition" || Token == "WorldPosition_DDX" || Token == "WorldPosition_DDY"
                      || Token == "ViewPosition")
                {
                    IO.bViewPos = true;
                }
                else if (Token == "EntityID" || Token == "bReceiveShadow")
                {
                    IO.bInstance = true;
                }
            }
            return IO;
        }

        // The camera lane rebuilds view position from the fragment, so only the shadow lane ever carries it.
        void AddMaskedInterpolantDefines(const FMaskedInterpolants& IO, bool bShadowLane, TVector<FString>& Out)
        {
            if (IO.bFull)
            {
                Out.emplace_back("MASKED_IO_FULL");
                for (const char* Define : { "MASKED_IO_UV1", "MASKED_IO_COLOR", "MASKED_IO_NORMAL", "MASKED_IO_TANGENT",
                                            "MASKED_IO_VIEW_POS", "MASKED_IO_WORLD_NO_WPO", "MASKED_IO_INSTANCE" })
                {
                    Out.emplace_back(Define);
                }
                return;
            }

            if (IO.bUV1)                    { Out.emplace_back("MASKED_IO_UV1"); }
            if (IO.bColor)                  { Out.emplace_back("MASKED_IO_COLOR"); }
            if (IO.bNormal)                 { Out.emplace_back("MASKED_IO_NORMAL"); }
            if (IO.bTangent)                { Out.emplace_back("MASKED_IO_TANGENT"); }
            if (IO.bViewPos && bShadowLane) { Out.emplace_back("MASKED_IO_VIEW_POS"); }
            if (IO.bWorldNoWPO)             { Out.emplace_back("MASKED_IO_WORLD_NO_WPO"); }
            if (IO.bInstance)               { Out.emplace_back("MASKED_IO_INSTANCE"); }
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
            TVector<TStrongObjectPtr<CTexture>> Resolved;
            Resolved.reserve(Material->Textures.size());
            for (uint32 i = 0; i < (uint32)Material->Textures.size(); ++i)
            {
                Material->ResolveTextureSlot(i);
                Resolved.push_back(i < (uint32)Material->ResolvedTextures.size() ? Material->ResolvedTextures[i] : nullptr);
            }
            Compiler.SeedManifest(Material->Parameters, *Material->GetMaterialUniforms(), Resolved,
                Material->ParameterCollections);

            // Rebuilt from nothing, so a stage the new graph no longer emits cannot linger.
            Material->ClearPermutation(Target.Key);
        }

        THashSet<FGuid> VisitedFunctions;
        RefreshFunctionCallPins(Graph, VisitedFunctions);
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
        if (Material->IsOITResolved())
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
        Result.StageLog = MakeShared<FMaterialStageCommitLog>();
        auto CommitStage = [Material, bPermutation, TargetKey, TargetGen, Log = Result.StageLog](EMaterialShaderStage Stage)
        {
            Log->Dispatched.fetch_or(1u << (uint32)Stage, std::memory_order_relaxed);
            return [Material, Stage, bPermutation, TargetKey, TargetGen, Log](const FShaderHeader& Header)
            {
                Log->Committed.fetch_or(1u << (uint32)Stage, std::memory_order_release);
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
                // Masked geometry exports what its pixel graph reads, and each lane's two stages must agree on that set.
                const FMaskedInterpolants MaskedIO = ScanMaskedInterpolants(Compiler.GetPixelGraphSource());

                FShaderCompileOptions VisMaskedOptions; VisMaskedOptions.DebugName = MatName + " [VBMM]";
                VisMaskedOptions.MacroDefinitions.emplace_back("VISBUFFER_MASKED_GEOM");
                AddMaskedInterpolantDefines(MaskedIO, false, VisMaskedOptions.MacroDefinitions);
                ShaderCompiler->CompilerShaderRaw(VisSource, Move(VisMaskedOptions), CommitStage(EMaterialShaderStage::VisBufferMeshMasked));

                const FString MaskedPSSource = Compiler.BuildPixelShaderFromTemplate(MeshShaderDir + "VisBufferMaskedPixel.slang");
                FShaderCompileOptions MaskedPSOptions; MaskedPSOptions.DebugName = MatName + " [MVBP]";
                MaskedPSOptions.MacroDefinitions.emplace_back("VISBUFFER_PRIMID");
                AddMaskedInterpolantDefines(MaskedIO, false, MaskedPSOptions.MacroDefinitions);
                ShaderCompiler->CompilerShaderRaw(MaskedPSSource, Move(MaskedPSOptions), CommitStage(EMaterialShaderStage::MaskedVisBufferPixel));

                // The shadow lane too, so a cut-out casts its own silhouette and not its quad.
                FShaderCompileOptions ShadowMaskedOptions; ShadowMaskedOptions.DebugName = MatName + " [MSSM]";
                ShadowMaskedOptions.MacroDefinitions.emplace_back("MESHLET_MESH_MASKED_SHADOW");
                AddMaskedInterpolantDefines(MaskedIO, true, ShadowMaskedOptions.MacroDefinitions);
                ShaderCompiler->CompilerShaderRaw(MeshSource, Move(ShadowMaskedOptions), CommitStage(EMaterialShaderStage::MeshShadowMasked));

                const FString ShadowPSSource = Compiler.BuildPixelShaderFromTemplate(MeshShaderDir + "ShadowMaskedPixel.slang");
                FShaderCompileOptions ShadowPSOptions; ShadowPSOptions.DebugName = MatName + " [SMP]";
                AddMaskedInterpolantDefines(MaskedIO, true, ShadowPSOptions.MacroDefinitions);
                ShaderCompiler->CompilerShaderRaw(ShadowPSSource, Move(ShadowPSOptions), CommitStage(EMaterialShaderStage::ShadowMaskedPixel));
            }

            const FString DeferredSource = Compiler.BuildDeferredShaderFromTemplate(MeshShaderDir + "DeferredMaterial.slang", EMaterialType::PBR);
            FShaderCompileOptions DeferredOptions; DeferredOptions.DebugName = MatName + " [DM]";

            // The runtime model check lives in ShadeGBuffer, so forward and deferred cannot drift apart.
            ShaderCompiler->CompilerShaderRaw(DeferredSource, Move(DeferredOptions), CommitStage(EMaterialShaderStage::Deferred));
        }

        ShaderCompiler->CompilerShaderRaw(Result.PixelSource, Move(Options), CommitStage(EMaterialShaderStage::Pixel));


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

        const uint32 Dispatched = Result.StageLog ? Result.StageLog->Dispatched.load(std::memory_order_acquire) : 0u;
        const uint32 Committed  = Result.StageLog ? Result.StageLog->Committed.load(std::memory_order_acquire) : 0u;

        // Checked against the permutation's own bytecode, since GetStageForKey would fall back and pass.
        auto StageEmpty = [&](EMaterialShaderStage Stage) -> bool
        {
            const uint32 Bit = 1u << (uint32)Stage;
            const TVector<uint32>& Binaries = Target.bPermutation
                                            ? Material->GetPermutationStageBinaries(Target.Key, Stage)
                                            : Material->GetShaderStageBinaries(Stage);

            // A stage this compile sent out that never came back failed, even with the previous compile's bytecode still loaded.
            const bool bFailedThisCompile = (Dispatched & Bit) != 0u && (Committed & Bit) == 0u;
            if (!Binaries.empty() && !bFailedThisCompile)
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

            TVector<TStrongObjectPtr<CTexture>> BoundTextures;
            Compiler.GetBoundTextures(BoundTextures);

            Material->Textures.clear();
            Material->Textures.reserve(BoundTextures.size());
            Material->ResolvedTextures.clear();
            Material->ResolvedTextures.reserve(BoundTextures.size());

            for (const TStrongObjectPtr<CTexture>& Texture : BoundTextures)
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

        Memory::Memzero(Material->GetMaterialUniforms(), sizeof(FMaterialUniforms));
        Material->Parameters.clear();
        Compiler.GetParameters(Material->Parameters, *Material->GetMaterialUniforms());

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
            CollectGrassOutputs(Material, Graph);
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
        struct FPendingMaterialRecompile
        {
            // The per-stage commit callbacks capture the material RAW, so this ref has to outlive the wait.
            TStrongObjectPtr<CMaterial>   Material;
            // Finish reads bound textures and parameters back off it, so it outlives the dispatch.
            TUniquePtr<FMaterialCompiler> Compiler;
            FMaterialGraphCompileResult   Result;
            TStrongObjectPtr<CMaterialNodeGraph> Graph;
            uint64                        GraphContentVersion = 0;
            bool                          bFunctionChange = false;
            bool                          bSaveWhenComplete = false;
        };

        FPendingMaterialRecompile GPendingMaterialRecompile;
        TVector<FGuid> GFunctionRecompileQueue;
        THashSet<FGuid> GQueuedFunctionMaterials;

        struct FPendingPermutationCompile
        {
            TStrongObjectPtr<CMaterial>   Material;
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

    void QueueMaterialFunctionRecompiles(const FGuid& FunctionGUID)
    {
        FAssetRegistry& Registry = FAssetRegistry::Get();
        const FName MaterialClass = CMaterial::StaticClass()->GetName();
        const FName FunctionClass = CMaterialFunction::StaticClass()->GetName();

        TVector<FGuid> Functions;
        THashSet<FGuid> SeenFunctions;
        Functions.push_back(FunctionGUID);
        SeenFunctions.insert(FunctionGUID);

        for (size_t Index = 0; Index < Functions.size(); ++Index)
        {
            for (const FAssetData* Referencer : Registry.GetReferencersOf(Functions[Index]))
            {
                if (Referencer->AssetClass == FunctionClass)
                {
                    if (SeenFunctions.insert(Referencer->AssetGUID).second)
                    {
                        Functions.push_back(Referencer->AssetGUID);
                    }
                }
                else if (Referencer->AssetClass == MaterialClass &&
                         GQueuedFunctionMaterials.insert(Referencer->AssetGUID).second)
                {
                    GFunctionRecompileQueue.push_back(Referencer->AssetGUID);
                }
            }
        }
    }

    bool RecompileMaterialIfStale(CMaterial* Material, FString& OutError)
    {
        if (Material == nullptr || Material->CompiledTemplateHash == CMaterial::GetShaderTemplateHash())
        {
            return true;
        }

        CMaterialNodeGraph* Graph = LoadMaterialGraph(Material);
        if (Graph == nullptr)
        {
            OutError = "it has no saved graph to recompile from";
            return false;
        }

        const FMaterialGraphCompileResult Result = CompileMaterialGraph(Material, Graph);
        if (!Result.bSuccess)
        {
            OutError = Lumina::Format("its graph failed to compile with {} error(s)", Result.Errors.size());
            return false;
        }
        return true;
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

        TStrongObjectPtr<CMaterial> Material;
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

    void ProcessMaterialRecompiles()
    {
        // One material at a time keeps the swarm's work bounded and the poll below unambiguous.
        if (GPendingMaterialRecompile.Compiler != nullptr)
        {
            // Global across every compile, so an unrelated burst delays this finish but never commits wrong.
            if (GShaderCompiler->HasPendingRequests())
            {
                return;
            }

            CMaterial* Material = GPendingMaterialRecompile.Material.Get();
            FinishMaterialGraphCompile(Material, *GPendingMaterialRecompile.Compiler, GPendingMaterialRecompile.Result);
            const bool bGraphUnchanged = GPendingMaterialRecompile.Graph != nullptr &&
                GPendingMaterialRecompile.Graph->GetTreeContentVersion() == GPendingMaterialRecompile.GraphContentVersion;
            if (GPendingMaterialRecompile.bFunctionChange && bGraphUnchanged)
            {
                GPendingMaterialRecompile.Graph->MarkCompiled();
            }

            // Read before the reset, since the material may be the only thing still naming the package.
            CPackage* Package = (Material != nullptr) ? Material->GetPackage() : nullptr;
            const FString Name = (Material != nullptr) ? FString(Material->GetName().c_str()) : FString("<destroyed>");
            const bool bSuccess = GPendingMaterialRecompile.Result.bSuccess;
            const bool bFunctionChange = GPendingMaterialRecompile.bFunctionChange;
            const bool bSaveWhenComplete = GPendingMaterialRecompile.bSaveWhenComplete;
            const bool bSuperseded = Material != nullptr && GQueuedFunctionMaterials.contains(Material->GetGUID());

            GPendingMaterialRecompile = {};

            if (bSuccess && Package != nullptr)
            {
                Package->MarkDirty();
                if (bSaveWhenComplete && bGraphUnchanged && !bSuperseded)
                {
                    FCoreEditorDelegates::OnAssetPreSave.Broadcast(Material);
                    if (CPackage::SavePackage(Package, Package->GetPackagePath()))
                    {
                        FAssetRegistry::Get().AssetSaved(Material);
                        FCoreEditorDelegates::OnAssetSaved.Broadcast(Material);
                        LOG_INFO("Material '{0}' recompiled and saved after a material function changed", Name.c_str());
                    }
                    else
                    {
                        ImGuiX::Notifications::NotifyError("Material '{0}' recompiled but could not be saved", Name.c_str());
                    }
                }
                else if (bFunctionChange)
                {
                    ImGuiX::Notifications::NotifyInfo("Material '{0}' recompiled after a material function changed; save to keep", Name.c_str());
                }
                else
                {
                    ImGuiX::Notifications::NotifyInfo("Material '{0}' recompiled (shader templates changed) - save to keep", Name.c_str());
                }
            }
            else
            {
                ImGuiX::Notifications::NotifyError("Material '{0}' failed to recompile", Name.c_str());
            }
            return;
        }

        const bool bFunctionChange = !GFunctionRecompileQueue.empty();
        TStrongObjectPtr<CMaterial> Material;
        if (bFunctionChange)
        {
            const FGuid MaterialGUID = GFunctionRecompileQueue.back();
            GFunctionRecompileQueue.pop_back();
            GQueuedFunctionMaterials.erase(MaterialGUID);
            const FAssetData* Data = FAssetRegistry::Get().GetAssetByGUID(MaterialGUID);
            if (Data != nullptr)
            {
                Material = Cast<CMaterial>(FAssetManager::Get().LoadAssetSynchronous(Data->Path, MaterialGUID));
            }
        }
        else
        {
            Material = CMaterial::PopStaleTemplateMaterial();
        }

        if (!Material.IsValid() || (!bFunctionChange && Material->CompiledTemplateHash == CMaterial::GetShaderTemplateHash()))
        {
            return;
        }

        CMaterialNodeGraph* Graph = LoadMaterialGraph(Material.Get());
        if (Graph == nullptr)
        {
            LOG_WARN("Material '{0}' has no saved graph to recompile from", Material->GetName().c_str());
            return;
        }

        const bool bSaveWhenComplete = bFunctionChange && !Material->GetPackage()->IsDirty() && !Graph->NeedsCompile();

        TUniquePtr<FMaterialCompiler> Compiler = MakeUnique<FMaterialCompiler>();
        FMaterialGraphCompileResult   Result;

        // A graph that failed up front has nothing to wait for and would hold the slot until unrelated work.
        if (!BeginMaterialGraphCompile(Material.Get(), Graph, *Compiler, Result))
        {
            if (bFunctionChange)
            {
                Graph->MarkCompiled();
            }
            ImGuiX::Notifications::NotifyError("Material '{0}' failed to recompile", Material->GetName().c_str());
            return;
        }

        GPendingMaterialRecompile.Material        = Material;
        GPendingMaterialRecompile.Compiler        = Move(Compiler);
        GPendingMaterialRecompile.Result          = Move(Result);
        GPendingMaterialRecompile.Graph           = Graph;
        GPendingMaterialRecompile.GraphContentVersion = Graph->GetTreeContentVersion();
        GPendingMaterialRecompile.bFunctionChange = bFunctionChange;
        GPendingMaterialRecompile.bSaveWhenComplete = bSaveWhenComplete;
    }
}
