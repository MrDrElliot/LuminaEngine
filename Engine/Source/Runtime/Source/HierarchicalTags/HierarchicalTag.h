#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "HierarchicalTag.generated.h"

namespace Lumina
{
    // A tag authored as a dotted name like "Ability.Fire.Fireball", matched through its ancestors.
    REFLECT()
    struct RUNTIME_API SHierarchicalTag
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        FName TagName;

        bool IsValid() const { return !TagName.IsNone(); }

        // Registers the tag on first use; 0 for an unset tag.
        uint32 GetId() const;

        // This tag is Other or a descendant of it ("Ability.Fire.Fireball" matches "Ability.Fire").
        bool MatchesTag(const SHierarchicalTag& Other) const;

        bool MatchesTagExact(const SHierarchicalTag& Other) const;

        SHierarchicalTag GetParent() const;

        bool operator==(const SHierarchicalTag& Other) const { return TagName == Other.TagName; }
        bool operator!=(const SHierarchicalTag& Other) const { return !(*this == Other); }
    };

    REFLECT()
    struct RUNTIME_API SHierarchicalTagContainer
    {
        GENERATED_BODY()

        PROPERTY(Editable)
        TVector<SHierarchicalTag> Tags;

        void AddTag(const SHierarchicalTag& Tag);
        void RemoveTag(const SHierarchicalTag& Tag);

        // Hierarchical: a "Damage.Fire" entry satisfies HasTag("Damage").
        bool HasTag(const SHierarchicalTag& Tag) const;

        bool HasTagExact(const SHierarchicalTag& Tag) const;

        bool HasAny(const SHierarchicalTagContainer& Other) const;
        bool HasAll(const SHierarchicalTagContainer& Other) const;

        int32 Num() const { return Tags.Num(); }
        bool IsEmpty() const { return Tags.IsEmpty(); }
    };

    // A container baked into bits over tag ids, so every query is a few word operations. It is a snapshot: build it again when the container changes.
    class RUNTIME_API FHierarchicalTagMask
    {
    public:

        FHierarchicalTagMask() = default;
        explicit FHierarchicalTagMask(const SHierarchicalTagContainer& Container);

        void AddTag(const SHierarchicalTag& Tag);
        void AddTag(uint32 TagId);
        void Reset();

        bool HasTag(const SHierarchicalTag& Tag) const;
        bool HasTag(uint32 TagId) const;
        bool HasTagExact(const SHierarchicalTag& Tag) const;
        bool HasTagExact(uint32 TagId) const;

        bool HasAny(const FHierarchicalTagMask& Other) const;
        bool HasAll(const FHierarchicalTagMask& Other) const;

        bool IsEmpty() const { return Explicit.IsEmpty(); }

    private:

        TInlineVector<uint64, 2> Explicit;
        TInlineVector<uint64, 2> Implied;
    };
}
