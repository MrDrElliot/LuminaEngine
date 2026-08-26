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
    bool BeginMaterialGraphCompile(CMaterial* Material, CMaterialNodeGraph* Graph, FMaterialCompiler& Compiler, FMaterialGraphCompileResult& Result)
    {
        if (Material == nullptr || Graph == nullptr)
        {
            return false;
        }

        Compiler.SetMaterialType(Material->GetMaterialType());
        Compiler.SetMasked(Material->GetBlendMode() == EBlendMode::Masked);
        Graph->CompileGraph(Compiler);
        Material->SetReadyForRender(false);

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
        if (Material->GetBlendMode() == EBlendMode::Translucent)
        {
            Options.MacroDefinitions.emplace_back("TRANSLUCENT");
        }
        if (Material->GetBlendMode() == EBlendMode::Masked)
        {
            Options.MacroDefinitions.emplace_back("MASKED");
        }
        // Every model reaches the shader through runtime flags, so an instance can override back to Lit.

        FShaderCompileOptions VSOptions;
        VSOptions.DebugName = MatName + " [VS]";

        // CommitShaderStage owns the blob store and library commit, so this file repeats no suffixes.
        auto CommitStage = [Material](EMaterialShaderStage Stage)
        {
            return [Material, Stage](const FShaderHeader& Header)
            {
                Material->CommitShaderStage(Stage, TSpan<const uint32>(Header.Binaries.data(), Header.Binaries.size()));
            };
        };

        // PBR geometry is task and mesh, while UI, PostProcess, Decal and Terrain still raster a vertex stage.
        if (Material->GetMaterialType() != EMaterialType::PBR)
        {
            ShaderCompiler->CompilerShaderRaw(Result.VertexSource, Move(VSOptions), CommitStage(EMaterialShaderStage::Vertex));
        }

        // Separate compiles rather than spec-constant variants, since they declare different OUTPUT types.
        if (Material->GetMaterialType() == EMaterialType::PBR)
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

            Material->ClearShaderStage(EMaterialShaderStage::MaskedVisBufferPixel);
            Material->ClearShaderStage(EMaterialShaderStage::VisBufferMeshMasked);
            Material->ClearShaderStage(EMaterialShaderStage::MeshShadowMasked);
            Material->ClearShaderStage(EMaterialShaderStage::ShadowMaskedPixel);
            if (Material->GetBlendMode() == EBlendMode::Masked)
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

        // Cleared first, so a translucent to opaque recompile drops stale binaries.
        Material->ClearShaderStage(EMaterialShaderStage::MomentPixel);
        const bool bNeedsMomentStage = Material->GetMaterialType() == EMaterialType::PBR
                                    && Material->GetBlendMode()   == EBlendMode::Translucent;
        if (bNeedsMomentStage)
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

    void FinishMaterialGraphCompile(CMaterial* Material, FMaterialCompiler& Compiler, FMaterialGraphCompileResult& Result)
    {
        if (Material == nullptr)
        {
            return;
        }

        // A pure function of type and blend mode, neither of which can change while stages are in flight.
        const bool bNeedsMomentStage = Material->GetMaterialType() == EMaterialType::PBR
                                    && Material->GetBlendMode()   == EBlendMode::Translucent;

        // A failed stage only leaves its output empty, and committing anyway yields a depth-only ghost.
        auto StageEmpty = [&](const TVector<uint32>& Binaries, const char* StageName) -> bool
        {
            if (!Binaries.empty())
            {
                return false;
            }
            EdNodeGraph::FError Error;
            Error.Name        = "Shader Stage Failed";
            Error.Description  = FString("The ") + StageName + " shader stage produced no output (compile failed).";
            Result.Errors.push_back(Error);
            return true;
        };

        bool bStageFailed = false;
        bStageFailed |= StageEmpty(Material->PixelShaderBinaries, "Pixel");
        if (bNeedsMomentStage)
        {
            // Without it the moment pass skips this material and shading reconstructs from absent moments.
            bStageFailed |= StageEmpty(Material->MomentPixelShaderBinaries, "Moment Pixel");
        }
        if (Material->GetMaterialType() == EMaterialType::PBR)
        {
            // There is no fallback, so a material missing one renders nothing rather than taking another path.
            bStageFailed |= StageEmpty(Material->DeferredShaderBinaries, "Deferred");
            bStageFailed |= StageEmpty(Material->VisBufferMeshShaderBinaries, "VisBuffer Geometry");
            bStageFailed |= StageEmpty(Material->MeshShaderShadowBinaries, "Shadow Geometry");
            bStageFailed |= StageEmpty(Material->MeshShaderBaseBinaries, "Base Geometry");
            if (Material->GetBlendMode() == EBlendMode::Masked)
            {
                bStageFailed |= StageEmpty(Material->MaskedVisBufferPixelShaderBinaries, "Masked VisBuffer Pixel");
                bStageFailed |= StageEmpty(Material->VisBufferMeshShaderMaskedBinaries, "Masked VisBuffer Geometry");
                bStageFailed |= StageEmpty(Material->MeshShaderShadowMaskedBinaries, "Masked Shadow Geometry");
                bStageFailed |= StageEmpty(Material->ShadowMaskedPixelShaderBinaries, "Masked Shadow Pixel");
            }
        }
        else
        {
            bStageFailed |= StageEmpty(Material->VertexShaderBinaries, "Vertex");
        }
        if (bStageFailed)
        {
            Result.Stats    = Compiler.GetStats();
            Result.bSuccess = false;
            return;   // leave the material not-ready; the caller skips it rather than saving a ghost.
        }

        // The GUID comes from the live object, so a later resolve skips the registry and survives a rename.
        {
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

        Memory::Memzero(&Material->MaterialUniforms, sizeof(FMaterialUniforms));
        Material->Parameters.clear();
        Compiler.GetParameters(Material->Parameters, Material->MaterialUniforms);

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

        CPackage* Package = Material->GetPackage();
        if (Package == nullptr)
        {
            return;
        }

        FString GraphName = "AssetMaterialGraph";
        CMaterialNodeGraph* Graph = Cast<CMaterialNodeGraph>(Package->LoadObjectByName(GraphName));
        if (Graph == nullptr)
        {
            LOG_WARN("Material '{0}' was compiled against older shader templates but has no saved graph to recompile from", Material->GetName().c_str());
            return;
        }

        // The graph's PostLoad already restored wiring, and Initialize only creates the drawing context.
        Graph->SetMaterial(Material.Get());
        Graph->ValidateGraph();

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
