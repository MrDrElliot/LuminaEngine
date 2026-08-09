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
    FMaterialGraphCompileResult CompileMaterialGraph(CMaterial* Material, CMaterialNodeGraph* Graph)
    {
        FMaterialGraphCompileResult Result;
        if (Material == nullptr || Graph == nullptr)
        {
            return Result;
        }

        FMaterialCompiler Compiler;
        Compiler.SetMaterialType(Material->GetMaterialType());
        Compiler.SetMasked(Material->GetBlendMode() == EBlendMode::Masked);
        Graph->CompileGraph(Compiler);
        Material->SetReadyForRender(false);

        // Carried out on BOTH paths. A warning is about a material that compiled, so the success path is
        // the one that matters; the failure path keeps them so a graph with an error and a warning does not
        // silently drop the warning once the error is fixed.
        Result.Warnings = Compiler.GetWarnings();

        if (Compiler.HasErrors())
        {
            Result.Errors   = Compiler.GetErrors();
            Result.Stats    = Compiler.GetStats();
            Result.bSuccess = false;
            return Result;
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
        if (Material->GetShadingModel() == EMaterialShadingModel::Unlit)
        {
            Options.MacroDefinitions.emplace_back("UNLIT");
        }

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

        // PBR geometry is task + mesh, end to end: four mesh binaries from two templates, no vertex stage.
        // Each is per-material because the vertex graph's World Position Offset is compiled into it, and
        // each is a separate compile rather than a spec-constant variant because they declare different
        // OUTPUT types -- which a spec constant cannot change.
        //
        // The masked pair is compiled only for masked materials and cleared otherwise, so a masked ->
        // opaque recompile drops the stale stages instead of leaving them bound.
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

            // UNLIT must reach this lane too. Opaque PBR shades through the deferred VisBuffer pass, NOT the
            // forward pixel stage above -- so an unlit master compiled without it takes the lit branch of
            // ShadeSurface and picks up IBL/lights/shadows exactly where the shading model says it should not.
            // The forward stage (which does get UNLIT) is the one you see in the material editor preview, so the
            // two lanes disagreeing shows up as "correct in the editor, lit in the world".
            //
            // UNLIT only. TRANSLUCENT does not belong: translucent/additive never resolve a deferred shader
            // (MeshResolve::ResolveSurfaceMaterial returns before requiring one) and this template's PSOutput
            // reads only Shaded.Color/.Alpha, never the translucent Reflection pair. MASKED does not belong
            // either: it gates [earlydepthstencil], which this template has no attribute for, and the alpha-test
            // discard inside ShadeSurface is driven by the runtime MatFlag_Masked flag rather than the define.
            if (Material->GetShadingModel() == EMaterialShadingModel::Unlit)
            {
                DeferredOptions.MacroDefinitions.emplace_back("UNLIT");
            }

            ShaderCompiler->CompilerShaderRaw(DeferredSource, Move(DeferredOptions), CommitStage(EMaterialShaderStage::Deferred));
        }

        ShaderCompiler->CompilerShaderRaw(Result.PixelSource, Move(Options), CommitStage(EMaterialShaderStage::Pixel));

        // MBOIT pass 1 (Includes/MomentOIT.slang): a second compile of the SAME pixel source that runs the
        // opacity graph and stops there, accumulating absorbance moments instead of shading. Only PBR
        // translucency is drawn by the moment pass -- additive materials composite order-independently on
        // their own, and the non-meshlet domains never reach it -- so nothing else compiles the stage.
        //
        // Cleared first, so a translucent -> opaque recompile drops the stale binaries rather than leaving
        // the moment pass drawing geometry that the shading pass no longer visits.
        Material->ClearShaderStage(EMaterialShaderStage::MomentPixel);
        const bool bNeedsMomentStage = Material->GetMaterialType() == EMaterialType::PBR
                                    && Material->GetBlendMode()   == EBlendMode::Translucent;
        if (bNeedsMomentStage)
        {
            FShaderCompileOptions MomentOptions;
            MomentOptions.DebugName = MatName + " [MOM]";
            MomentOptions.MacroDefinitions.emplace_back("TRANSLUCENT");
            MomentOptions.MacroDefinitions.emplace_back("MOMENT_GENERATION");
            if (Material->GetShadingModel() == EMaterialShadingModel::Unlit)
            {
                MomentOptions.MacroDefinitions.emplace_back("UNLIT");
            }
            ShaderCompiler->CompilerShaderRaw(Result.PixelSource, Move(MomentOptions), CommitStage(EMaterialShaderStage::MomentPixel));
        }

        ShaderCompiler->Flush();

        // CompilerShaderRaw signals a failed stage only by leaving its output empty. A graph that passes the
        // node-level checks above can still fail to compile in a specific template (notably the deferred stage),
        // which would otherwise save a master that writes VisBuffer depth but is SKIPPED by the deferred shading
        // pass (MaterialGBufferPass requires a non-null DeferredShader) -> the mesh renders as a depth-only
        // "ghost" in the world while still looking fine when reopened in the editor (which recompiles). Treat any
        // required stage with empty binaries as a compile failure so a broken master is never committed/saved.
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
            // Required, not optional: without it the moment pass skips this material, so the shading pass
            // reconstructs its transmittance from moments that never saw it and the surface renders at full
            // brightness through everything in front of it.
            bStageFailed |= StageEmpty(Material->MomentPixelShaderBinaries, "Moment Pixel");
        }
        if (Material->GetMaterialType() == EMaterialType::PBR)
        {
            // Every geometry stage is REQUIRED now: there is no fallback to fall back to, so a material
            // missing one renders nothing in whichever pass wanted it rather than quietly taking the
            // other path. Shadow and base are separate entries because a material can legitimately be
            // opaque-only or translucent-only in a scene, but both are cheap and always compiled.
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
            return Result;   // leave the material not-ready; the caller skips it rather than saving a ghost.
        }

        // The compiler works in live CTexture*; the material stores SOFT refs, so the graph's texture
        // pins are demoted to (path, GUID) here. The GUID is filled in from the live object rather than
        // left for TryResolve, so a later resolve never has to hit the registry by path -- and so a
        // texture that gets renamed still resolves.
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

                // Registry first, package path as the fallback. The registry can legitimately miss a
                // texture that was imported moments ago and has not been indexed yet, and an empty path
                // is not a recoverable state: the soft ref would look valid (its GUID is set) while
                // every resolve of it calls LoadPackage("") and silently yields the magenta placeholder,
                // for the life of the asset.
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

                // Seed the resolved cache with the pointer the graph already holds. Compiling is not a
                // reason to go back through the asset manager for something that is loaded and resident
                // right now -- and doing so is what made a freshly imported material render magenta,
                // because the round trip could fail while the live pointer was sitting right here.
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
        return Result;
    }

    void ProcessStaleMaterialRecompiles()
    {
        // One material per call: each recompile runs the full multi-stage pipeline (ending in a compiler
        // Flush), so spreading the work across frames avoids one long hitch when a template edit makes
        // many materials stale at once.
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

        const FMaterialGraphCompileResult Result = CompileMaterialGraph(Material.Get(), Graph);
        if (Result.bSuccess)
        {
            // Committed in memory; dirty the package so the user can save and stop paying the recompile
            // on every session. Never auto-save.
            Package->MarkDirty();
            ImGuiX::Notifications::NotifyInfo("Material '{0}' recompiled (shader templates changed) - save to keep", Material->GetName().c_str());
        }
        else
        {
            ImGuiX::Notifications::NotifyError("Material '{0}' failed to recompile against the current shader templates", Material->GetName().c_str());
        }
    }
}
