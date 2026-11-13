#include "MaterialCompileScheduler.h"
#include "MaterialGraphCompile.h"
#include "MaterialNodeGraph.h"
#include "Assets/AssetManager/AssetManager.h"
#include "Assets/AssetRegistry/AssetData.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialCompileQueue.h"
#include "Containers/Algorithm.h"
#include "Core/CoreEditorDelegates.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Package/Package.h"
#include "Platform/Time/PlatformTime.h"
#include "Renderer/RenderManager.h"
#include "Renderer/ShaderCompiler.h"
#include "TaskSystem/Fork/ForkRuntime.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        // Graph codegen runs on the game thread, so launches stop here and resume next frame rather than hitch.
        constexpr double kLaunchBudgetMs = 4.0;

        // Nothing waits on these, and a waiting thread that helped with one would stall its frame for the whole compile.
        constexpr Jobs::EJobPriority kCompilePriority = Jobs::EJobPriority::Background;
    }

    struct FMaterialCompileScheduler::FJob
    {
        TStrongObjectPtr<CMaterial>          Material;
        TStrongObjectPtr<CMaterialNodeGraph> Graph;
        FMaterialCompiler                    Compiler;
        FMaterialGraphCompileResult          Result;
        FMaterialCompileTarget               Target;
        uint64                               GraphContentVersion = 0;
        uint64                               SourceHashBefore    = 0;
        bool                                 bFunctionChange     = false;
        bool                                 bSaveWhenComplete   = false;
    };

    FMaterialCompileScheduler::FMaterialCompileScheduler() = default;

    FMaterialCompileScheduler::~FMaterialCompileScheduler()
    {
        // Stage callbacks write into the material and compiler, so neither may go while one is still running.
        for (const TUniquePtr<FJob>& Job : InFlight)
        {
            Job->Result.StageLog->Stages.Wait();
        }
    }

    void FMaterialCompileScheduler::Tick()
    {
        FRenderManager* Renderer = TryRender();
        if (Renderer == nullptr || Global::GShaderCompiler == nullptr)
        {
            return;
        }

        FMaterialCompileQueue& Queue = Renderer->GetMaterialCompileQueue();
        FinishSettledJobs(Queue);
        LaunchJobs(Queue);

        if (InFlight.IsEmpty() && Deferred.IsEmpty() && MaterialsNeedingSave > 0)
        {
            ImGuiX::Notifications::NotifyInfo("{0} material(s) recompiled into the artifact cache. Save them once to drop the bytecode from their assets.", MaterialsNeedingSave);
            MaterialsNeedingSave = 0;
        }
    }

    void FMaterialCompileScheduler::FinishSettledJobs(FMaterialCompileQueue& Queue)
    {
        for (int32 i = 0; i < InFlight.Num();)
        {
            FJob& Job = *InFlight[i];
            if (!Job.Result.StageLog->Stages.IsSettled())
            {
                ++i;
                continue;
            }

            if (Job.Target.bPermutation)
            {
                FinishPermutation(Job);
            }
            else
            {
                FinishRecompile(Job, Queue);
            }
            InFlight.RemoveAt(i);
        }
    }

    void FMaterialCompileScheduler::LaunchJobs(FMaterialCompileQueue& Queue)
    {
        // A quarter of the workers stays free, so the editor's own frame work is never starved by a recompile burst.
        const uint32 StageBudget = Math::Max(1u, Jobs::GetNumWorkers() * 3u / 4u);
        const uint64 Start = PlatformTime::Cycles();

        TVector<FRequest> Retry = Move(Deferred);
        Deferred.Reset();
        int32 NextRetry = 0;

        while (OutstandingStages() < StageBudget && PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start) < kLaunchBudgetMs)
        {
            FRequest Request;
            if (NextRetry < Retry.Num())
            {
                Request = Move(Retry[NextRetry++]);
            }
            else if (!PopRequest(Queue, Request))
            {
                break;
            }
            Launch(Move(Request));
        }

        for (; NextRetry < Retry.Num(); ++NextRetry)
        {
            Deferred.Add(Move(Retry[NextRetry]));
        }
    }

    bool FMaterialCompileScheduler::PopRequest(FMaterialCompileQueue& Queue, FRequest& OutRequest)
    {
        FGuid MaterialGUID;
        if (Queue.PopFunctionRecompile(MaterialGUID))
        {
            OutRequest.Kind = ERequestKind::FunctionRecompile;
            OutRequest.Material = Cast<CMaterial>(FAssetManager::Get().LoadSynchronous(MaterialGUID));
            return true;
        }

        if (TStrongObjectPtr<CMaterial> Material = Queue.PopRecompile())
        {
            OutRequest.Kind     = ERequestKind::Recompile;
            OutRequest.Material = Move(Material);
            return true;
        }

        OutRequest.Kind = ERequestKind::Permutation;
        return Queue.PopPermutation(OutRequest.Material, OutRequest.Key);
    }

    void FMaterialCompileScheduler::Launch(FRequest&& Request)
    {
        if (!Request.Material.IsValid())
        {
            return;
        }

        // Two jobs on one material would race each other's stage commits.
        if (IsCompiling(Request.Material.Get()))
        {
            Deferred.Add(Move(Request));
            return;
        }

        if (Request.Kind == ERequestKind::Permutation)
        {
            LaunchPermutation(Move(Request));
        }
        else
        {
            LaunchRecompile(Move(Request));
        }
    }

    void FMaterialCompileScheduler::LaunchRecompile(FRequest&& Request)
    {
        CMaterial* Material = Request.Material.Get();
        const bool bFunctionChange = Request.Kind == ERequestKind::FunctionRecompile;
        const bool bCurrent = Material->CompiledTemplateHash == CMaterial::GetShaderTemplateHash() && Material->SourceHash != 0;
        if (!bFunctionChange && bCurrent)
        {
            return;
        }

        CMaterialNodeGraph* Graph = LoadMaterialGraph(Material);
        if (Graph == nullptr)
        {
            LOG_WARN("Material '{0}' has no saved graph to recompile from", *Material->GetName());
            return;
        }

        TUniquePtr<FJob> Job = MakeUnique<FJob>();
        Job->Material            = Move(Request.Material);
        Job->Graph               = Graph;
        Job->GraphContentVersion = Graph->GetTreeContentVersion();
        Job->SourceHashBefore    = Material->SourceHash;
        Job->bFunctionChange     = bFunctionChange;
        Job->bSaveWhenComplete   = bFunctionChange && !Material->GetPackage()->IsDirty() && !Graph->NeedsCompile();

        if (!BeginMaterialGraphCompile(Material, Graph, Job->Compiler, Job->Result, {}, kCompilePriority))
        {
            if (bFunctionChange)
            {
                Graph->MarkCompiled();
            }
            ImGuiX::Notifications::NotifyError("Material '{0}' failed to recompile", *Material->GetName());
            return;
        }
        InFlight.Add(Move(Job));
    }

    void FMaterialCompileScheduler::LaunchPermutation(FRequest&& Request)
    {
        CMaterial* Material = Request.Material.Get();

        // A material that failed its own compile has no manifest to build the permutation against.
        if (!Material->IsReadyForRender() || Material->HasPermutation(Request.Key))
        {
            return;
        }

        TUniquePtr<FJob> Job = MakeUnique<FJob>();
        if (!MakeMaterialPermutationTarget(Material, Request.Key, Job->Target))
        {
            return;
        }

        CMaterialNodeGraph* Graph = LoadMaterialGraph(Material);
        if (Graph == nullptr)
        {
            LOG_WARN("Material '{0}': an instance selects a static switch permutation this material has not "
                     "compiled, and it has no saved graph to compile it from. The instance draws with every "
                     "switch at its default.", *Material->GetName());
            return;
        }

        Job->Material = Move(Request.Material);
        if (!BeginMaterialGraphCompile(Material, Graph, Job->Compiler, Job->Result, Job->Target, kCompilePriority))
        {
            ImGuiX::Notifications::NotifyError("Material '{0}': a static switch permutation failed to compile", *Material->GetName());
            return;
        }
        InFlight.Add(Move(Job));
    }

    void FMaterialCompileScheduler::FinishRecompile(FJob& Job, FMaterialCompileQueue& Queue)
    {
        CMaterial* Material = Job.Material.Get();
        FinishMaterialGraphCompile(Material, Job.Compiler, Job.Result);

        const bool bGraphUnchanged = Job.Graph->GetTreeContentVersion() == Job.GraphContentVersion;
        if (Job.bFunctionChange && bGraphUnchanged)
        {
            Job.Graph->MarkCompiled();
        }

        const FString Name = *Material->GetName();
        if (!Job.Result.bSuccess)
        {
            for (const EdNodeGraph::FError& Error : Job.Result.Errors)
            {
                LOG_ERROR("Material '{0}' recompile failed. {1}", *Name, *Error.Description);
            }
            ImGuiX::Notifications::NotifyError("Material '{0}' failed to recompile", *Name);
            return;
        }

        // A template edit alone only refreshed the artifact cache, so the asset stays clean for source control.
        CPackage* Package = Material->GetPackage();
        if (Package == nullptr || Material->SourceHash == Job.SourceHashBefore)
        {
            return;
        }

        Package->MarkDirty();
        if (!Job.bFunctionChange)
        {
            ++MaterialsNeedingSave;
            return;
        }

        const bool bSuperseded = Queue.IsFunctionRecompileQueued(Material->GetGUID());
        if (!Job.bSaveWhenComplete || !bGraphUnchanged || bSuperseded)
        {
            ImGuiX::Notifications::NotifyInfo("Material '{0}' recompiled after a material function changed; save to keep", *Name);
            return;
        }

        FCoreEditorDelegates::OnAssetPreSave.Broadcast(Material);
        if (CPackage::SavePackage(Package, Package->GetPackagePath()))
        {
            FAssetRegistry::Get().AssetSaved(Material);
            FCoreEditorDelegates::OnAssetSaved.Broadcast(Material);
            LOG_INFO("Material '{0}' recompiled and saved after a material function changed", *Name);
        }
        else
        {
            ImGuiX::Notifications::NotifyError("Material '{0}' recompiled but could not be saved", *Name);
        }
    }

    void FMaterialCompileScheduler::FinishPermutation(FJob& Job)
    {
        FinishMaterialGraphCompile(Job.Material.Get(), Job.Compiler, Job.Result, Job.Target);

        // A recompile that superseded this permutation fails it with no errors, and says so itself.
        if (!Job.Result.bSuccess && !Job.Result.Errors.IsEmpty())
        {
            ImGuiX::Notifications::NotifyError("Material '{0}': a static switch permutation failed to compile", *Job.Material->GetName());
        }
    }

    bool FMaterialCompileScheduler::IsCompiling(const CMaterial* Material) const
    {
        return Algo::AnyOf(InFlight, [Material](const TUniquePtr<FJob>& Job) { return Job->Material.Get() == Material; });
    }

    uint32 FMaterialCompileScheduler::OutstandingStages() const
    {
        uint32 Stages = 0;
        for (const TUniquePtr<FJob>& Job : InFlight)
        {
            Stages += Job->Result.StageLog->Stages.GetOutstanding();
        }
        return Stages;
    }
}
