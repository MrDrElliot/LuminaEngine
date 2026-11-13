#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "World/World.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/Component.h"

using namespace Lumina;

extern "C" void* LuminaSharp_QueryViewBegin(uint64 World, const void* const* IncludeOps, int NInc, uint32 WriteMask, const void* const* ExcludeOps, int NExc,
                                            const void* const* RequireOps, int NReq, const void* const* FilterOps, int NFilter, uint32 AddedMask, uint32 Since, uint32* OutStampTick);
extern "C" int LuminaSharp_ViewNextChunk(void* StatePtr, uint32* OutEntities, void** OutPtrs, uint32** OutTicks, int MaxCount, int NInclude);
extern "C" void LuminaSharp_ViewEnd(void* StatePtr);

namespace DotNetViewTestsInternal
{
    struct FViewPosition
    {
        int32 Value = 0;
    };

    struct FViewVelocity
    {
        int32 Value = 0;
    };
}

using namespace DotNetViewTestsInternal;

// Both filters changed is the only match, and each row's payloads and tick slots point at the entity's own dense slot, whether or not it sits in its snapshot slot.
TEST(DotNetView, WriteTrackedFilteredQueryHandsOutEachRowsOwnSlots)
{
    CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    Registry.TrackChanges<FViewPosition>();
    Registry.TrackChanges<FViewVelocity>();

    ECS::FEntity Entities[4];
    for (ECS::FEntity& Entity : Entities)
    {
        Entity = Registry.Create();
        Registry.Emplace<FViewPosition>(Entity);
        Registry.Emplace<FViewVelocity>(Entity);
    }
    const ECS::FEntity PositionOnly = Registry.Create();
    Registry.Emplace<FViewPosition>(PositionOnly);

    const uint32 Since = ECS::FSparseSet::AdvanceChangeTick();
    for (const ECS::FEntity Entity : { Entities[0], Entities[2], Entities[3], PositionOnly })
    {
        Registry.Get<ECS::Write<FViewPosition>>(Entity).Value = 1;
    }
    for (const ECS::FEntity Entity : { Entities[0], Entities[1], Entities[3] })
    {
        Registry.Get<ECS::Write<FViewVelocity>>(Entity).Value = 1;
    }

    const void* Ops[] = { &Meta::GetComponentOps<FViewPosition>(), &Meta::GetComponentOps<FViewVelocity>() };
    uint32 StampTick = 0;
    void* View = LuminaSharp_QueryViewBegin(reinterpret_cast<uint64>(World), Ops, 2, 0b11, nullptr, 0, nullptr, 0, Ops, 2, 0, Since, &StampTick);
    ASSERT_NE(View, nullptr);

    constexpr int MaxCount = 8;
    uint32 OutEntities[MaxCount] = {};
    void* OutPtrs[MaxCount * 2] = {};
    uint32* OutTicks[MaxCount * 2] = {};
    const int Count = LuminaSharp_ViewNextChunk(View, OutEntities, OutPtrs, OutTicks, MaxCount, 2);
    LuminaSharp_ViewEnd(View);

    ASSERT_EQ(Count, 2) << "only the rows whose position and velocity both changed match";
    ECS::FSparseSet* Pools[] = { Registry.FindStorage(ECS::GetComponentTypeID<FViewPosition>()), Registry.FindStorage(ECS::GetComponentTypeID<FViewVelocity>()) };
    for (int Row = 0; Row < Count; ++Row)
    {
        const ECS::FEntity Entity = static_cast<ECS::FEntity>(OutEntities[Row]);
        EXPECT_TRUE(Entity == Entities[0] || Entity == Entities[3]);
        for (int k = 0; k < 2; ++k)
        {
            const uint32 DenseIndex = Pools[k]->FindLiveDense(Entity);
            EXPECT_EQ(OutPtrs[Row * 2 + k], Pools[k]->GetRaw(Entity));
            EXPECT_EQ(OutTicks[Row * 2 + k], Pools[k]->GetChangeTickData() + DenseIndex) << "a written include hands out the row's own change-tick slot";
        }
    }
    EXPECT_NE(OutEntities[0], OutEntities[1]);
}
