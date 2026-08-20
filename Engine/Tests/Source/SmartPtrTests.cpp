#include <gtest/gtest.h>

#include <atomic>
#include <vector>

#include "Containers/Vector.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"
#include "Platform/Time/PlatformTime.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaSmartPtrTests
{
    namespace PlatformTime = Lumina::PlatformTime;
    using Lumina::TSharedPtr;
    using Lumina::TWeakPtr;
    using Lumina::TUniquePtr;
    using Lumina::TSharedFromThis;
    using Lumina::MakeShared;
    using Lumina::MakeUnique;
    using Lumina::TVector;
    using Lumina::FThread;
    using Lumina::int32;
    using Lumina::uint32;

    struct FCounted
    {
        static inline int32 Alive = 0;

        int32 Payload = 0;

        explicit FCounted(int32 InPayload = 0) : Payload(InPayload) { ++Alive; }
        FCounted(const FCounted& Other) : Payload(Other.Payload) { ++Alive; }
        ~FCounted() { --Alive; }
    };

    struct FBase
    {
        virtual ~FBase() = default;
        virtual int32 Which() const { return 1; }
    };

    struct FDerived : FBase
    {
        int32 Which() const override { return 2; }
    };

    struct FSelfAware : TSharedFromThis<FSelfAware>
    {
        int32 Payload = 7;
    };

    struct FTrackingDeleter
    {
        static inline int32 Calls = 0;

        void operator()(FCounted* Ptr) const
        {
            ++Calls;
            Lumina::Memory::Delete(Ptr);
        }
    };

    TEST(SharedPtr, DefaultIsEmpty)
    {
        const TSharedPtr<FCounted> Ptr;

        EXPECT_FALSE(Ptr.IsValid());
        EXPECT_FALSE(static_cast<bool>(Ptr));
        EXPECT_EQ(Ptr.Get(), nullptr);
        EXPECT_EQ(Ptr.GetRefCount(), 0u);
        EXPECT_TRUE(Ptr == nullptr);
    }

    TEST(SharedPtr, MakeSharedConstructsAndDestroysExactlyOnce)
    {
        FCounted::Alive = 0;
        {
            const TSharedPtr<FCounted> Ptr = MakeShared<FCounted>(42);
            EXPECT_EQ(FCounted::Alive, 1);
            EXPECT_EQ(Ptr->Payload, 42);
            EXPECT_EQ((*Ptr).Payload, 42);
            EXPECT_EQ(Ptr.GetRefCount(), 1u);
            EXPECT_TRUE(Ptr.IsUnique());
        }

        EXPECT_EQ(FCounted::Alive, 0);
    }

    TEST(SharedPtr, CopiesShareOneValue)
    {
        FCounted::Alive = 0;
        {
            const TSharedPtr<FCounted> First = MakeShared<FCounted>(1);
            TSharedPtr<FCounted> Second = First;
            const TSharedPtr<FCounted> Third(Second);

            EXPECT_EQ(FCounted::Alive, 1);
            EXPECT_EQ(First.GetRefCount(), 3u);
            EXPECT_EQ(First.Get(), Third.Get());
            EXPECT_TRUE(First == Third);

            Second.Reset();
            EXPECT_EQ(First.GetRefCount(), 2u);
            EXPECT_EQ(FCounted::Alive, 1);
        }

        EXPECT_EQ(FCounted::Alive, 0);
    }

    TEST(SharedPtr, MovingLeavesTheSourceEmpty)
    {
        TSharedPtr<FCounted> First = MakeShared<FCounted>(5);
        const TSharedPtr<FCounted> Second = std::move(First);

        EXPECT_FALSE(First.IsValid());
        EXPECT_TRUE(Second.IsValid());
        EXPECT_EQ(Second.GetRefCount(), 1u);
    }

    TEST(SharedPtr, SelfAssignmentKeepsTheValue)
    {
        FCounted::Alive = 0;
        TSharedPtr<FCounted> Ptr = MakeShared<FCounted>(9);
        const TSharedPtr<FCounted>& Alias = Ptr;

        Ptr = Alias;

        EXPECT_EQ(FCounted::Alive, 1);
        EXPECT_EQ(Ptr->Payload, 9);
        EXPECT_EQ(Ptr.GetRefCount(), 1u);
    }

    TEST(SharedPtr, ConvertsToABase)
    {
        const TSharedPtr<FDerived> Derived = MakeShared<FDerived>();
        const TSharedPtr<FBase> Base = Derived;

        EXPECT_EQ(Base->Which(), 2);
        EXPECT_EQ(Base.GetRefCount(), 2u);

        const TSharedPtr<FDerived> Back = Lumina::StaticCastSharedPtr<FDerived>(Base);
        EXPECT_EQ(Back.Get(), Derived.Get());
        EXPECT_EQ(Base.GetRefCount(), 3u);
    }

    TEST(SharedPtr, AdoptsARawPointerAndRunsItsDeleter)
    {
        FCounted::Alive = 0;
        FTrackingDeleter::Calls = 0;
        {
            const TSharedPtr<FCounted> Ptr(Lumina::Memory::New<FCounted>(3), FTrackingDeleter{});
            EXPECT_EQ(FCounted::Alive, 1);
            EXPECT_EQ(Ptr->Payload, 3);
        }

        EXPECT_EQ(FCounted::Alive, 0);
        EXPECT_EQ(FTrackingDeleter::Calls, 1);
    }

    TEST(SharedPtr, AdoptingNullStaysEmpty)
    {
        const TSharedPtr<FCounted> Ptr(static_cast<FCounted*>(nullptr));
        EXPECT_FALSE(Ptr.IsValid());
    }

    TEST(SharedPtr, IsTwoPointersWide)
    {
        EXPECT_EQ(sizeof(TSharedPtr<FCounted>), 2 * sizeof(void*));
        EXPECT_EQ(sizeof(TWeakPtr<FCounted>), 2 * sizeof(void*));
        EXPECT_EQ(sizeof(TUniquePtr<FCounted>), sizeof(void*));
    }

    TEST(SharedPtr, SurvivesVectorGrowth)
    {
        FCounted::Alive = 0;
        {
            TVector<TSharedPtr<FCounted>> Pointers;
            for (int32 Index = 0; Index < 200; ++Index)
            {
                Pointers.push_back(MakeShared<FCounted>(Index));
            }

            EXPECT_EQ(FCounted::Alive, 200);
            for (int32 Index = 0; Index < 200; ++Index)
            {
                ASSERT_EQ(Pointers[Index]->Payload, Index);
            }
        }

        EXPECT_EQ(FCounted::Alive, 0);
    }

    TEST(WeakPtr, PinsWhileTheValueLivesAndFailsAfter)
    {
        TWeakPtr<FCounted> Weak;
        {
            const TSharedPtr<FCounted> Strong = MakeShared<FCounted>(11);
            Weak = Strong;

            EXPECT_FALSE(Weak.IsExpired());
            const TSharedPtr<FCounted> Pinned = Weak.Pin();
            EXPECT_TRUE(Pinned.IsValid());
            EXPECT_EQ(Pinned->Payload, 11);
            EXPECT_EQ(Strong.GetRefCount(), 2u);
        }

        EXPECT_TRUE(Weak.IsExpired());
        EXPECT_FALSE(Weak.Pin().IsValid());
    }

    TEST(WeakPtr, DestroysTheValueBeforeTheBlock)
    {
        FCounted::Alive = 0;
        TWeakPtr<FCounted> Weak;
        {
            const TSharedPtr<FCounted> Strong = MakeShared<FCounted>(1);
            Weak = Strong;
            EXPECT_EQ(FCounted::Alive, 1);
        }

        // The block is still alive for the weak pointer, but the value is gone.
        EXPECT_EQ(FCounted::Alive, 0);
        EXPECT_TRUE(Weak.IsExpired());
        Weak.Reset();
    }

    TEST(SharedFromThis, HandsOutAPointerToItself)
    {
        const TSharedPtr<FSelfAware> Owner = MakeShared<FSelfAware>();
        const TSharedPtr<FSelfAware> Again = Owner->AsShared();

        EXPECT_EQ(Again.Get(), Owner.Get());
        EXPECT_EQ(Owner->Payload, 7);
        EXPECT_EQ(Owner.GetRefCount(), 2u);
    }

    TEST(UniquePtr, OwnsAndReleases)
    {
        FCounted::Alive = 0;
        {
            TUniquePtr<FCounted> Ptr = MakeUnique<FCounted>(4);
            EXPECT_EQ(FCounted::Alive, 1);
            EXPECT_EQ(Ptr->Payload, 4);

            FCounted* Raw = Ptr.Release();
            EXPECT_FALSE(Ptr.IsValid());
            EXPECT_EQ(FCounted::Alive, 1);

            Ptr.Reset(Raw);
            EXPECT_TRUE(Ptr.IsValid());
        }

        EXPECT_EQ(FCounted::Alive, 0);
    }

    TEST(UniquePtr, MovesAndConvertsToABase)
    {
        TUniquePtr<FDerived> Derived = MakeUnique<FDerived>();
        TUniquePtr<FBase> Base = std::move(Derived);

        EXPECT_FALSE(Derived.IsValid());
        EXPECT_EQ(Base->Which(), 2);
    }

    TEST(UniquePtr, IsNotCopyable)
    {
        EXPECT_FALSE(std::is_copy_constructible_v<TUniquePtr<FCounted>>);
        EXPECT_TRUE(std::is_move_constructible_v<TUniquePtr<FCounted>>);
    }

    TEST(UniquePtr, RunsACustomDeleter)
    {
        FTrackingDeleter::Calls = 0;
        {
            TUniquePtr<FCounted, FTrackingDeleter> Ptr(Lumina::Memory::New<FCounted>(1));
            EXPECT_TRUE(Ptr.IsValid());
        }

        EXPECT_EQ(FTrackingDeleter::Calls, 1);
    }

    TEST(UniquePtr, SurvivesVectorGrowth)
    {
        FCounted::Alive = 0;
        {
            TVector<TUniquePtr<FCounted>> Pointers;
            for (int32 Index = 0; Index < 200; ++Index)
            {
                Pointers.push_back(MakeUnique<FCounted>(Index));
            }

            EXPECT_EQ(FCounted::Alive, 200);
            EXPECT_EQ(Pointers[199]->Payload, 199);
        }

        EXPECT_EQ(FCounted::Alive, 0);
    }

    TEST(SharedPtr, RefCountsSurviveConcurrentCopying)
    {
        FCounted::Alive = 0;
        const TSharedPtr<FCounted> Root = MakeShared<FCounted>(1);

        constexpr uint32 kThreads = 8;
        constexpr uint32 kIterations = 20000;

        std::vector<FThread> Threads;
        Threads.reserve(kThreads);
        for (uint32 Index = 0; Index < kThreads; ++Index)
        {
            Threads.emplace_back([&Root]
            {
                for (uint32 Step = 0; Step < kIterations; ++Step)
                {
                    TSharedPtr<FCounted> Copy = Root;
                    TSharedPtr<FCounted> Second = Copy;
                    Copy.Reset();
                }
            });
        }

        for (FThread& Thread : Threads)
        {
            Thread.Join();
        }

        EXPECT_EQ(Root.GetRefCount(), 1u);
        EXPECT_EQ(FCounted::Alive, 1);
    }

    TEST(SharedPtr, WeakPinsRaceSafelyAgainstTheLastRelease)
    {
        constexpr uint32 kRounds = 2000;
        std::atomic<uint32> Pinned{ 0 };

        for (uint32 Round = 0; Round < kRounds; ++Round)
        {
            TSharedPtr<FCounted> Strong = MakeShared<FCounted>(static_cast<int32>(Round));
            TWeakPtr<FCounted> Weak = Strong;

            FThread Reader([&Weak, &Pinned]
            {
                if (const TSharedPtr<FCounted> Held = Weak.Pin())
                {
                    Pinned.fetch_add(1, std::memory_order_relaxed);
                }
            });

            Strong.Reset();
            Reader.Join();
        }

        EXPECT_EQ(FCounted::Alive, 0);
        SUCCEED();
    }

    TEST(SharedPtr, AtomicStoreAndLoadPublishAcrossThreads)
    {
        TSharedPtr<FCounted> Slot = MakeShared<FCounted>(0);
        std::atomic<bool> bStop{ false };
        std::atomic<uint32> Reads{ 0 };

        FThread Reader([&]
        {
            while (!bStop.load(std::memory_order_acquire))
            {
                const TSharedPtr<FCounted> Seen = Lumina::AtomicLoad(&Slot);
                if (Seen)
                {
                    EXPECT_GE(Seen->Payload, 0);
                    Reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

        for (int32 Index = 1; Index <= 2000; ++Index)
        {
            Lumina::AtomicStore(&Slot, MakeShared<FCounted>(Index));
        }

        bStop.store(true, std::memory_order_release);
        Reader.Join();

        EXPECT_EQ(Slot->Payload, 2000);
        EXPECT_GT(Reads.load(), 0u);
    }
}
