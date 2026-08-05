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
            // PostLoad is what guarantees a loaded system has an emitter, and it does not run on a freshly
            // constructed one -- so a new asset would otherwise open with an empty emitter list.
            System->AddEmitter();
        }
        return System;
    }
}
