#include "EditorPCH.h"
#include "ParticleSystemFactory.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"

namespace Lumina
{
    CObject* CParticleSystemFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CParticleSystem>(Package, Name);
    }
}
