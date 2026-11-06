#include "RuntimePCH.h"

#include "LayoutRegistry.h"
#include "NameSizeRegistry.h"

namespace Lumina::DotNet
{
    void RegisterLayout(const char* Key, int32 Size)
    {
        NameSizeRegistry::Register(ENameSizeRegistry::Layout, Key, Size);
    }

    int32 GetLayoutSize(const char* Name, int32 Len)
    {
        return NameSizeRegistry::Find(ENameSizeRegistry::Layout, Name, Len);
    }
}
