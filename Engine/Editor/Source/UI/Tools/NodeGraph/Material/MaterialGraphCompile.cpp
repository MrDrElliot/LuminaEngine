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

        // Carried on BOTH paths: the failure path keeps warnings so a graph with an error and a warning does
        // not silently drop the warning once the error is fixed.
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

        // Crash-dump-friendly shader names: "<MaterialName> [Stage]".
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
        // NO shading-model defines. Every model, Unlit included, reaches the shader through the material's
        // runtime flags and is branched on there, so an instance can still override its way back to Lit.

        FShaderCompileOptions VSOptions;
        VSOptions.DebugName = MatName + " [VS]";

        // One committing callback per stage; CMaterial::CommitShaderStage owns the blob-store +
        // library-commit against the shared stage table, so this file never repeats suffixes/types.
        auto CommitStage = [Material](EMaterialShaderStage Stage)
        {
            return [Material, Stage](const FShaderHeader& Header)
            {
                Material->CommitShaderStage(Stage, TSpan<const uint32>(Header.Binaries.data(), Header.Binaries.size()));
            };
        };

        // PBR geometry has no vertex stage -- it is task + mesh. The other domains (UI, PostProcess,
        // Decal, Terrain) raster through a real vertex shader and still compile one.
        if (Material->GetMaterialType() != EMaterialType::PBR)
        {
            ShaderCompiler->CompilerShaderRaw(Result.VertexSource, Move(VSOptions), CommitStage(EMaterialShaderStage::Vertex));
        }

        // PBR geometry is task + mesh end to end: four mesh binaries from two templates, no vertex stage.
        // Separate compiles rather than spec-constant variants because they declare different OUTPUT types.
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
                FShaderCompileOptions Options;
                Options.DebugName = MatName + " [" + Geo.Tag + "]";
                if (Geo.Define != nullptr)
                {
                    Options.MacroDefinitions.emplace_back(Geo.Define);
                }
                ShaderCompiler->CompilerShaderRaw(*Geo.Source, Move(Options), CommitStage(Geo.Stage));
            }

            Material->ClearShaderStage(EMaterialShaderStage::MaskedVisBufferPixel);
            Material->ClearShaderStage(EMaterialShaderStage::VisBufferMeshMasked);
            if (Material->GetBlendMode() == EBlendMode::Masked)
            {
                // Masked geometry widens the per-vertex output back to the full interpolant set the masked
                // pixel shader reads; opaque materials keep the position-only stage above.
                FShaderCompileOptions VisMaskedOptions; VisMaskedOptions.DebugName = MatName + " [VBMM]";
                VisMaskedOptions.MacroDefinitions.emplace_back("VISBUFFER_MASKED_GEOM");
                ShaderCompiler->CompilerShaderRaw(VisSource, Move(VisMaskedOptions), CommitStage(EMaterialShaderStage::VisBufferMeshMasked));

                const FString MaskedPSSource = Compiler.BuildPixelShaderFromTemplate(MeshShaderDir + "VisBufferMaskedPixel.slang");
                FShaderCompileOptions MaskedPSOptions; MaskedPSOptions.DebugName = MatName + " [MVBP]";
                MaskedPSOptions.MacroDefinitions.emplace_back("VISBUFFER_PRIMID");
                ShaderCompiler->CompilerShaderRaw(MaskedPSSource, Move(MaskedPSOptions), CommitStage(EMaterialShaderStage::MaskedVisBufferPixel));
            }

            const FString DeferredSource = Compiler.BuildDeferredShaderFromTemplate(MeshShaderDir + "DeferredMaterial.slang", EMaterialType::PBR);
            FShaderCompileOptions DeferredOptions; DeferredOptions.DebugName = MatName + " [DM]";

            // No defines on this lane. The runtime model check lives in ShadeGBuffer, so the forward and
            // deferred lanes read the same flag and cannot drift into "correct in the editor, lit in the world".
            ShaderCompiler->CompilerShaderRaw(DeferredSource, Move(DeferredOptions), CommitStage(EMaterialShaderStage::Deferred));
        }

        ShaderCompiler->CompilerShaderRaw(Result.PixelSource, Move(Options), CommitStage(EMaterialShaderStage::Pixel));

        // MBOIT pass 1: a second compile of the SAME pixel source that runs the opacity graph and stops,
        // accumulating moments. Cleared first, so a translucent -> opaque recompile drops stale binaries.
        Material->ClearShaderStage(EMaterialShaderStage::MomentPixel);
        const bool bNeedsMomentStage = Material->GetMaterialType() == EMaterialType::PBR
                                    && Material->GetBlendMode()   == EBlendMode::Translucent;
        if (bNeedsMomentStage)
        {
            FShaderCompileOptions MomentOptions;
            MomentOptions.DebugName = MatName + " [MOM]";
            MomentOptions.MacroDefinitions.emplace_back("TRANSLUCENT");
            MomentOptions.MacroDefinitions.emplace_back("MOMENT_GENERATION");
            // ComputeSurfaceCoverage tests the model at runtime, so the moment pass needs no UNLIT define
            // to agree with the shading pass about an unlit surface's coverage.
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

        // Recomputed rather than carried across the wait: it is a pure function of the material's own
        // type and blend mode, and neither can change while its stages are in flight.
        const bool bNeedsMomentStage = Material->GetMaterialType() == EMaterialType::PBR
                                    && Material->GetBlendMode()   == EBlendMode::Translucent;

        // CompilerShaderRaw signals a failed stage only by leaving its output empty. Committing one anyway
        // yields a master that writes VisBuffer depth but is skipped by shading -- a depth-only ghost.
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
            // Required, not optional: without it the moment pass skips this material and the shading pass
            // reconstructs transmittance from moments that never saw it.
            bStageFailed |= StageEmpty(Material->MomentPixelShaderBinaries, "Moment Pixel");
        }
        if (Material->GetMaterialType() == EMaterialType::PBR)
        {
            // Every geometry stage is REQUIRED: there is no fallback, so a material missing one renders nothing
            // in whichever pass wanted it rather than quietly taking the other path.
            bStageFailed |= StageEmpty(Material->DeferredShaderBinaries, "Deferred");
            bStageFailed |= StageEmpty(Material->VisBufferMeshShaderBinaries, "VisBuffer Geometry");
            bStageFailed |= StageEmpty(Material->MeshShaderShadowBinaries, "Shadow Geometry");
            bStageFailed |= StageEmpty(Material->MeshShaderBaseBinaries, "Base Geometry");
            if (Material->GetBlendMode() == EBlendMode::Masked)
            {
                bStageFailed |= StageEmpty(Material->MaskedVisBufferPixelShaderBinaries, "Masked VisBuffer Pixel");
                bStageFailed |= StageEmpty(Material->VisBufferMeshShaderMaskedBinaries, "Masked VisBuffer Geometry");
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

        // The compiler works in live CTexture*; the material stores SOFT refs. The GUID is filled from the
        // live object so a later resolve never hits the registry by path, and a rename still resolves.
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

                // Registry first, package path as fallback. An empty path is not recoverable: the soft ref looks
                // valid while every resolve calls LoadPackage("") and yields the magenta placeholder forever.
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

                // Seed the cache with the pointer the graph already holds. Round-tripping through the asset manager
                // for something already resident is what made a freshly imported material render magenta.
                Material->ResolvedTextures.emplace_back(Texture);
            }
        }

        Memory::Memzero(&Material->MaterialUniforms, sizeof(FMaterialUniforms));
        Material->Parameters.clear();
        Compiler.GetParameters(Material->Parameters, Material->MaterialUniforms);

        // Stamp the templates these stages were built from (serialized with the asset) BEFORE PostLoad,
        // which compares it against the current hash to queue stale materials for auto-recompile.
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

        return Result;
    }

    namespace
    {
        // The one stale recompile currently in flight, if any. The stage compiles run on the shader task
        // swarm, so the drain dispatches and then POLLS across frames rather than calling Flush() -- which
        // would park the game thread inside EditorUI::OnUpdate for the whole compile.
        struct FPendingStaleRecompile
        {
            // Strong ref, and load-bearing: the per-stage commit callbacks capture the material RAW and fire
            // on a worker, so nothing else may be the last thing keeping it alive across the wait.
            TObjectPtr<CMaterial>         Material;
            // Finish reads the bound textures and parameters back off the compiler, so it has to outlive
            // the dispatch it was built by.
            TUniquePtr<FMaterialCompiler> Compiler;
            FMaterialGraphCompileResult   Result;
        };

        FPendingStaleRecompile GPendingStaleRecompile;
    }

    void ProcessStaleMaterialRecompiles()
    {
        // Finish the in-flight one before taking another; one material at a time keeps the swarm's work
        // bounded and the poll below unambiguous.
        if (GPendingStaleRecompile.Compiler != nullptr)
        {
            // Global across every compile in the process, so an unrelated burst delays this finish. That is
            // the same counter Flush() waited on -- a later commit, never a wrong one.
            if (GShaderCompiler->HasPendingRequests())
            {
                return;
            }

            CMaterial* Material = GPendingStaleRecompile.Material.Get();
            FinishMaterialGraphCompile(Material, *GPendingStaleRecompile.Compiler, GPendingStaleRecompile.Result);

            // Read before the reset: the material may be the only thing still naming the package.
            CPackage* Package = (Material != nullptr) ? Material->GetPackage() : nullptr;
            const FString Name = (Material != nullptr) ? FString(Material->GetName().c_str()) : FString("<destroyed>");
            const bool bSuccess = GPendingStaleRecompile.Result.bSuccess;

            GPendingStaleRecompile = {};

            if (bSuccess && Package != nullptr)
            {
                // Committed in memory; dirty the package so the user can save and stop paying the recompile
                // on every session. Never auto-save.
                Package->MarkDirty();
                ImGuiX::Notifications::NotifyInfo("Material '{0}' recompiled (shader templates changed) - save to keep", Name.c_str());
            }
            else
            {
                ImGuiX::Notifications::NotifyError("Material '{0}' failed to recompile against the current shader templates", Name.c_str());
            }
            return;
        }

        // One material per drain: each recompile runs the full multi-stage pipeline, so spreading the work
        // avoids one long hitch when a template edit makes many materials stale at once.
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

        // Headless compile: node/pin wiring is already restored by the graph's PostLoad; skip Initialize()
        // (it creates the ImGui node-editor context, only needed for drawing). Same flow as the importer.
        Graph->SetMaterial(Material.Get());
        Graph->ValidateGraph();

        TUniquePtr<FMaterialCompiler> Compiler = MakeUnique<FMaterialCompiler>();
        FMaterialGraphCompileResult   Result;

        // Only park it if stages actually went out; a graph that failed up front has nothing to wait for
        // and would otherwise hold the slot until the next unrelated compile drained the counter.
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
