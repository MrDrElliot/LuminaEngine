#include "RuntimePCH.h"
#include "GameplayTag.h"

#include "GameplayTags/GameplayTagRegistry.h"

namespace Lumina
{
    FGameplayTagRegistry& FGameplayTagRegistry::Get()
    {
        static FGameplayTagRegistry Instance;
        return Instance;
    }

    namespace
    {
        // Resolve a tag's FName to its registry id (interning it on first use).
        uint32 IdOf(const FName& Name)
        {
            return Name.IsNone() ? 0u : FGameplayTagRegistry::Get().RequestTag(FStringView(Name.c_str()));
        }
    }

    bool SGameplayTag::MatchesTag(const SGameplayTag& Other) const
    {
        return FGameplayTagRegistry::Get().Matches(IdOf(TagName), IdOf(Other.TagName));
    }

    bool SGameplayTag::MatchesTagExact(const SGameplayTag& Other) const
    {
        return !TagName.IsNone() && TagName == Other.TagName;
    }

    SGameplayTag SGameplayTag::GetParent() const
    {
        SGameplayTag Parent;
        const uint32 ParentId = FGameplayTagRegistry::Get().GetParent(IdOf(TagName));
        if (ParentId != 0)
        {
            Parent.TagName = FName(FGameplayTagRegistry::Get().GetName(ParentId).c_str());
        }
        return Parent;
    }

    void SGameplayTagContainer::AddTag(const SGameplayTag& Tag)
    {
        if (Tag.IsValid() && !HasTagExact(Tag))
        {
            Tags.push_back(Tag);
        }
    }

    void SGameplayTagContainer::RemoveTag(const SGameplayTag& Tag)
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

    bool SGameplayTagContainer::HasTag(const SGameplayTag& Tag) const
    {
        return Algo::AnyOf(Tags, [&Tag](const SGameplayTag& Owned) { return Owned.MatchesTag(Tag); });
    }

    bool SGameplayTagContainer::HasTagExact(const SGameplayTag& Tag) const
    {
        return Algo::Contains(Tags, Tag);
    }

    bool SGameplayTagContainer::HasAny(const SGameplayTagContainer& Other) const
    {
        return Algo::AnyOf(Other.Tags, [this](const SGameplayTag& Tag) { return HasTag(Tag); });
    }

    bool SGameplayTagContainer::HasAll(const SGameplayTagContainer& Other) const
    {
        return Algo::AllOf(Other.Tags, [this](const SGameplayTag& Tag) { return HasTag(Tag); });
    }
}
