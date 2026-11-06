#include "RuntimePCH.h"
#include "ParticleCompileQueue.h"
#include "ParticleSystem.h"

namespace Lumina
{
    FParticleCompileQueue::FParticleCompileQueue() = default;
    FParticleCompileQueue::~FParticleCompileQueue() = default;

    void FParticleCompileQueue::QueueRecompile(CParticleSystem* System)
    {
        FScopeLock Lock(Mutex);
        if (Queued.TryAdd(System))
        {
            Recompiles.Add(System);
        }
    }

    TStrongObjectPtr<CParticleSystem> FParticleCompileQueue::PopRecompile()
    {
        FScopeLock Lock(Mutex);
        if (Recompiles.IsEmpty())
        {
            return nullptr;
        }
        TStrongObjectPtr<CParticleSystem> System = Move(Recompiles.Last());
        Recompiles.Pop();
        Queued.Remove(System.Get());
        return System;
    }
}
