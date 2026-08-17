#include <gtest/gtest.h>
#include "Containers/Array.h"
#include "Containers/ContainerOps.h"
#include "Containers/String.h"

using namespace Lumina;

namespace
{
    // Every op is driven through the type-erased table, the way FArrayProperty and the C# TVector view see a
    // reflected member: a fixed container reflects as a TVector, so its table must still be its own.
    template <typename TContainer>
    void ExerciseVectorOps(SIZE_T Count)
    {
        const FVectorOps* Ops = GetVectorOpsFor<TContainer>();
        ASSERT_NE(Ops, nullptr);
        EXPECT_EQ(Ops->ElementSize, sizeof(typename TContainer::value_type));

        alignas(TContainer) uint8 Storage[sizeof(TContainer)];
        Ops->ConstructContainer(Storage, Ops->ContainerContext);
        EXPECT_EQ(Ops->Size(Storage), 0u);

        for (SIZE_T i = 0; i < Count; ++i)
        {
            const int32 Value = (int32)i;
            Ops->PushBack(Storage, &Value);
        }
        EXPECT_EQ(Ops->Size(Storage), Count);

        // Data() must address the live storage, inline buffer or spilled heap block alike.
        const int32* Data = static_cast<const int32*>(Ops->Data(Storage));
        for (SIZE_T i = 0; i < Count; ++i)
        {
            EXPECT_EQ(Data[i], (int32)i);
        }

        Ops->Swap(Storage, 0, Count - 1);
        Data = static_cast<const int32*>(Ops->Data(Storage));
        EXPECT_EQ(Data[0], (int32)(Count - 1));
        EXPECT_EQ(Data[Count - 1], 0);

        Ops->RemoveAt(Storage, 0);
        EXPECT_EQ(Ops->Size(Storage), Count - 1);

        Ops->Resize(Storage, Count);
        EXPECT_EQ(Ops->Size(Storage), Count);

        Ops->Clear(Storage);
        EXPECT_EQ(Ops->Size(Storage), 0u);

        Ops->PushBack(Storage, nullptr);
        EXPECT_EQ(Ops->Size(Storage), 1u);

        Ops->DestructContainer(Storage, Ops->ContainerContext);
    }
}

TEST(ContainerOps, VectorOpsDriveAGrowableVector)
{
    ExerciseVectorOps<TVector<int32>>(6);
}

TEST(ContainerOps, VectorOpsDriveAFixedVectorWithinCapacity)
{
    ExerciseVectorOps<TFixedVector<int32, 8>>(6);
}

// The interesting case: the table grows the container past its inline buffer, so it must be the fixed
// container's own table. A TVector table here would free the inline buffer as if it were a heap block.
TEST(ContainerOps, VectorOpsDriveAFixedVectorPastItsInlineCapacity)
{
    ExerciseVectorOps<TFixedVector<int32, 4>>(9);
}

TEST(ContainerOps, FixedVectorOpsHandleNonTrivialElements)
{
    using ContainerType = TFixedVector<FString, 2>;
    const FVectorOps* Ops = GetVectorOpsFor<ContainerType>();

    alignas(ContainerType) uint8 Storage[sizeof(ContainerType)];
    Ops->ConstructContainer(Storage, Ops->ContainerContext);

    const FString First("inline");
    const FString Spilled("overflowed onto the heap, long enough to allocate");
    Ops->PushBack(Storage, &First);
    Ops->PushBack(Storage, &First);
    Ops->PushBack(Storage, &Spilled);

    EXPECT_EQ(Ops->Size(Storage), 3u);
    const FString* Data = static_cast<const FString*>(Ops->Data(Storage));
    EXPECT_EQ(Data[0], First);
    EXPECT_EQ(Data[2], Spilled);

    Ops->DestructContainer(Storage, Ops->ContainerContext);
}

namespace
{
    template <typename TContainer>
    void ExerciseMapOps(SIZE_T Count)
    {
        const FMapOps* Ops = GetMapOpsFor<TContainer>();
        ASSERT_NE(Ops, nullptr);
        EXPECT_EQ(Ops->KeySize, sizeof(typename TContainer::key_type));
        EXPECT_EQ(Ops->ValueSize, sizeof(typename TContainer::mapped_type));

        alignas(TContainer) uint8 Storage[sizeof(TContainer)];
        Ops->ConstructContainer(Storage, Ops->ContainerContext);
        EXPECT_EQ(Ops->Size(Storage), 0u);

        for (SIZE_T i = 0; i < Count; ++i)
        {
            const int32 Key = (int32)i;
            const int32 Value = (int32)(i * 10);
            Ops->Insert(Storage, &Key, &Value);
        }
        EXPECT_EQ(Ops->Size(Storage), Count);

        const int32 Probe = (int32)(Count - 1);
        const void* Found = Ops->Find(Storage, &Probe);
        ASSERT_NE(Found, nullptr);
        EXPECT_EQ(*static_cast<const int32*>(Found), Probe * 10);

        EXPECT_TRUE(Ops->RemoveByKey(Storage, &Probe));
        EXPECT_EQ(Ops->Size(Storage), Count - 1);
        EXPECT_EQ(Ops->Find(Storage, &Probe), nullptr);

        Ops->Clear(Storage);
        EXPECT_EQ(Ops->Size(Storage), 0u);

        Ops->DestructContainer(Storage, Ops->ContainerContext);
    }
}

TEST(ContainerOps, MapOpsDriveAGrowableHashMap)
{
    ExerciseMapOps<THashMap<int32, int32>>(5);
}

TEST(ContainerOps, MapOpsDriveAFixedHashMapPastItsInlineCapacity)
{
    ExerciseMapOps<TFixedHashMap<int32, int32, 4>>(9);
}
