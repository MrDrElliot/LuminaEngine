#pragma once

#include "World/ECS/Registry.h"

#include "Containers/Vector.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    // Indexed by ECS::FEntity::GetIndex(), sparse, an entry reads as missing unless this pass stamped it for the same entity.
    template<typename TEntry>
    struct TStampedEntityCache
    {
        TVector<TEntry> ByEntityIndex;

        uint32          Stamp = 0;

        // Bumps the stamp and grows to cover every consumer view, false when none of them has rows.
        template<typename... TViews>
        bool BeginPass(const TViews&... ConsumerViews)
        {
            ++Stamp;

            uint32 MaxIndex = 0;
            bool bAnyConsumer = false;
            ([&](const auto& View)
            {
                if (!View.IsEmpty())
                {
                    bAnyConsumer = true;
                    MaxIndex = Math::Max(MaxIndex, View.MaxLiveIndex());
                }
            }(ConsumerViews), ...);

            if (bAnyConsumer && (uint32)ByEntityIndex.Num() <= MaxIndex)
            {
                ByEntityIndex.SetNum(IntCastChecked<int32>(static_cast<int64>(MaxIndex) + 1u));
            }
            return bAnyConsumer;
        }

        NODISCARD bool IsFresh(const TEntry& Entry, ECS::FEntity Entity) const
        {
            return Entry.Owner == Entity && Entry.Stamp == Stamp;
        }

        NODISCARD const TEntry* Find(ECS::FEntity Entity) const
        {
            const uint32 Index = Entity.GetIndex();
            if (Index >= (uint32)ByEntityIndex.Num())
            {
                return nullptr;
            }

            const TEntry& Entry = ByEntityIndex[Index];
            return IsFresh(Entry, Entity) ? &Entry : nullptr;
        }

        NODISCARD TEntry* Find(ECS::FEntity Entity)
        {
            return const_cast<TEntry*>(static_cast<const TStampedEntityCache&>(*this).Find(Entity));
        }
    };
}
