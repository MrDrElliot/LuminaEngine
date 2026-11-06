#include "RuntimePCH.h"
#include "HierarchicalTag.h"

#include "HierarchicalTags/HierarchicalTagRegistry.h"

namespace Lumina
{
    namespace HierarchicalTagInternal
    {
        void SetBit(TInlineVector<uint64, 2>& Words, uint32 Bit)
        {
            const int32 Word = static_cast<int32>(Bit >> 6);
            if (Word >= Words.Num())
            {
                Words.SetNumZeroed(Word + 1);
            }
            Words[Word] |= uint64(1) << (Bit & 63);
        }

        bool TestBit(const TInlineVector<uint64, 2>& Words, uint32 Bit)
        {
            const int32 Word = static_cast<int32>(Bit >> 6);
            return Bit != 0 && Word < Words.Num() && ((Words[Word] >> (Bit & 63)) & 1) != 0;
        }
    }

    uint32 SHierarchicalTag::GetId() const
    {
        return FHierarchicalTagRegistry::Get().RequestTag(TagName);
    }

    bool SHierarchicalTag::MatchesTag(const SHierarchicalTag& Other) const
    {
        return FHierarchicalTagRegistry::Get().Matches(GetId(), Other.GetId());
    }

    bool SHierarchicalTag::MatchesTagExact(const SHierarchicalTag& Other) const
    {
        return !TagName.IsNone() && TagName == Other.TagName;
    }

    SHierarchicalTag SHierarchicalTag::GetParent() const
    {
        FHierarchicalTagRegistry& Registry = FHierarchicalTagRegistry::Get();
        SHierarchicalTag Parent;
        Parent.TagName = Registry.GetTagName(Registry.GetParent(GetId()));
        return Parent;
    }

    void SHierarchicalTagContainer::AddTag(const SHierarchicalTag& Tag)
    {
        if (Tag.IsValid() && !HasTagExact(Tag))
        {
            Tags.Add(Tag);
        }
    }

    void SHierarchicalTagContainer::RemoveTag(const SHierarchicalTag& Tag)
    {
        Tags.RemoveSingle(Tag);
    }

    bool SHierarchicalTagContainer::HasTag(const SHierarchicalTag& Tag) const
    {
        const uint32 Wanted = Tag.GetId();
        if (Wanted == 0)
        {
            return false;
        }
        FHierarchicalTagRegistry& Registry = FHierarchicalTagRegistry::Get();
        return Algo::AnyOf(Tags, [&](const SHierarchicalTag& Owned) { return Registry.Matches(Owned.GetId(), Wanted); });
    }

    bool SHierarchicalTagContainer::HasTagExact(const SHierarchicalTag& Tag) const
    {
        return Algo::Contains(Tags, Tag);
    }

    bool SHierarchicalTagContainer::HasAny(const SHierarchicalTagContainer& Other) const
    {
        return Algo::AnyOf(Other.Tags, [this](const SHierarchicalTag& Tag) { return HasTag(Tag); });
    }

    bool SHierarchicalTagContainer::HasAll(const SHierarchicalTagContainer& Other) const
    {
        return Algo::AllOf(Other.Tags, [this](const SHierarchicalTag& Tag) { return HasTag(Tag); });
    }

    FHierarchicalTagMask::FHierarchicalTagMask(const SHierarchicalTagContainer& Container)
    {
        for (const SHierarchicalTag& Tag : Container.Tags)
        {
            AddTag(Tag);
        }
    }

    void FHierarchicalTagMask::AddTag(const SHierarchicalTag& Tag)
    {
        AddTag(Tag.GetId());
    }

    void FHierarchicalTagMask::AddTag(uint32 TagId)
    {
        FHierarchicalTagRegistry& Registry = FHierarchicalTagRegistry::Get();
        if (!Registry.IsValid(TagId))
        {
            return;
        }
        HierarchicalTagInternal::SetBit(Explicit, TagId);
        for (uint32 Current = TagId; Current != 0; Current = Registry.GetParent(Current))
        {
            HierarchicalTagInternal::SetBit(Implied, Current);
        }
    }

    void FHierarchicalTagMask::Reset()
    {
        Explicit.Reset();
        Implied.Reset();
    }

    bool FHierarchicalTagMask::HasTag(const SHierarchicalTag& Tag) const
    {
        return HasTag(FHierarchicalTagRegistry::Get().FindTag(Tag.TagName));
    }

    bool FHierarchicalTagMask::HasTag(uint32 TagId) const
    {
        return HierarchicalTagInternal::TestBit(Implied, TagId);
    }

    bool FHierarchicalTagMask::HasTagExact(const SHierarchicalTag& Tag) const
    {
        return HasTagExact(FHierarchicalTagRegistry::Get().FindTag(Tag.TagName));
    }

    bool FHierarchicalTagMask::HasTagExact(uint32 TagId) const
    {
        return HierarchicalTagInternal::TestBit(Explicit, TagId);
    }

    bool FHierarchicalTagMask::HasAny(const FHierarchicalTagMask& Other) const
    {
        const int32 Words = Math::Min(Implied.Num(), Other.Explicit.Num());
        for (int32 Word = 0; Word < Words; ++Word)
        {
            if ((Implied[Word] & Other.Explicit[Word]) != 0)
            {
                return true;
            }
        }
        return false;
    }

    bool FHierarchicalTagMask::HasAll(const FHierarchicalTagMask& Other) const
    {
        for (int32 Word = 0; Word < Other.Explicit.Num(); ++Word)
        {
            const uint64 Mine = Word < Implied.Num() ? Implied[Word] : 0;
            if ((Other.Explicit[Word] & ~Mine) != 0)
            {
                return false;
            }
        }
        return true;
    }
}
