#include "RuntimePCH.h"
#include "GameplayTag.h"

#include "GameplayTags/GameplayTagRegistry.h"

namespace Lumina
{
    namespace
    {
        // Resolve a tag's FName to its registry id (interning it on first use).
        uint32 IdOf(const FName& Name)
        {
            return Name.IsNone() ? 0u : FGameplayTagRegistry::Get().RequestTag(FStringView(Name.c_str()));
        }
    }

    bool FGameplayTag::MatchesTag(const FGameplayTag& Other) const
    {
        return FGameplayTagRegistry::Get().Matches(IdOf(TagName), IdOf(Other.TagName));
    }

    bool FGameplayTag::MatchesTagExact(const FGameplayTag& Other) const
    {
        return !TagName.IsNone() && TagName == Other.TagName;
    }

    FGameplayTag FGameplayTag::GetParent() const
    {
        FGameplayTag Parent;
        const uint32 ParentId = FGameplayTagRegistry::Get().GetParent(IdOf(TagName));
        if (ParentId != 0)
        {
            Parent.TagName = FName(FGameplayTagRegistry::Get().GetName(ParentId).c_str());
        }
        return Parent;
    }

    void FGameplayTagContainer::AddTag(const FGameplayTag& Tag)
    {
        if (Tag.IsValid() && !HasTagExact(Tag))
        {
            Tags.push_back(Tag);
        }
    }

    void FGameplayTagContainer::RemoveTag(const FGameplayTag& Tag)
    {
        for (auto It = Tags.begin(); It != Tags.end(); ++It)
        {
            if (*It == Tag)
            {
                Tags.erase(It);
                return;
            }
        }
    }

    bool FGameplayTagContainer::HasTag(const FGameplayTag& Tag) const
    {
        return Algo::AnyOf(Tags, [&Tag](const FGameplayTag& Owned) { return Owned.MatchesTag(Tag); });
    }

    bool FGameplayTagContainer::HasTagExact(const FGameplayTag& Tag) const
    {
        return Algo::Contains(Tags, Tag);
    }

    bool FGameplayTagContainer::HasAny(const FGameplayTagContainer& Other) const
    {
        return Algo::AnyOf(Other.Tags, [this](const FGameplayTag& Tag) { return HasTag(Tag); });
    }

    bool FGameplayTagContainer::HasAll(const FGameplayTagContainer& Other) const
    {
        return Algo::AllOf(Other.Tags, [this](const FGameplayTag& Tag) { return HasTag(Tag); });
    }
}
