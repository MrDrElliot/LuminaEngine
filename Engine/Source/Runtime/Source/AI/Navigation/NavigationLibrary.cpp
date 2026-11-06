#include "RuntimePCH.h"

#include "NavigationLibrary.h"

namespace Lumina
{
    FString CNavigationLibrary::DescribePathResult(ENavPathResult Result)
    {
        return FString(ToString(Result));
    }
}
