#include "EditorPCH.h"
#include "ParticleSystemFactory.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"

namespace Lumina
{
    CObject* CParticleSystemFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CParticleSystem* System = NewObject<CParticleSystem>(Package, Name);
        if (System != nullptr)
        {
            // PostLoad guarantees an emitter but never runs on a fresh asset, which would open with none.
            System->AddEmitter();
        }
        return System;
    }

    CObject* CParticleEmitterFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        CParticleEmitter* Emitter = NewObject<CParticleEmitter>(Package, Name);
        if (Emitter != nullptr)
        {
            // The header every system sharing it shows, which would otherwise read just "Emitter".
            Emitter->EmitterName = *Name;
        }
        return Emitter;
    }
}
