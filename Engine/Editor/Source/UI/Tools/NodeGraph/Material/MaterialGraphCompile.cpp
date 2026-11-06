#include "MaterialGraphCompile.h"
#include "Containers/StringFormat.h"
#include "MaterialNodeGraph.h"
#include "MaterialFunctionGraph.h"
#include "Nodes/MaterialNode_Grass.h"
#include "Nodes/MaterialNode_Function.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/MaterialFunction/MaterialFunction.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Core/Math/Hash/Hash.h"
#include "Memory/Memory.h"
#include "Paths/Paths.h"
#include "Renderer/MaterialTypes.h"
#include "Renderer/RenderManager.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"
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
                if (Function == nullptr || !VisitedFunctions.TryAdd(Function->GetGUID()))
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

        // GrassOutput nodes sit outside the emit closure, so they are scanned directly; assign rather than merge so a removed node drops its species.
        void CollectGrassOutputs(CMaterial* Material, CMaterialNodeGraph* Graph)
        {
            Material->GrassOutputs.Reset();

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

                for (const SGrassOutputEntry& Entry : GrassNode->Entries)
                {
                    if (Entry.GrassType == nullptr)
                    {
                        continue;
                    }

                    SGrassOutput Output;
                    Output.GrassType    = Entry.GrassType;
                    Output.LayerIndex   = Entry.LayerIndex;
                    Output.DensityScale = Entry.DensityScale;
                    Material->GrassOutputs.Add(Output);
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
            case EMaterialShaderStage::MeshShadowVertex:           return "Shadow Geometry (vertex path)";
            case EMaterialShaderStage::MeshBaseVertex:             return "Base Geometry (vertex path)";
            case EMaterialShaderStage::VisBufferVertex:            return "VisBuffer Geometry (vertex path)";
            case EMaterialShaderStage::VisBufferVertexMasked:      return "Masked VisBuffer Geometry (vertex path)";
            case EMaterialShaderStage::MeshShadowMaskedVertex:     return "Masked Shadow Geometry (vertex path)";
            case EMaterialShaderStage::MaskedVisBufferPixelVertex: return "Masked VisBuffer Pixel (vertex path)";
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
            int32 i = 0;

            auto NextIdentifier = [&](int32& Pos) -> FStringView
            {
                while (Pos < Source.Len() && Source[Pos] == ' ')
                {
                    ++Pos;
                }
                const int32 Begin = Pos;
                while (Pos < Source.Len() && IsIdentifierChar(Source[Pos], Pos == Begin))
                {
                    ++Pos;
                }
                return FStringView(Source.GetData() + Begin, Pos - Begin);
            };

            while (i < Source.Len())
            {
                if (!IsIdentifierChar(Source[i], true) || (i > 0 && IsIdentifierChar(Source[i - 1], false)))
                {
                    ++i;
                    continue;
                }

                const FStringView Token = NextIdentifier(i);
                if (Token == "Input")
                {
                    int32 Pos = i;
                    while (Pos < Source.Len() && Source[Pos] == ' ')
                    {
                        ++Pos;
                    }
                    if (Pos >= Source.Len() || Source[Pos] != '.')
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
                Out.Emplace("MASKED_IO_FULL");
                for (const char* Define : { "MASKED_IO_UV1", "MASKED_IO_COLOR", "MASKED_IO_NORMAL", "MASKED_IO_TANGENT",
                                            "MASKED_IO_VIEW_POS", "MASKED_IO_WORLD_NO_WPO", "MASKED_IO_INSTANCE" })
                {
                    Out.Emplace(Define);
                }
                return;
            }

            if (IO.bUV1)                    { Out.Emplace("MASKED_IO_UV1"); }
            if (IO.bColor)                  { Out.Emplace("MASKED_IO_COLOR"); }
            if (IO.bNormal)                 { Out.Emplace("MASKED_IO_NORMAL"); }
            if (IO.bTangent)                { Out.Emplace("MASKED_IO_TANGENT"); }
            if (IO.bViewPos && bShadowLane) { Out.Emplace("MASKED_IO_VIEW_POS"); }
            if (IO.bWorldNoWPO)             { Out.Emplace("MASKED_IO_WORLD_NO_WPO"); }
            if (IO.bInstance)               { Out.Emplace("MASKED_IO_INSTANCE"); }
        }
    }

    bool MakeMaterialPermutationTarget(const CMaterial* Material, uint64 Key, FMaterialCompileTarget& OutTarget)
    {
        if (Material == nullptr || Material->StaticSwitches.IsEmpty())
        {
            return false;
        }

        OutTarget.bPermutation = true;
        OutTarget.Key          = Key;
        OutTarget.Generation   = Material->GetPermutationGeneration();
        OutTarget.StaticSwitchOverrides.Reset();

        for (const SMaterialStaticSwitch& Switch : Material->StaticSwitches)
        {
            OutTarget.StaticSwitchOverrides[Switch.ParameterName] = (Key & (1ull << Switch.BitIndex)) != 0;
        }

        return true;
    }

    bool BeginMaterialGraphCompile(CMaterial* Material, CMaterialNodeGraph* Graph, FMaterialCompiler& Compiler,
        FMaterialGraphCompileResult& Result, const FMaterialCompileTarget& Target, Jobs::EJobPriority Priority)
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
            Resolved.Reserve(Material->Textures.Num());
            for (int32 i = 0; i < Material->Textures.Num(); ++i)
            {
                Material->ResolveTextureSlot(i);
                Resolved.Add(i < Material->ResolvedTextures.Num() ? Material->ResolvedTextures[i] : nullptr);
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
            if (!Target.bPermutation)
            {
                Material->SetCompileFailed(true);
            }
            return false;
        }

        // BuildShaders yields both the pixel and vertex source with the $MATERIAL_INPUTS tokens substituted.
        Compiler.BuildShaders(Result.PixelSource, Result.VertexSource, Material->GetMaterialType());
        Result.Stats = Compiler.GetStats();

        // The template text is left out on purpose, since the artifact key adds the template hash and an engine shader edit must not change the asset.
        if (!Target.bPermutation)
        {
            uint32 RequiredStages = 0;
            for (size_t s = 0; s < (size_t)EMaterialShaderStage::Count; ++s)
            {
                RequiredStages |= IsStageRequired(Material, (EMaterialShaderStage)s) ? (1u << s) : 0u;
            }

            const uint64 Settings[] = { (uint64)Material->GetMaterialType(), (uint64)Material->GetBlendMode(),
                                        Material->IsOITResolved() ? 1ull : 0ull, (uint64)RequiredStages };
            uint64 Hash = Hash::GetHash64(Settings, sizeof(Settings), 0x534F5552434548ull);
            Hash = Hash::GetHash64(Compiler.GetPixelGraphSource(), Hash);
            Hash = Hash::GetHash64(Compiler.BuildVertexStageBody(Material->GetMaterialType()), Hash);
            Result.SourceHash = Hash != 0 ? Hash : 1;
        }

        IShaderCompiler* ShaderCompiler = GShaderCompiler;

        // Crash-dump-friendly shader names, the material name plus its stage.
        const FString MatName = *Material->GetName();

        FShaderCompileOptions Options;
        Options.DebugName = MatName + " [PS]";
        if (Material->IsOITResolved())
        {
            Options.MacroDefinitions.Emplace("TRANSLUCENT");
        }
        if (Material->GetBlendMode() == EBlendMode::AlphaComposite)
        {
            Options.MacroDefinitions.Emplace("ALPHA_COMPOSITE");
        }
        if (Material->GetBlendMode() == EBlendMode::Modulate)
        {
            Options.MacroDefinitions.Emplace("MODULATE");
        }
        if (Material->GetBlendMode() == EBlendMode::Masked)
        {
            Options.MacroDefinitions.Emplace("MASKED");
        }
        // Every model reaches the shader through runtime flags, so an instance can override back to Lit.

        FShaderCompileOptions VSOptions;
        VSOptions.DebugName = MatName + " [VS]";

        // CommitShaderStage owns the blob store and library commit, so this file repeats no suffixes.
        const bool   bPermutation = Target.bPermutation;
        const uint64 TargetKey    = Target.Key;
        const uint32 TargetGen    = Target.Generation;
        Result.StageLog = MakeShared<FMaterialStageCommitLog>();
        auto DispatchStage = [ShaderCompiler, Material, bPermutation, TargetKey, TargetGen, Priority, Log = Result.StageLog]
            (const FString& Source, FShaderCompileOptions&& StageOptions, EMaterialShaderStage Stage)
        {
            StageOptions.Priority = Priority;
            Log->Dispatched.fetch_or(1u << (uint32)Stage, std::memory_order_relaxed);

            auto Commit = [Material, Stage, bPermutation, TargetKey, TargetGen, Log](const FShaderHeader& Header)
            {
                const TSpan<const uint32> Spirv(Header.Binaries.GetData(), Header.Binaries.Num());
                if (bPermutation)
                {
                    Material->CommitPermutationStageIfCurrent(TargetKey, TargetGen, Stage, Spirv);
                }
                else
                {
                    Material->CommitShaderStage(Stage, Spirv);
                }
                Log->Committed.fetch_or(1u << (uint32)Stage, std::memory_order_release);
            };
            ShaderCompiler->CompilerShaderRaw(Source, Move(StageOptions), Move(Commit), Log->Stages.Track());
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
            DispatchStage(Result.VertexSource, Move(VSOptions), EMaterialShaderStage::Vertex);
        }

        // A geometry template carries both paths, MeshMain for Stage and VertexMain for its vertex-path twin.
        auto DispatchGeometryStage = [&DispatchStage](const FString& Source, const FShaderCompileOptions& StageOptions, EMaterialShaderStage Stage)
        {
            FShaderCompileOptions VertexOptions = StageOptions;
            VertexOptions.EntryPoint = "VertexMain";
            VertexOptions.DebugName += " [vertex path]";
            DispatchStage(Source, Move(VertexOptions), VertexPathStage(Stage));

            FShaderCompileOptions MeshOptions = StageOptions;
            MeshOptions.EntryPoint = "MeshMain";
            DispatchStage(Source, Move(MeshOptions), Stage);
        };

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
                    CompileOptions.MacroDefinitions.Emplace(Geo.Define);
                }
                DispatchGeometryStage(*Geo.Source, CompileOptions, Geo.Stage);
            }

            if (IsStageRequired(Material, EMaterialShaderStage::VisBufferMeshMasked))
            {
                // Masked geometry exports what its pixel graph reads, and each lane's two stages must agree on that set.
                const FMaskedInterpolants MaskedIO = ScanMaskedInterpolants(Compiler.GetPixelGraphSource());

                FShaderCompileOptions VisMaskedOptions; VisMaskedOptions.DebugName = MatName + " [VBMM]";
                VisMaskedOptions.MacroDefinitions.Emplace("VISBUFFER_MASKED_GEOM");
                AddMaskedInterpolantDefines(MaskedIO, false, VisMaskedOptions.MacroDefinitions);
                DispatchGeometryStage(VisSource, VisMaskedOptions, EMaterialShaderStage::VisBufferMeshMasked);

                const FString MaskedPSSource = Compiler.BuildPixelShaderFromTemplate(MeshShaderDir + "VisBufferMaskedPixel.slang");
                FShaderCompileOptions MaskedPSOptions; MaskedPSOptions.DebugName = MatName + " [MVBP]";
                AddMaskedInterpolantDefines(MaskedIO, false, MaskedPSOptions.MacroDefinitions);

                // The vertex path hands VisID over as a varying, since it cannot write a primitive ID.
                FShaderCompileOptions MaskedPSVertexOptions = MaskedPSOptions;
                MaskedPSVertexOptions.DebugName += " [vertex path]";
                MaskedPSVertexOptions.MacroDefinitions.Emplace("MESHLET_VERTEX_PATH");
                DispatchStage(MaskedPSSource, Move(MaskedPSVertexOptions), EMaterialShaderStage::MaskedVisBufferPixelVertex);
                DispatchStage(MaskedPSSource, Move(MaskedPSOptions), EMaterialShaderStage::MaskedVisBufferPixel);

                // The shadow lane too, so a cut-out casts its own silhouette and not its quad.
                FShaderCompileOptions ShadowMaskedOptions; ShadowMaskedOptions.DebugName = MatName + " [MSSM]";
                ShadowMaskedOptions.MacroDefinitions.Emplace("MESHLET_MESH_MASKED_SHADOW");
                AddMaskedInterpolantDefines(MaskedIO, true, ShadowMaskedOptions.MacroDefinitions);
                DispatchGeometryStage(MeshSource, ShadowMaskedOptions, EMaterialShaderStage::MeshShadowMasked);

                const FString ShadowPSSource = Compiler.BuildPixelShaderFromTemplate(MeshShaderDir + "ShadowMaskedPixel.slang");
                FShaderCompileOptions ShadowPSOptions; ShadowPSOptions.DebugName = MatName + " [SMP]";
                AddMaskedInterpolantDefines(MaskedIO, true, ShadowPSOptions.MacroDefinitions);
                DispatchStage(ShadowPSSource, Move(ShadowPSOptions), EMaterialShaderStage::ShadowMaskedPixel);
            }

            const FString DeferredSource = Compiler.BuildDeferredShaderFromTemplate(MeshShaderDir + "DeferredMaterial.slang", EMaterialType::PBR);
            FShaderCompileOptions DeferredOptions; DeferredOptions.DebugName = MatName + " [DM]";

            // The runtime model check lives in ShadeGBuffer, so forward and deferred cannot drift apart.
            DispatchStage(DeferredSource, Move(DeferredOptions), EMaterialShaderStage::Deferred);
        }

        DispatchStage(Result.PixelSource, Move(Options), EMaterialShaderStage::Pixel);


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
            if (!Binaries.IsEmpty() && !bFailedThisCompile)
            {
                return false;
            }
            EdNodeGraph::FError Error;
            Error.Name        = "Shader Stage Failed";
            Error.Description  = FString("The ") + StageDisplayName(Stage) + " shader stage produced no output (compile failed).";
            Result.Errors.Add(Error);
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
            else
            {
                Material->SetCompileFailed(true);
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

            Material->Textures.Reset();
            Material->Textures.Reserve(BoundTextures.Num());
            Material->ResolvedTextures.Reset();
            Material->ResolvedTextures.Reserve(BoundTextures.Num());

            for (const TStrongObjectPtr<CTexture>& Texture : BoundTextures)
            {
                if (Texture == nullptr)
                {
                    Material->Textures.Emplace();
                    Material->ResolvedTextures.Emplace();
                    continue;
                }

                const FGuid Guid = Texture->GetGUID();

                // An empty path is unrecoverable, since the soft ref looks valid but resolves magenta forever.
                FStringView Path;
                if (const FAssetData* Data = FAssetRegistry::Get().GetAssetByGUID(Guid))
                {
                    Path = FStringView(*Data->Path, Data->Path.Len());
                }

                FFixedString PackagePath;
                if (Path.IsEmpty() && Texture->GetPackage() != nullptr)
                {
                    PackagePath = Texture->GetPackage()->GetPackagePath();
                    Path = FStringView(*PackagePath, PackagePath.Len());
                }

                if (Path.IsEmpty())
                {
                    LOG_WARN("Material '{}': texture '{}' has no resolvable asset path; its slot will "
                             "fall back to the placeholder on any future load.",
                             Material->GetName(), Texture->GetName());
                }

                Material->Textures.Emplace(FSoftObjectPath(Path, Guid));

                // Round-tripping an already-resident texture through the asset manager rendered it magenta.
                Material->ResolvedTextures.Emplace(Texture);
            }
        }

        // Slot order is what the shader compiled against, so this is an assign, never a merge.
        Compiler.GetBoundCollections(Material->ParameterCollections);

        Memory::Memzero(Material->GetMaterialUniforms(), sizeof(FMaterialUniforms));
        Material->Parameters.Reset();
        Compiler.GetParameters(Material->Parameters, *Material->GetMaterialUniforms());

        // A permutation never reaches a switch nested under a dropped branch, so it must not renumber.
        if (!Target.bPermutation)
        {
            Compiler.GetStaticSwitches(Material->StaticSwitches);
        }

        // Stamped before PostLoad, which compares it and queues stale materials for auto-recompile.
        Material->CompiledTemplateHash = CMaterial::GetShaderTemplateHash();
        if (!Target.bPermutation)
        {
            Material->SourceHash = Result.SourceHash;
        }

        Material->PostLoad();

        // Published so the next load, on this machine or any sharing the cache, skips the compile.
        if (Target.bPermutation)
        {
            Material->StorePermutationInArtifactCache(Target.Key);
        }
        else
        {
            Material->StoreShadersInArtifactCache();
        }

        Result.bSuccess = true;
    }

    FMaterialGraphCompileResult CompileMaterialGraph(CMaterial* Material, CMaterialNodeGraph* Graph)
    {
        FMaterialGraphCompileResult Result;
        FMaterialCompiler Compiler;

        if (BeginMaterialGraphCompile(Material, Graph, Compiler, Result))
        {
            Result.StageLog->Stages.Wait();
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

    void QueueMaterialFunctionRecompiles(const FGuid& FunctionGUID)
    {
        FRenderManager* Renderer = TryRender();
        if (Renderer == nullptr)
        {
            return;
        }

        FAssetRegistry& Registry = FAssetRegistry::Get();
        const FName MaterialClass = CMaterial::StaticClass()->GetName();
        const FName FunctionClass = CMaterialFunction::StaticClass()->GetName();

        TVector<FGuid> Functions;
        THashSet<FGuid> SeenFunctions;
        Functions.Add(FunctionGUID);
        SeenFunctions.Add(FunctionGUID);

        for (int32 Index = 0; Index < Functions.Num(); ++Index)
        {
            for (const FAssetData* Referencer : Registry.GetReferencersOf(Functions[Index]))
            {
                if (Referencer->AssetClass == FunctionClass)
                {
                    if (SeenFunctions.TryAdd(Referencer->AssetGUID))
                    {
                        Functions.Add(Referencer->AssetGUID);
                    }
                }
                else if (Referencer->AssetClass == MaterialClass)
                {
                    Renderer->GetMaterialCompileQueue().QueueFunctionRecompile(Referencer->AssetGUID);
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
            OutError = Lumina::Format("its graph failed to compile with {} error(s)", Result.Errors.Num());
            return false;
        }
        return true;
    }

    uint32 EnsureMaterialPermutations(CMaterial* Material, TSpan<const uint64> Keys)
    {
        if (Material == nullptr)
        {
            return (uint32)Keys.Num();
        }

        struct FPermutationCompile
        {
            FMaterialCompiler           Compiler;
            FMaterialGraphCompileResult Result;
            FMaterialCompileTarget      Target;
        };

        uint32 Failed = 0;
        CMaterialNodeGraph* Graph = nullptr;
        TVector<TUniquePtr<FPermutationCompile>> Compiles;
        for (uint64 Key : Keys)
        {
            if (Material->LoadPermutationFromArtifactCache(Key))
            {
                continue;
            }

            Graph = Graph != nullptr ? Graph : LoadMaterialGraph(Material);
            TUniquePtr<FPermutationCompile> Compile = MakeUnique<FPermutationCompile>();
            if (Graph == nullptr || !MakeMaterialPermutationTarget(Material, Key, Compile->Target) ||
                !BeginMaterialGraphCompile(Material, Graph, Compile->Compiler, Compile->Result, Compile->Target))
            {
                ++Failed;
                continue;
            }
            Compiles.Add(Move(Compile));
        }

        // Every permutation is dispatched before the first wait, so their stages share the workers.
        for (const TUniquePtr<FPermutationCompile>& Compile : Compiles)
        {
            Compile->Result.StageLog->Stages.Wait();
            FinishMaterialGraphCompile(Material, Compile->Compiler, Compile->Result, Compile->Target);
            Failed += Compile->Result.bSuccess ? 0u : 1u;
        }
        return Failed;
    }
}
