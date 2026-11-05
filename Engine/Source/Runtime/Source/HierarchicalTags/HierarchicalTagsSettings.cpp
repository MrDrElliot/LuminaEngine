#include "RuntimePCH.h"
#include "HierarchicalTagsSettings.h"

#include "HierarchicalTags/HierarchicalTagRegistry.h"

namespace Lumina
{
    void CHierarchicalTagsSettings::PostInitSettings()
    {
        for (const FString& Tag : Tags)
        {
            FHierarchicalTagRegistry::Get().RequestTag(FStringView(*Tag, Tag.Len()));
        }
    }
}
