#include <gtest/gtest.h>

#include <thread>

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectAllocator.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectCore.h"
#include "GUID/GUID.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include "Platform/Process/PlatformProcess.h"
#include "Platform/Time/PlatformTime.h"
#include "Scripting/ScriptableTest.h"

using namespace Lumina;

// disabled by default, run with --gtest_also_run_disabled_tests --gtest_filter=*CObjectAllocationBenchmark*
namespace CObjectAllocationBench
{
    constexpr int32 kObjects = 200'000;
    constexpr int32 kRounds  = 5;
    constexpr int32 kThreads = 8;

    double NsPer(double Seconds, int32 Count)
    {
        return Seconds * 1e9 / (double)Count;
    }

    template<typename TFunc>
    double BestOf(TFunc&& Func)
    {
        double Best = 1e30;
        for (int32 Round = 0; Round < kRounds; ++Round)
        {
            Best = Math::Min(Best, Func());
        }
        return Best;
    }

    CObject* NewBenchObject()
    {
        return NewObject(CObjectRefTest::StaticClass(), nullptr, NAME_None, FGuid::New(), OF_Transient);
    }

    void CreateAll(TVector<CObject*>& Objects)
    {
        for (CObject*& Object : Objects)
        {
            Object = NewBenchObject();
        }
    }

    void DestroyAll(TVector<CObject*>& Objects)
    {
        for (CObject* Object : Objects)
        {
            Object->ConditionalBeginDestroy();
        }
    }

    // Create and destroy timed separately, the pair repeated so the best round is a warm one.
    void TimeLifetime(const char* Label, TVector<CObject*>& Objects)
    {
        double Create = 1e30;
        double Destroy = 1e30;
        for (int32 Round = 0; Round < kRounds; ++Round)
        {
            double Start = PlatformTime::Seconds();
            CreateAll(Objects);
            Create = Math::Min(Create, PlatformTime::Seconds() - Start);

            Start = PlatformTime::Seconds();
            DestroyAll(Objects);
            Destroy = Math::Min(Destroy, PlatformTime::Seconds() - Start);
        }
        printf("[Bench] %-28s create %6.1f ns  destroy %6.1f ns\n", Label, NsPer(Create, kObjects), NsPer(Destroy, kObjects));
    }
}

using namespace CObjectAllocationBench;

TEST(CObjectAllocationBenchmark, DISABLED_Breakdown)
{
    const CClass* Class = CObjectRefTest::StaticClass();
    const uint32  Size  = Class->GetSize();
    printf("[Bench] class %s, %u bytes, %d objects, best of %d\n", *Class->GetName(), Size, kObjects, kRounds);

    TVector<CObject*> Objects(kObjects);
    TVector<void*>    Raw(kObjects);

    {
        const uint64 TrackedBefore = Memory::GetTrackedLiveBytes();
        const uint64 CountBefore   = Memory::GetTrackedLiveCount();
        const size_t CommitBefore  = Platform::GetProcessMemoryUsageBytes();
        CreateAll(Objects);
        const size_t CommitAfter   = Platform::GetProcessMemoryUsageBytes();
        printf("[Bench] first round per object: %.1f tracked heap bytes in %.2f allocations, %.1f process bytes\n",
            (double)(Memory::GetTrackedLiveBytes() - TrackedBefore) / kObjects,
            (double)(Memory::GetTrackedLiveCount() - CountBefore) / kObjects,
            (double)(int64)(CommitAfter - CommitBefore) / kObjects);
        DestroyAll(Objects);
    }

    TimeLifetime("NewObject, tracking on", Objects);
    Memory::SetTrackingEnabled(false);
    TimeLifetime("NewObject, tracking off", Objects);
    Memory::SetTrackingEnabled(true);

    auto TimePairs = [&](auto&& Alloc, auto&& Free)
    {
        return BestOf([&]
        {
            const double Start = PlatformTime::Seconds();
            for (void*& Ptr : Raw)
            {
                Ptr = Alloc();
            }
            for (void*& Ptr : Raw)
            {
                Free(Ptr);
            }
            return PlatformTime::Seconds() - Start;
        });
    };

    const double Tracked = TimePairs([&] { return Memory::Malloc(Size, 16); }, [](void*& Ptr) { Memory::Free(Ptr); });
    Memory::SetTrackingEnabled(false);
    const double Untracked = TimePairs([&] { return Memory::Malloc(Size, 16); }, [](void*& Ptr) { Memory::Free(Ptr); });
    Memory::SetTrackingEnabled(true);
    printf("[Bench]   Malloc+Free tracked         %6.1f ns\n", NsPer(Tracked, kObjects));
    printf("[Bench]   Malloc+Free untracked       %6.1f ns\n", NsPer(Untracked, kObjects));

    // Any non-null pointer, since the array only stores it.
    CObjectBase* Fake = reinterpret_cast<CObjectBase*>(uintptr_t(0x1000));
    TVector<int32> Indices(kObjects);
    const double SlotTime = BestOf([&]
    {
        const double Start = PlatformTime::Seconds();
        for (int32 i = 0; i < kObjects; ++i)
        {
            Indices[i] = Object::Internal::AllocateObject(Fake).Index;
        }
        for (int32 i = 0; i < kObjects; ++i)
        {
            Object::Internal::DeallocateObject(Indices[i]);
        }
        return PlatformTime::Seconds() - Start;
    });
    printf("[Bench]   object array slot pair      %6.1f ns\n", NsPer(SlotTime, kObjects));

    const double NameTime = BestOf([&]
    {
        const double Start = PlatformTime::Seconds();
        for (int32 i = 0; i < kObjects; ++i)
        {
            const FName Name(*Class->GetName(), i + 1);
            (void)Name;
        }
        return PlatformTime::Seconds() - Start;
    });
    printf("[Bench]   numbered FName              %6.1f ns\n", NsPer(NameTime, kObjects));

    const double GuidTime = BestOf([&]
    {
        const double Start = PlatformTime::Seconds();
        for (int32 i = 0; i < kObjects; ++i)
        {
            const FGuid Guid = FGuid::New();
            (void)Guid;
        }
        return PlatformTime::Seconds() - Start;
    });
    printf("[Bench]   FGuid::New                  %6.1f ns\n", NsPer(GuidTime, kObjects));

    CreateAll(Objects);
    const double HashTime = BestOf([&]
    {
        const double Start = PlatformTime::Seconds();
        for (CObject* Object : Objects)
        {
            Object->HandleNameChange(Object->GetName());
        }
        return PlatformTime::Seconds() - Start;
    });
    printf("[Bench]   hash remove+add pair        %6.1f ns\n", NsPer(HashTime, kObjects));

    const double CopyTime = BestOf([&]
    {
        const CObject* Default = Class->GetDefaultObject();
        const double Start = PlatformTime::Seconds();
        for (CObject* Object : Objects)
        {
            Class->CopyProperties(Object, Default);
        }
        return PlatformTime::Seconds() - Start;
    });
    printf("[Bench]   CopyProperties from CDO     %6.1f ns\n", NsPer(CopyTime, kObjects));
    DestroyAll(Objects);
}

TEST(CObjectAllocationBenchmark, DISABLED_Threaded)
{
    constexpr int32 PerThread = kObjects / kThreads;
    TVector<TVector<CObject*>> PerThreadObjects(kThreads);

    auto Run = [&](bool bCreate)
    {
        TVector<std::thread> Threads;
        const double Start = PlatformTime::Seconds();
        for (int32 t = 0; t < kThreads; ++t)
        {
            Threads.Emplace([&, t, bCreate]
            {
                Memory::InitializeThreadHeap();
                TVector<CObject*>& Mine = PerThreadObjects[t];
                Mine.SetNum(PerThread);
                if (bCreate)
                {
                    CreateAll(Mine);
                }
                else
                {
                    DestroyAll(Mine);
                }
                Memory::ShutdownThreadHeap();
            });
        }
        for (std::thread& Thread : Threads)
        {
            Thread.join();
        }
        return PlatformTime::Seconds() - Start;
    };

    double Create = 1e30;
    double Destroy = 1e30;
    for (int32 Round = 0; Round < kRounds; ++Round)
    {
        Create  = Math::Min(Create, Run(true));
        Destroy = Math::Min(Destroy, Run(false));
    }

    printf("[Bench] %d threads, %d objects each, wall time per object\n", kThreads, PerThread);
    printf("[Bench] create %6.1f ns  destroy %6.1f ns\n", NsPer(Create, kObjects), NsPer(Destroy, kObjects));

    // Any non-null pointer, since the array only stores it.
    CObjectBase* Fake = reinterpret_cast<CObjectBase*>(uintptr_t(0x1000));
    const double Slots = BestOf([&]
    {
        TVector<std::thread> Threads;
        const double Start = PlatformTime::Seconds();
        for (int32 t = 0; t < kThreads; ++t)
        {
            Threads.Emplace([&]
            {
                Memory::InitializeThreadHeap();
                TVector<int32> Indices(PerThread);
                for (int32& Index : Indices)
                {
                    Index = Object::Internal::AllocateObject(Fake).Index;
                }
                for (int32 Index : Indices)
                {
                    Object::Internal::DeallocateObject(Index);
                }
                Memory::ShutdownThreadHeap();
            });
        }
        for (std::thread& Thread : Threads)
        {
            Thread.join();
        }
        return PlatformTime::Seconds() - Start;
    });
    printf("[Bench] object array slot pair %6.1f ns\n", NsPer(Slots, kObjects));
}
