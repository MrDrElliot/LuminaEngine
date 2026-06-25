#include "pch.h"
#include "DelegateProperty.h"
#include "Containers/String.h"

namespace Lumina
{
    bool FDelegateProperty::Identical(const void* ValueA, const void* ValueB) const
    {
        return true;
    }

    void FDelegateProperty::CopyCompleteValue(void* Dst, const void* Src) const
    {
    }

    FString FDelegateProperty::ToString(const void* Data) const
    {
        return "<delegate>";
    }
}
