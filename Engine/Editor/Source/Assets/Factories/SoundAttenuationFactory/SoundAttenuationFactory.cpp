#include "EditorPCH.h"
#include "SoundAttenuationFactory.h"

namespace Lumina
{
    CObject* CSoundAttenuationFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CSoundAttenuation>(Package, Name);
    }
}
