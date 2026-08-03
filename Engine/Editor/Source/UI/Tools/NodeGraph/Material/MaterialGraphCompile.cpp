#include "MaterialGraphCompile.h"
#include "MaterialNodeGraph.h"
#include "Assets/AssetTypes/Material/Material.h"
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

        ShaderCompiler->CompilerShaderRaw(Result.VertexSource, Move(VSOptions), CommitStage(EMaterialShaderStage::Vertex));

        // The PBR domain also compiles the mesh-shader geometry stage, the two VisBuffer variants, and the
        // deferred-material pixel stage so the asset is portable across all render paths (matches the default
        // material + the material editor's compile). Non-PBR domains use only PS + VS.
        if (Material->GetMaterialType() == EMaterialType::PBR)
        {
            const FString MeshShaderDir = Paths::GetEngineResourceDirectory() + "/Shaders/MaterialShader/";

            const FString MeshSource = Compiler.BuildVertexShaderFromTemplate(MeshShaderDir + "MeshletMesh.slang", EMaterialType::PBR);
            FShaderCompileOptions MeshOptions; MeshOptions.DebugName = MatName + " [MS]";
            ShaderCompiler->CompilerShaderRaw(MeshSource, Move(MeshOptions), CommitStage(EMaterialShaderStage::Mesh));

            const FString VisSource = Compiler.BuildVertexShaderFromTemplate(MeshShaderDir + "MeshletVisBuffer.slang", EMaterialType::PBR);
            FShaderCompileOptions VisOptions; VisOptions.DebugName = MatName + " [VBM]";
            ShaderCompiler->CompilerShaderRaw(VisSource, Move(VisOptions), CommitStage(EMaterialShaderStage::VisBufferMesh));

            const FString VisVSSource = Compiler.BuildVertexShaderFromTemplate(MeshShaderDir + "MeshletVisBufferVS.slang", EMaterialType::PBR);
            FShaderCompileOptions VisVSOptions; VisVSOptions.DebugName = MatName + " [VBV]";
            ShaderCompiler->CompilerShaderRaw(VisVSSource, Move(VisVSOptions), CommitStage(EMaterialShaderStage::VisBufferVertex));

            // Masked-only VisBuffer PIXEL shaders: run the opacity graph + discard cut-out texels BEFORE they
            // write VisID/depth. The GEOMETRY stage is the SHARED VisBuffer VS/mesh above -- the VISBUFFER_MASKED
            // spec constant makes it emit interpolants for masked materials (no separate masked geometry shader).
            // Two PS variants: flat-VisID (VS path) + SV_PrimitiveID (mesh path). Cleared first so a
            // masked->opaque recompile drops the stale stages (never bound for non-masked).
            Material->ClearShaderStage(EMaterialShaderStage::MaskedVisBufferPixel);
            Material->ClearShaderStage(EMaterialShaderStage::MaskedVisBufferPixelPrim);
            if (Material->GetBlendMode() == EBlendMode::Masked)
            {
                const FString MaskedPSSource = Compiler.BuildPixelShaderFromTemplate(MeshShaderDir + "VisBufferMaskedPixel.slang");
                FShaderCompileOptions MaskedPSOptions; MaskedPSOptions.DebugName = MatName + " [MVBP]";
                ShaderCompiler->CompilerShaderRaw(MaskedPSSource, Move(MaskedPSOptions), CommitStage(EMaterialShaderStage::MaskedVisBufferPixel));

                // Mesh-path variant: same source compiled with VISBUFFER_PRIMID (VisID via SV_PrimitiveID).
                FShaderCompileOptions MaskedPSPrimOptions; MaskedPSPrimOptions.DebugName = MatName + " [MVBPP]";
                MaskedPSPrimOptions.MacroDefinitions.emplace_back("VISBUFFER_PRIMID");
                ShaderCompiler->CompilerShaderRaw(MaskedPSSource, Move(MaskedPSPrimOptions), CommitStage(EMaterialShaderStage::MaskedVisBufferPixelPrim));
            }

            const FString DeferredSource = Compiler.BuildDeferredShaderFromTemplate(MeshShaderDir + "DeferredMaterial.slang", EMaterialType::PBR);
            FShaderCompileOptions DeferredOptions; DeferredOptions.DebugName = MatName + " [DM]";
            ShaderCompiler->CompilerShaderRaw(DeferredSource, Move(DeferredOptions), CommitStage(EMaterialShaderStage::Deferred));
        }

        ShaderCompiler->CompilerShaderRaw(Result.PixelSource, Move(Options), CommitStage(EMaterialShaderStage::Pixel));

        ShaderCompiler->Flush();

        // CompilerShaderRaw signals a failed stage only by leaving its output empty. A graph that passes the
        // node-level checks above can still fail to compile in a specific template (notably the deferred stage),
        // which would otherwise save a master that writes VisBuffer depth but is SKIPPED by the deferred shading
        // pass (DeferredMaterialPass requires a non-null DeferredShader) -> the mesh renders as a depth-only
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
        bStageFailed |= StageEmpty(Material->VertexShaderBinaries, "Vertex");
        bStageFailed |= StageEmpty(Material->PixelShaderBinaries, "Pixel");
        if (Material->GetMaterialType() == EMaterialType::PBR)
        {
            // The deferred + VisBuffer (vertex-emulation) stages are always required for the VisBuffer path;
            // the mesh-shader variants are optional (device-gated), so they're not checked here.
            bStageFailed |= StageEmpty(Material->DeferredShaderBinaries, "Deferred");
            bStageFailed |= StageEmpty(Material->VisBufferVertexShaderBinaries, "VisBuffer");
            // Masked materials additionally require their masked VisBuffer PIXEL shaders so cut-outs clip in the
            // geometry stage; without them they'd stamp full-coverage depth and occlude geometry behind. (The
            // geometry shader is the shared VisBuffer VS/mesh, already required above, spec-gated for masked.)
            if (Material->GetBlendMode() == EBlendMode::Masked)
            {
                bStageFailed |= StageEmpty(Material->MaskedVisBufferPixelShaderBinaries, "Masked VisBuffer Pixel");
                bStageFailed |= StageEmpty(Material->MaskedVisBufferPixelShaderPrimBinaries, "Masked VisBuffer Pixel (mesh)");
            }
        }
        if (bStageFailed)
        {
            Result.Stats    = Compiler.GetStats();
            Result.bSuccess = false;
            return Result;   // leave the material not-ready; the caller skips it rather than saving a ghost.
        }

        Compiler.GetBoundTextures(Material->Textures);

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
