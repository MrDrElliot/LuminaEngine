#include "MaterialGraphCompile.h"
#include "MaterialNodeGraph.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Memory/Memory.h"
#include "Paths/Paths.h"
#include "Renderer/MaterialTypes.h"
#include "Renderer/ShaderCompiler.h"
#include "Renderer/ShaderLibrary.h"

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

        ShaderCompiler->CompilerShaderRaw(Result.VertexSource, Move(VSOptions), [Material](const FShaderHeader& Header) mutable
        {
            Material->VertexShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
            Material->VertexShader = FShaderLibrary::Commit(FName((Material->GetGUID().ToString() + "_VS").c_str()), ERHIShaderType::Vertex,
                TSpan<const uint32>(Header.Binaries.data(), Header.Binaries.size()));
        });

        // The PBR domain also compiles the mesh-shader geometry stage, the two VisBuffer variants, and the
        // deferred-material pixel stage so the asset is portable across all render paths (matches the default
        // material + the material editor's compile). Non-PBR domains use only PS + VS.
        if (Material->GetMaterialType() == EMaterialType::PBR)
        {
            const FString MeshShaderDir = Paths::GetEngineResourceDirectory() + "/Shaders/MaterialShader/";

            const FString MeshSource = Compiler.BuildVertexShaderFromTemplate(MeshShaderDir + "MeshletMesh.slang", EMaterialType::PBR);
            FShaderCompileOptions MeshOptions; MeshOptions.DebugName = MatName + " [MS]";
            ShaderCompiler->CompilerShaderRaw(MeshSource, Move(MeshOptions), [Material](const FShaderHeader& Header) mutable
            {
                Material->MeshShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
                Material->MeshShader = FShaderLibrary::Commit(FName((Material->GetGUID().ToString() + "_MS").c_str()), ERHIShaderType::Mesh,
                    TSpan<const uint32>(Header.Binaries.data(), Header.Binaries.size()));
            });

            const FString VisSource = Compiler.BuildVertexShaderFromTemplate(MeshShaderDir + "MeshletVisBuffer.slang", EMaterialType::PBR);
            FShaderCompileOptions VisOptions; VisOptions.DebugName = MatName + " [VBM]";
            ShaderCompiler->CompilerShaderRaw(VisSource, Move(VisOptions), [Material](const FShaderHeader& Header) mutable
            {
                Material->VisBufferMeshShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
                Material->VisBufferMeshShader = FShaderLibrary::Commit(FName((Material->GetGUID().ToString() + "_VBM").c_str()), ERHIShaderType::Mesh,
                    TSpan<const uint32>(Header.Binaries.data(), Header.Binaries.size()));
            });

            const FString VisVSSource = Compiler.BuildVertexShaderFromTemplate(MeshShaderDir + "MeshletVisBufferVS.slang", EMaterialType::PBR);
            FShaderCompileOptions VisVSOptions; VisVSOptions.DebugName = MatName + " [VBV]";
            ShaderCompiler->CompilerShaderRaw(VisVSSource, Move(VisVSOptions), [Material](const FShaderHeader& Header) mutable
            {
                Material->VisBufferVertexShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
                Material->VisBufferVertexShader = FShaderLibrary::Commit(FName((Material->GetGUID().ToString() + "_VBV").c_str()), ERHIShaderType::Vertex,
                    TSpan<const uint32>(Header.Binaries.data(), Header.Binaries.size()));
            });

            // Masked-only VisBuffer PIXEL shaders: run the opacity graph + discard cut-out texels BEFORE they
            // write VisID/depth. The GEOMETRY stage is the SHARED VisBuffer VS/mesh above -- the VISBUFFER_MASKED
            // spec constant makes it emit interpolants for masked materials (no separate masked geometry shader).
            // Two PS variants: flat-VisID (VS path) + SV_PrimitiveID (mesh path). Cleared first so a
            // masked->opaque recompile drops the stale stages (never bound for non-masked).
            Material->MaskedVisBufferPixelShaderBinaries.clear();
            Material->MaskedVisBufferPixelShaderPrimBinaries.clear();
            Material->MaskedVisBufferPixelShader       = nullptr;
            Material->MaskedVisBufferPixelShaderPrim   = nullptr;
            if (Material->GetBlendMode() == EBlendMode::Masked)
            {
                const FString MaskedPSSource = Compiler.BuildPixelShaderFromTemplate(MeshShaderDir + "VisBufferMaskedPixel.slang");
                FShaderCompileOptions MaskedPSOptions; MaskedPSOptions.DebugName = MatName + " [MVBP]";
                ShaderCompiler->CompilerShaderRaw(MaskedPSSource, Move(MaskedPSOptions), [Material](const FShaderHeader& Header) mutable
                {
                    Material->MaskedVisBufferPixelShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
                    Material->MaskedVisBufferPixelShader = FShaderLibrary::Commit(FName((Material->GetGUID().ToString() + "_MVBP").c_str()), ERHIShaderType::Fragment,
                        TSpan<const uint32>(Header.Binaries.data(), Header.Binaries.size()));
                });

                // Mesh-path variant: same source compiled with VISBUFFER_PRIMID (VisID via SV_PrimitiveID).
                FShaderCompileOptions MaskedPSPrimOptions; MaskedPSPrimOptions.DebugName = MatName + " [MVBPP]";
                MaskedPSPrimOptions.MacroDefinitions.emplace_back("VISBUFFER_PRIMID");
                ShaderCompiler->CompilerShaderRaw(MaskedPSSource, Move(MaskedPSPrimOptions), [Material](const FShaderHeader& Header) mutable
                {
                    Material->MaskedVisBufferPixelShaderPrimBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
                    Material->MaskedVisBufferPixelShaderPrim = FShaderLibrary::Commit(FName((Material->GetGUID().ToString() + "_MVBPP").c_str()), ERHIShaderType::Fragment,
                        TSpan<const uint32>(Header.Binaries.data(), Header.Binaries.size()));
                });
            }

            const FString DeferredSource = Compiler.BuildDeferredShaderFromTemplate(MeshShaderDir + "DeferredMaterial.slang", EMaterialType::PBR);
            FShaderCompileOptions DeferredOptions; DeferredOptions.DebugName = MatName + " [DM]";
            ShaderCompiler->CompilerShaderRaw(DeferredSource, Move(DeferredOptions), [Material](const FShaderHeader& Header) mutable
            {
                Material->DeferredShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
                Material->DeferredShader = FShaderLibrary::Commit(FName((Material->GetGUID().ToString() + "_DM").c_str()), ERHIShaderType::Fragment,
                    TSpan<const uint32>(Header.Binaries.data(), Header.Binaries.size()));
            });
        }

        ShaderCompiler->CompilerShaderRaw(Result.PixelSource, Move(Options), [Material](const FShaderHeader& Header) mutable
        {
            Material->PixelShaderBinaries.assign(Header.Binaries.begin(), Header.Binaries.end());
            Material->PixelShader = FShaderLibrary::Commit(FName((Material->GetGUID().ToString() + "_PS").c_str()), ERHIShaderType::Fragment,
                TSpan<const uint32>(Header.Binaries.data(), Header.Binaries.size()));
        });

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

        Material->PostLoad();

        Result.bSuccess = true;
        return Result;
    }
}
