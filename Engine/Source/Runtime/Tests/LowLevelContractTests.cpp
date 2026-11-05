#include <gtest/gtest.h>
#include "Containers/SparseArray.h"
#include "Containers/String.h"
#include "Containers/Variant.h"
#include "Core/Templates/Align.h"
#include "Memory/Memory.h"
#include <thread>

namespace LuminaLowLevelContractTests
{
    struct FTrackedElement
    {
        inline static int Live = 0;
        const FTrackedElement* Self = this;
        int Value;

        explicit FTrackedElement(int InValue) : Value(InValue) { ++Live; }
        FTrackedElement(const FTrackedElement& Other) : Value(Other.Value) { ++Live; }
        FTrackedElement(FTrackedElement&& Other) noexcept : Value(Other.Value) { ++Live; }
        ~FTrackedElement()
        {
            EXPECT_EQ(Self, this);
            --Live;
        }
    };

    TEST(LowLevelContracts, SparseArrayPreservesObjectLifetimes)
    {
        EXPECT_EQ(FTrackedElement::Live, 0);
        {
            Lumina::TSparseArray<FTrackedElement> Values;
            for (int Index = 0; Index < 128; ++Index)
            {
                Values.Emplace(Index);
            }
            Values.Reserve(1024);
            Values.Shrink();
            for (const auto& Value : Values)
            {
                EXPECT_EQ(Value.Self, &Value);
            }
            EXPECT_EQ(FTrackedElement::Live, 128);
            Values.RemoveAt(0);
            Values.RemoveAt(63);
            Values.RemoveAt(127);
            auto Copy = Values;
            EXPECT_EQ(Copy.Num(), 125);
            EXPECT_EQ(Copy.GetMaxIndex(), Values.GetMaxIndex());
            EXPECT_EQ(Copy.TryGet(0), nullptr);
            EXPECT_EQ(Copy.TryGet(63), nullptr);
            EXPECT_EQ(Copy.TryGet(127), nullptr);
            EXPECT_EQ(Copy.Emplace(999), 127);
            EXPECT_EQ(Copy[127].Value, 999);
            Values.Compact();
            EXPECT_EQ(Values.Num(), 125);
            EXPECT_EQ(Values.NumFree(), 0);
            EXPECT_EQ(Values[0].Value, 1);
            Lumina::TSparseArray<FTrackedElement>::const_iterator Iterator = Values.begin();
            EXPECT_EQ(Iterator->Value, 1);
            auto Moved = std::move(Copy);
            EXPECT_TRUE(Copy.IsEmpty());
            EXPECT_EQ(Moved[127].Value, 999);
        }
        EXPECT_EQ(FTrackedElement::Live, 0);
    }

    TEST(LowLevelContracts, SparseArrayCopiesOnlyHoles)
    {
        Lumina::TSparseArray<int> Values;
        Values.Emplace(42);
        Values.RemoveAt(0);
        auto Copy = Values;
        EXPECT_TRUE(Copy.IsEmpty());
        EXPECT_EQ(Copy.GetMaxIndex(), 1);
        EXPECT_EQ(Copy.Emplace(7), 0);
        EXPECT_EQ(Copy[0], 7);
    }

    struct FLayout
    {
        char Tag;
        uint64 Value;
    };

    static_assert(LE_OFFSETOF(FLayout, Value) == offsetof(FLayout, Value));
    static_assert(LE_INCLUDES_MEMBER(sizeof(FLayout), FLayout, Value));
    static_assert(!LE_INCLUDES_MEMBER(LE_OFFSETOF(FLayout, Value) + sizeof(uint64) - 1, FLayout, Value));
    static_assert(!Lumina::IncludesMember(1, 2, size_t(-1)));
    constexpr int Items[] = {1, 2, 3};
    static_assert(LE_COUNTOF(Items) == 3);

    TEST(LowLevelContracts, ThrowingAllocationRejectsAllocatorFailure)
    {
        constexpr auto UnsupportedAlignment = std::align_val_t(size_t(1) << 30);
        EXPECT_THROW((void)::operator new(16, UnsupportedAlignment), std::bad_alloc);
        EXPECT_THROW((void)::operator new[](16, UnsupportedAlignment), std::bad_alloc);
        void* Allocation = ::operator new(0);
        EXPECT_NE(Allocation, nullptr);
        ::operator delete(Allocation);
    }

    struct FStringOwner
    {
        Lumina::FString Text;
    };

    TEST(LowLevelContracts, VariantAssignmentPreservesAliasedSources)
    {
        const Lumina::FString Expected(256, 'x');
        Lumina::Containers::TVariant<Lumina::FString, int> Value(Expected);
        Value = Value.Get<Lumina::FString>();
        EXPECT_EQ(Value.Get<Lumina::FString>(), Expected);
        Value = std::move(Value.Get<Lumina::FString>());
        EXPECT_EQ(Value.Get<Lumina::FString>(), Expected);
        Lumina::Containers::TVariant<FStringOwner, Lumina::FString> Owner(FStringOwner{Expected});
        Owner = Owner.Get<FStringOwner>().Text;
        EXPECT_EQ(Owner.Get<Lumina::FString>(), Expected);
        Lumina::Containers::TVariant<FTrackedElement, int> Nonassignable(std::in_place_index<0>, 42);
        Nonassignable = Nonassignable.Get<FTrackedElement>();
        EXPECT_EQ(Nonassignable.Get<FTrackedElement>().Value, 42);
        EXPECT_EQ(Nonassignable.Get<FTrackedElement>().Self, &Nonassignable.Get<FTrackedElement>());
    }

    struct alignas(64) FArrayElement
    {
        inline static int Live = 0;
        inline static int Attempts = 0;
        inline static int ThrowAt = -1;
        FArrayElement()
        {
            if (Attempts++ == ThrowAt)
            {
                throw std::runtime_error("array construction failure");
            }
            ++Live;
        }
        ~FArrayElement() { --Live; }
    };

    TEST(LowLevelContracts, ArrayAllocationChecksSizeAndDestroysElements)
    {
        const size_t OverflowCount = std::numeric_limits<size_t>::max() / sizeof(uint64) + 1;
        EXPECT_THROW((void)Lumina::Memory::NewArray<uint64>(OverflowCount), std::bad_array_new_length);
        const uint64 InitialValue = 7;
        EXPECT_THROW((void)Lumina::Memory::NewArray<uint64>(OverflowCount, InitialValue), std::bad_array_new_length);
        EXPECT_THROW((void)Lumina::Memory::NewArray<uint8>(std::numeric_limits<size_t>::max()), std::bad_array_new_length);
        auto* Values = Lumina::Memory::NewArray<uint64>(3, InitialValue);
        EXPECT_EQ(Values[0], 7u);
        EXPECT_EQ(Values[2], 7u);
        Lumina::Memory::DeleteArray(Values);
        Lumina::Memory::DeleteArray(Lumina::Memory::NewArray<uint64>(0));
        Lumina::Memory::DeleteArray<uint64>(nullptr);
        auto* Aligned = Lumina::Memory::NewArray<FArrayElement>(3);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(Aligned) % alignof(FArrayElement), 0u);
        EXPECT_EQ(FArrayElement::Live, 3);
        Lumina::Memory::DeleteArray(Aligned);
        EXPECT_EQ(FArrayElement::Live, 0);
        FArrayElement::Attempts = 0;
        FArrayElement::ThrowAt = 2;
        EXPECT_THROW((void)Lumina::Memory::NewArray<FArrayElement>(4), std::runtime_error);
        EXPECT_EQ(FArrayElement::Live, 0);
        FArrayElement::ThrowAt = -1;
    }

    TEST(LowLevelContracts, TypedFreeClearsPointerAndDeleteDestroysObject)
    {
        auto* Pointer = static_cast<uint64*>(Lumina::Memory::Malloc(sizeof(uint64)));
        Lumina::Memory::Free(Pointer);
        EXPECT_EQ(Pointer, nullptr);
        Lumina::Memory::Free(Pointer);
        const auto* Object = Lumina::Memory::New<FTrackedElement>(7);
        EXPECT_EQ(FTrackedElement::Live, 1);
        Lumina::Memory::Delete(Object);
        EXPECT_EQ(FTrackedElement::Live, 0);
        Lumina::Memory::Delete<FTrackedElement>(nullptr);
    }

    TEST(LowLevelContracts, ConcurrentHeapInitializationAndAllocation)
    {
        Lumina::TVector<std::thread> Workers;
        for (int Index = 0; Index < 8; ++Index)
        {
            Workers.Emplace([]
            {
                for (int Iteration = 0; Iteration < 100; ++Iteration)
                {
                    Lumina::Memory::InitializeThreadHeap();
                    void* Allocation = Lumina::Memory::MallocOrThrow(64);
                    Lumina::Memory::Free(Allocation);
                }
                Lumina::Memory::ShutdownThreadHeap();
            });
        }
        for (auto& Worker : Workers)
        {
            Worker.join();
        }
    }
}
