#include <gtest/gtest.h>
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/ContainerOps.h"
#include "Containers/String.h"

using namespace Lumina;

namespace
{
    // A fixed container reflects as a TVector, so its type-erased table must still be its own.
    template <typename TContainer>
    void ExerciseVectorOps(int32 Count)
    {
        const FVectorOps* Ops = GetVectorOpsFor<TContainer>();
        ASSERT_NE(Ops, nullptr);
        EXPECT_EQ(Ops->ElementSize, sizeof(typename TContainer::value_type));

        alignas(TContainer) uint8 Storage[sizeof(TContainer)];
        Ops->ConstructContainer(Storage, Ops->ContainerContext);
        EXPECT_EQ(Ops->Num(Storage), 0);

        for (int32 i = 0; i < Count; ++i)
        {
            const int32 Value = i;
            Ops->Add(Storage, &Value);
        }
        EXPECT_EQ(Ops->Num(Storage), Count);

        // Data() must address the live storage, inline buffer or spilled heap block alike.
        const int32* Data = static_cast<const int32*>(Ops->GetData(Storage));
        for (int32 i = 0; i < Count; ++i)
        {
            EXPECT_EQ(Data[i], i);
        }

        Ops->Swap(Storage, 0, Count - 1);
        Data = static_cast<const int32*>(Ops->GetData(Storage));
        EXPECT_EQ(Data[0], Count - 1);
        EXPECT_EQ(Data[Count - 1], 0);

        Ops->RemoveAt(Storage, 0);
        EXPECT_EQ(Ops->Num(Storage), Count - 1);

        Ops->SetNum(Storage, Count);
        EXPECT_EQ(Ops->Num(Storage), Count);

        Ops->Reset(Storage);
        EXPECT_EQ(Ops->Num(Storage), 0);

        Ops->Add(Storage, nullptr);
        EXPECT_EQ(Ops->Num(Storage), 1);

        Ops->DestructContainer(Storage, Ops->ContainerContext);
    }
}

TEST(ContainerOps, VectorOpsDriveAGrowableVector)
{
    ExerciseVectorOps<TVector<int32>>(6);
}

TEST(ContainerOps, VectorOpsDriveAFixedVectorWithinCapacity)
{
    ExerciseVectorOps<TInlineVector<int32, 8>>(6);
}

// Growing past the inline buffer needs the fixed container's own table, or it frees inline.
TEST(ContainerOps, VectorOpsDriveAFixedVectorPastItsInlineCapacity)
{
    ExerciseVectorOps<TInlineVector<int32, 4>>(9);
}

TEST(ContainerOps, FixedVectorOpsHandleNonTrivialElements)
{
    using ContainerType = TInlineVector<FString, 2>;
    const FVectorOps* Ops = GetVectorOpsFor<ContainerType>();

    alignas(ContainerType) uint8 Storage[sizeof(ContainerType)];
    Ops->ConstructContainer(Storage, Ops->ContainerContext);

    const FString First("inline");
    const FString Spilled("overflowed onto the heap, long enough to allocate");
    Ops->Add(Storage, &First);
    Ops->Add(Storage, &First);
    Ops->Add(Storage, &Spilled);

    EXPECT_EQ(Ops->Num(Storage), 3);
    const FString* Data = static_cast<const FString*>(Ops->GetData(Storage));
    EXPECT_EQ(Data[0], First);
    EXPECT_EQ(Data[2], Spilled);

    Ops->DestructContainer(Storage, Ops->ContainerContext);
}

namespace
{
    template <typename TContainer>
    void ExerciseMapOps(int32 Count)
    {
        const FMapOps* Ops = GetMapOpsFor<TContainer>();
        ASSERT_NE(Ops, nullptr);
        EXPECT_EQ(Ops->KeySize, sizeof(typename TContainer::key_type));
        EXPECT_EQ(Ops->ValueSize, sizeof(typename TContainer::mapped_type));

        alignas(TContainer) uint8 Storage[sizeof(TContainer)];
        Ops->ConstructContainer(Storage, Ops->ContainerContext);
        EXPECT_EQ(Ops->Num(Storage), 0);

        for (int32 i = 0; i < Count; ++i)
        {
            const int32 Key = i;
            const int32 Value = i * 10;
            Ops->Add(Storage, &Key, &Value);
        }
        EXPECT_EQ(Ops->Num(Storage), Count);

        const int32 Probe = Count - 1;
        const void* Found = Ops->Find(Storage, &Probe);
        ASSERT_NE(Found, nullptr);
        EXPECT_EQ(*static_cast<const int32*>(Found), Probe * 10);

        EXPECT_TRUE(Ops->Remove(Storage, &Probe));
        EXPECT_EQ(Ops->Num(Storage), Count - 1);
        EXPECT_EQ(Ops->Find(Storage, &Probe), nullptr);

        // Next resumes from its cursor, so a walk reaches every remaining pair once, skipping the erased slot.
        int32 Cursor = 0;
        int32 Visited = 0;
        const void* Key = nullptr;
        void* Value = nullptr;
        while (Ops->Next(Storage, &Cursor, &Key, &Value))
        {
            EXPECT_EQ(*static_cast<const int32*>(Value), *static_cast<const int32*>(Key) * 10);
            ++Visited;
        }
        EXPECT_EQ(Visited, Count - 1);

        Ops->Reset(Storage);
        EXPECT_EQ(Ops->Num(Storage), 0);

        Ops->DestructContainer(Storage, Ops->ContainerContext);
    }
}

TEST(ContainerOps, MapOpsDriveAGrowableHashMap)
{
    ExerciseMapOps<THashMap<int32, int32>>(5);
}

TEST(ContainerOps, MapOpsDriveAFixedHashMapPastItsInlineCapacity)
{
    ExerciseMapOps<TInlineHashMap<int32, int32, 4>>(9);
}
