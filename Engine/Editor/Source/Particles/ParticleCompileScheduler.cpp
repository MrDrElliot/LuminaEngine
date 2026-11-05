#include "ParticleCompileScheduler.h"
#include "ParticleOps.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleCompileQueue.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Containers/Algorithm.h"
#include "Core/Object/Package/Package.h"
#include "Renderer/RenderManager.h"
#include "Renderer/ShaderCompiler.h"
#include "TaskSystem/Fork/ForkRuntime.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "Log/Log.h"

namespace Lumina
{
    struct FParticleCompileScheduler::FJob
    {
        TStrongObjectPtr<CParticleSystem>               System;
        TVector<TUniquePtr<FParticleEmitterCompileJob>> Emitters;
        TVector<uint64>                                 CodeHashesBefore;
    };

    FParticleCompileScheduler::FParticleCompileScheduler() = default;

    FParticleCompileScheduler::~FParticleCompileScheduler()
    {
        // The compile callbacks only hold their shared binaries, but waiting keeps a shutdown from racing a half-built job.
        for (const TUniquePtr<FJob>& Job : InFlight)
        {
            for (const TUniquePtr<FParticleEmitterCompileJob>& Emitter : Job->Emitters)
            {
                Emitter->Waiter.Wait();
            }
        }
    }

    void FParticleCompileScheduler::Tick()
    {
        if (TryRender() == nullptr || GShaderCompiler == nullptr)
        {
            return;
        }

        FinishSettledJobs();
        LaunchJobs();

        if (InFlight.IsEmpty() && AssetsNeedingSave > 0)
        {
            ImGuiX::Notifications::NotifyInfo("{0} particle asset(s) recompiled into the artifact cache. Save them once to drop the bytecode from their assets.", AssetsNeedingSave);
            AssetsNeedingSave = 0;
        }
    }

    void FParticleCompileScheduler::FinishSettledJobs()
    {
        for (int32 i = 0; i < InFlight.Num();)
        {
            const bool bSettled = Algo::AllOf(InFlight[i]->Emitters, [](const TUniquePtr<FParticleEmitterCompileJob>& Emitter)
            {
                return Emitter->Waiter.IsSettled();
            });
            if (!bSettled)
            {
                ++i;
                continue;
            }

            Finish(*InFlight[i]);
            InFlight.RemoveAt(i);
        }
    }

    void FParticleCompileScheduler::LaunchJobs()
    {
        // A quarter of the workers stays free, so the editor's own frame work is never starved by a recompile burst.
        const uint32 Budget = Math::Max(1u, Jobs::GetNumWorkers() * 3u / 4u);
        FParticleCompileQueue& Queue = TryRender()->GetParticleCompileQueue();

        while (static_cast<uint32>(InFlight.Num()) < Budget)
        {
            TStrongObjectPtr<CParticleSystem> System = Queue.PopRecompile();
            if (!System.IsValid())
            {
                return;
            }

            TUniquePtr<FJob> Job = MakeUnique<FJob>();
            for (int32 Index = 0; Index < System->Emitters.Num(); ++Index)
            {
                const CParticleEmitter* Emitter = System->Emitters[Index].Get();
                if (Emitter == nullptr || !Emitter->NeedsShaderCompile())
                {
                    continue;
                }

                // Without its saved stack the compile would author a starter one in its place.
                if (ParticleOps::FindStack(System.Get(), Index) == nullptr)
                {
                    LOG_WARN("Particle emitter '{0}' in '{1}' has no saved module stack to recompile from", *Emitter->EmitterName, *System->GetName());
                    continue;
                }

                TUniquePtr<FParticleEmitterCompileJob> EmitterJob = MakeUnique<FParticleEmitterCompileJob>();
                FParticleEmitterCompileOutput Output;
                const uint64 CodeHashBefore = Emitter->CompiledCodeHash;
                if (!ParticleOps::BeginCompileEmitter(System.Get(), Index, *EmitterJob, Output, Jobs::EJobPriority::Background))
                {
                    for (const FString& Error : Output.Errors)
                    {
                        LOG_ERROR("Particle emitter '{0}' in '{1}' failed to recompile. {2}", *Emitter->EmitterName, *System->GetName(), *Error);
                    }
                    continue;
                }
                Job->Emitters.Add(Move(EmitterJob));
                Job->CodeHashesBefore.Add(CodeHashBefore);
            }

            if (!Job->Emitters.IsEmpty())
            {
                Job->System = Move(System);
                InFlight.Add(Move(Job));
            }
        }
    }

    void FParticleCompileScheduler::Finish(FJob& Job)
    {
        CParticleSystem* System = Job.System.Get();
        bool bFailed = false;
        for (int32 i = 0; i < Job.Emitters.Num(); ++i)
        {
            FParticleEmitterCompileJob& EmitterJob = *Job.Emitters[i];
            CParticleEmitter* Emitter = EmitterJob.EmitterIndex < System->Emitters.Num() ? System->Emitters[EmitterJob.EmitterIndex].Get() : nullptr;

            FParticleEmitterCompileOutput Output;
            if (!ParticleOps::FinishCompileEmitter(EmitterJob, Output))
            {
                for (const FString& Error : Output.Errors)
                {
                    LOG_ERROR("Particle system '{0}' emitter {1} failed to recompile. {2}", *System->GetName(), EmitterJob.EmitterIndex, *Error);
                }
                bFailed = true;
                continue;
            }

            // A template edit alone only refreshed the artifact cache, so the asset stays clean for source control.
            CPackage* Package = Emitter != nullptr ? Emitter->GetPackage() : nullptr;
            if (Package != nullptr && (Emitter->bLoadedEmbeddedBinaries || Emitter->CompiledCodeHash != Job.CodeHashesBefore[i]))
            {
                AssetsNeedingSave += Package->IsDirty() ? 0 : 1;
                Package->MarkDirty();
            }
        }

        if (bFailed)
        {
            ImGuiX::Notifications::NotifyError("Particle system '{0}' failed to recompile", *System->GetName());
        }
    }
}
