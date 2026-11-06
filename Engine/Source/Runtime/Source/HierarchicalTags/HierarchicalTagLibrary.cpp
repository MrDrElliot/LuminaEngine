#include "RuntimePCH.h"

#include "HierarchicalTagLibrary.h"

#include "HierarchicalTags/HierarchicalTagComponent.h"
#include "HierarchicalTags/HierarchicalTagRegistry.h"

namespace Lumina
{
    uint32 CHierarchicalTagLibrary::RequestTag(const FString& Name)
    {
        return Name.IsEmpty() ? 0u : FHierarchicalTagRegistry::Get().RequestTag(FStringView(*Name, Name.Len()));
    }

    FString CHierarchicalTagLibrary::GetTagName(uint32 TagId)
    {
        return FHierarchicalTagRegistry::Get().GetName(TagId);
    }

    bool CHierarchicalTagLibrary::Matches(uint32 A, uint32 B)
    {
        return FHierarchicalTagRegistry::Get().Matches(A, B);
    }

    bool CHierarchicalTagLibrary::MatchesExact(uint32 A, uint32 B)
    {
        return FHierarchicalTagRegistry::Get().MatchesExact(A, B);
    }

    uint32 CHierarchicalTagLibrary::GetParent(uint32 TagId)
    {
        return FHierarchicalTagRegistry::Get().GetParent(TagId);
    }

    bool CHierarchicalTagLibrary::IsValidTag(uint32 TagId)
    {
        return FHierarchicalTagRegistry::Get().IsValid(TagId);
    }
}
