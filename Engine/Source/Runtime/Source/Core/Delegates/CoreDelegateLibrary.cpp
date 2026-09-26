#include "RuntimePCH.h"
#include "CoreDelegateLibrary.h"

#include "CoreDelegates.h"

namespace Lumina
{
    int64 CCoreDelegateLibrary::GetCoreDelegates()
    {
        return (int64)&FCoreDelegates::Get();
    }
}
