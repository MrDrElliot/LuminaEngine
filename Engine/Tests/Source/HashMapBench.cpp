#include <gtest/gtest.h>

#include <cstdio>
#include <algorithm>
#include <vector>

#include <unordered_map>
#include <unordered_set>

#include "Platform/Time/PlatformTime.h"
#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Containers/String.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaHashMapBench
{
    using Lumina::uint32;
    using Lumina::uint64;
    using Lumina::FName;
    using Lumina::FString;
    using Lumina::FStringView;

    template <typename K, typename V>
    using TLuminaMap = Lumina::Containers::THashMap<K, V>;

    template <typename T>
    using TLuminaSet = Lumina::Containers::THashSet<T>;

    // Engine key types no longer specialize std::hash, so the baseline borrows GetTypeHash for those.
    struct FBaselineHash
    {
        template <typename T>
        size_t operator()(const T& Key) const noexcept
        {
            if constexpr (requires { std::hash<T>{}(Key); })
            {
                return std::hash<T>{}(Key);
            }
            else
            {
                return static_cast<size_t>(GetTypeHash(Key));
            }
        }
    };

    template <typename K, typename V>
    using TBaselineMap = std::unordered_map<K, V, FBaselineHash>;

    volatile uint64 GBenchSink = 0;

    constexpr int kElementCount = 200'000;
    constexpr int kLookupCount  = 1'000'000;
    constexpr int kRepeats      = 5;

    /** Deterministic so every run compares the same key set, and independent of the engine RNG. */
    struct FBenchRandom
    {
        uint64 State = 0x853c49e6748fea9bull;

        uint64 Next()
        {
            State += 0x9e3779b97f4a7c15ull;
            uint64 Value = State;
            Value ^= Value >> 30;
            Value *= 0xbf58476d1ce4e5b9ull;
            Value ^= Value >> 27;
            Value *= 0x94d049bb133111ebull;
            return Value ^ (Value >> 31);
        }
    };

    template <typename TBody>
    double BestMillisOf(int Repeats, TBody&& Body)
    {
        double Best = 1e30;
        for (int Attempt = 0; Attempt < Repeats; ++Attempt)
        {
            const Lumina::uint64 Start = Lumina::PlatformTime::Cycles();
            Body();
            const Lumina::uint64 Stop = Lumina::PlatformTime::Cycles();

            const double Millis = Lumina::PlatformTime::ToMilliseconds(Stop - Start);
            Best = Millis < Best ? Millis : Best;
        }
        return Best;
    }

    void ReportHeader(const char* Title)
    {
        std::printf("\n%s\n", Title);
        std::printf("  %-26s %10s %10s\n", "implementation", "best (ms)", "vs std");
        std::printf("  %-26s %10s %10s\n", "--------------------------", "---------", "--------");
    }

    void ReportRow(const char* Name, double Millis, double Baseline)
    {
        if (Baseline > 0.0)
        {
            std::printf("  %-26s %10.3f %9.2fx\n", Name, Millis, Baseline / Millis);
        }
        else
        {
            std::printf("  %-26s %10.3f %10s\n", Name, Millis, "-");
        }
    }

    // The standard map never reports its footprint, so this rebuilds it from buckets and nodes.
    template <typename TMap>
    size_t BaselineBytes(const TMap& Map)
    {
        using FValue = typename TMap::value_type;
        constexpr size_t NodeSize = sizeof(FValue) + sizeof(void*);
        return Map.bucket_count() * sizeof(void*) + Map.size() * NodeSize;
    }

    const std::vector<uint32>& UInt32Keys()
    {
        static const std::vector<uint32> Keys = []
        {
            std::vector<uint32> Built;
            Built.reserve(kElementCount);
            FBenchRandom Random;
            for (int Index = 0; Index < kElementCount; ++Index)
            {
                Built.push_back(static_cast<uint32>(Random.Next()));
            }
            return Built;
        }();
        return Keys;
    }

    const std::vector<uint32>& MissingUInt32Keys()
    {
        static const std::vector<uint32> Keys = []
        {
            std::vector<uint32> Built;
            Built.reserve(kElementCount);
            FBenchRandom Random;
            Random.State = 0xfeedfacecafebeefull;
            for (int Index = 0; Index < kElementCount; ++Index)
            {
                Built.push_back(static_cast<uint32>(Random.Next()));
            }
            return Built;
        }();
        return Keys;
    }

    const std::vector<FName>& NameKeys()
    {
        static const std::vector<FName> Keys = []
        {
            std::vector<FName> Built;
            Built.reserve(kElementCount);
            for (int Index = 0; Index < kElementCount; ++Index)
            {
                Built.push_back(FName(Lumina::Format("Entity_{}_Component", Index).c_str()));
            }
            return Built;
        }();
        return Keys;
    }

    const std::vector<FString>& StringKeys()
    {
        static const std::vector<FString> Keys = []
        {
            std::vector<FString> Built;
            Built.reserve(kElementCount);
            for (int Index = 0; Index < kElementCount; ++Index)
            {
                Built.push_back(Lumina::Format("/Game/Content/Meshes/Prop_{}.lasset", Index));
            }
            return Built;
        }();
        return Keys;
    }

    const std::vector<void*>& PointerKeys()
    {
        static const std::vector<void*> Keys = []
        {
            static std::vector<uint64> Backing(kElementCount);
            std::vector<void*> Built;
            Built.reserve(kElementCount);
            for (int Index = 0; Index < kElementCount; ++Index)
            {
                Built.push_back(&Backing[static_cast<size_t>(Index)]);
            }
            return Built;
        }();
        return Keys;
    }

    TEST(HashMapBench, Footprint)
    {
        const std::vector<uint32>& Keys = UInt32Keys();

        TLuminaMap<uint32, uint32> Lumina;
        TBaselineMap<uint32, uint32> Baseline;
        for (uint32 Key : Keys)
        {
            Lumina[Key] = Key;
            Baseline[Key] = Key;
        }

        const double LuminaBytes = static_cast<double>(Lumina.GetAllocatedBytes());
        const double BaselineEstimate = static_cast<double>(BaselineBytes(Baseline));

        std::printf("\nfootprint for %d live uint32 to uint32 entries\n", kElementCount);
        std::printf("  %-26s %12s %14s %12s\n", "implementation", "total KB", "bytes/element", "allocations");
        std::printf("  %-26s %12.1f %14.2f %12s\n", "Lumina THashMap",
            LuminaBytes / 1024.0, LuminaBytes / Lumina.size(), "1");
        std::printf("  %-26s %12.1f %14.2f %12s\n", "THashMap (computed)",
            BaselineEstimate / 1024.0, BaselineEstimate / Baseline.size(), "1 + one per element");

        std::printf("\n  %-26s %10zu\n", "sizeof Lumina THashMap", sizeof(TLuminaMap<uint32, uint32>));
        std::printf("  %-26s %10zu\n", "sizeof THashMap", sizeof(TBaselineMap<uint32, uint32>));
        SUCCEED();
    }

    TEST(HashMapBench, InsertUInt32NoReserve)
    {
        const std::vector<uint32>& Keys = UInt32Keys();

        const double Baseline = BestMillisOf(kRepeats, [&Keys]
        {
            TBaselineMap<uint32, uint32> Map;
            for (uint32 Key : Keys)
            {
                Map[Key] = Key;
            }
            GBenchSink += Map.size();
        });

        const double Lumina = BestMillisOf(kRepeats, [&Keys]
        {
            TLuminaMap<uint32, uint32> Map;
            for (uint32 Key : Keys)
            {
                Map[Key] = Key;
            }
            GBenchSink += Map.size();
        });

        ReportHeader("insert 200,000 random uint32, no reserve");
        ReportRow("THashMap", Baseline, 0.0);
        ReportRow("Lumina THashMap", Lumina, Baseline);
        SUCCEED();
    }

    TEST(HashMapBench, InsertUInt32Reserved)
    {
        const std::vector<uint32>& Keys = UInt32Keys();

        const double Baseline = BestMillisOf(kRepeats, [&Keys]
        {
            TBaselineMap<uint32, uint32> Map;
            Map.reserve(Keys.size());
            for (uint32 Key : Keys)
            {
                Map[Key] = Key;
            }
            GBenchSink += Map.size();
        });

        const double Lumina = BestMillisOf(kRepeats, [&Keys]
        {
            TLuminaMap<uint32, uint32> Map;
            Map.reserve(Keys.size());
            for (uint32 Key : Keys)
            {
                Map[Key] = Key;
            }
            GBenchSink += Map.size();
        });

        ReportHeader("insert 200,000 random uint32, reserved");
        ReportRow("THashMap", Baseline, 0.0);
        ReportRow("Lumina THashMap", Lumina, Baseline);
        SUCCEED();
    }

    TEST(HashMapBench, FindHitUInt32)
    {
        const std::vector<uint32>& Keys = UInt32Keys();

        TBaselineMap<uint32, uint32> Baseline;
        TLuminaMap<uint32, uint32> Lumina;
        for (uint32 Key : Keys)
        {
            Baseline[Key] = Key;
            Lumina[Key] = Key;
        }

        const double BaselineMillis = BestMillisOf(kRepeats, [&Keys, &Baseline]
        {
            uint64 Sum = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (uint32 Key : Keys)
                {
                    Sum += Baseline.find(Key)->second;
                }
            }
            GBenchSink += Sum;
        });

        const double LuminaMillis = BestMillisOf(kRepeats, [&Keys, &Lumina]
        {
            uint64 Sum = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (uint32 Key : Keys)
                {
                    Sum += Lumina.find(Key)->second;
                }
            }
            GBenchSink += Sum;
        });

        ReportHeader("1,000,000 successful uint32 lookups over 200,000 entries");
        ReportRow("THashMap", BaselineMillis, 0.0);
        ReportRow("Lumina THashMap", LuminaMillis, BaselineMillis);
        SUCCEED();
    }

    // A miss is where the group probe wins hardest, ending the search with no compare.
    TEST(HashMapBench, FindMissUInt32)
    {
        const std::vector<uint32>& Keys = UInt32Keys();
        const std::vector<uint32>& Missing = MissingUInt32Keys();

        TBaselineMap<uint32, uint32> Baseline;
        TLuminaMap<uint32, uint32> Lumina;
        for (uint32 Key : Keys)
        {
            Baseline[Key] = Key;
            Lumina[Key] = Key;
        }

        const double BaselineMillis = BestMillisOf(kRepeats, [&Missing, &Baseline]
        {
            uint64 Hits = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (uint32 Key : Missing)
                {
                    Hits += Baseline.find(Key) != Baseline.end() ? 1 : 0;
                }
            }
            GBenchSink += Hits;
        });

        const double LuminaMillis = BestMillisOf(kRepeats, [&Missing, &Lumina]
        {
            uint64 Hits = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (uint32 Key : Missing)
                {
                    Hits += Lumina.find(Key) != Lumina.end() ? 1 : 0;
                }
            }
            GBenchSink += Hits;
        });

        ReportHeader("1,000,000 failed uint32 lookups over 200,000 entries");
        ReportRow("THashMap", BaselineMillis, 0.0);
        ReportRow("Lumina THashMap", LuminaMillis, BaselineMillis);
        SUCCEED();
    }

    TEST(HashMapBench, InsertFName)
    {
        const std::vector<FName>& Keys = NameKeys();

        const double Baseline = BestMillisOf(kRepeats, [&Keys]
        {
            TBaselineMap<FName, uint32> Map;
            uint32 Value = 0;
            for (const FName& Key : Keys)
            {
                Map[Key] = Value++;
            }
            GBenchSink += Map.size();
        });

        const double Lumina = BestMillisOf(kRepeats, [&Keys]
        {
            TLuminaMap<FName, uint32> Map;
            uint32 Value = 0;
            for (const FName& Key : Keys)
            {
                Map[Key] = Value++;
            }
            GBenchSink += Map.size();
        });

        ReportHeader("insert 200,000 FName keys, no reserve");
        ReportRow("THashMap", Baseline, 0.0);
        ReportRow("Lumina THashMap", Lumina, Baseline);
        SUCCEED();
    }

    TEST(HashMapBench, FindHitFName)
    {
        const std::vector<FName>& Keys = NameKeys();

        TBaselineMap<FName, uint32> Baseline;
        TLuminaMap<FName, uint32> Lumina;
        uint32 Value = 0;
        for (const FName& Key : Keys)
        {
            Baseline[Key] = Value;
            Lumina[Key] = Value;
            ++Value;
        }

        const double BaselineMillis = BestMillisOf(kRepeats, [&Keys, &Baseline]
        {
            uint64 Sum = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (const FName& Key : Keys)
                {
                    Sum += Baseline.find(Key)->second;
                }
            }
            GBenchSink += Sum;
        });

        const double LuminaMillis = BestMillisOf(kRepeats, [&Keys, &Lumina]
        {
            uint64 Sum = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (const FName& Key : Keys)
                {
                    Sum += Lumina.find(Key)->second;
                }
            }
            GBenchSink += Sum;
        });

        ReportHeader("1,000,000 successful FName lookups over 200,000 entries");
        ReportRow("THashMap", BaselineMillis, 0.0);
        ReportRow("Lumina THashMap", LuminaMillis, BaselineMillis);
        SUCCEED();
    }

    TEST(HashMapBench, FindHitPointer)
    {
        const std::vector<void*>& Keys = PointerKeys();

        TBaselineMap<void*, uint32> Baseline;
        TLuminaMap<void*, uint32> Lumina;
        uint32 Value = 0;
        for (void* Key : Keys)
        {
            Baseline[Key] = Value;
            Lumina[Key] = Value;
            ++Value;
        }

        const double BaselineMillis = BestMillisOf(kRepeats, [&Keys, &Baseline]
        {
            uint64 Sum = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (void* Key : Keys)
                {
                    Sum += Baseline.find(Key)->second;
                }
            }
            GBenchSink += Sum;
        });

        const double LuminaMillis = BestMillisOf(kRepeats, [&Keys, &Lumina]
        {
            uint64 Sum = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (void* Key : Keys)
                {
                    Sum += Lumina.find(Key)->second;
                }
            }
            GBenchSink += Sum;
        });

        ReportHeader("1,000,000 successful pointer lookups over 200,000 entries");
        ReportRow("THashMap", BaselineMillis, 0.0);
        ReportRow("Lumina THashMap", LuminaMillis, BaselineMillis);
        SUCCEED();
    }

    // Same keys probed out of order, which separates real speed from a sequential best case.
    TEST(HashMapBench, FindHitPointerShuffled)
    {
        const std::vector<void*>& Keys = PointerKeys();

        std::vector<void*> Probe = Keys;
        FBenchRandom Random;
        for (size_t Index = Probe.size(); Index > 1; --Index)
        {
            const size_t Swap = static_cast<size_t>(Random.Next() % Index);
            std::swap(Probe[Index - 1], Probe[Swap]);
        }

        TBaselineMap<void*, uint32> Baseline;
        TLuminaMap<void*, uint32> Lumina;
        uint32 Value = 0;
        for (void* Key : Keys)
        {
            Baseline[Key] = Value;
            Lumina[Key] = Value;
            ++Value;
        }

        const double BaselineMillis = BestMillisOf(kRepeats, [&Probe, &Baseline]
        {
            uint64 Sum = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (void* Key : Probe)
                {
                    Sum += Baseline.find(Key)->second;
                }
            }
            GBenchSink += Sum;
        });

        const double LuminaMillis = BestMillisOf(kRepeats, [&Probe, &Lumina]
        {
            uint64 Sum = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (void* Key : Probe)
                {
                    Sum += Lumina.find(Key)->second;
                }
            }
            GBenchSink += Sum;
        });

        ReportHeader("1,000,000 pointer lookups over 200,000 entries, shuffled probe order");
        ReportRow("THashMap", BaselineMillis, 0.0);
        ReportRow("Lumina THashMap", LuminaMillis, BaselineMillis);
        SUCCEED();
    }

    TEST(HashMapBench, FindHitUInt32Shuffled)
    {
        const std::vector<uint32>& Keys = UInt32Keys();

        std::vector<uint32> Probe = Keys;
        FBenchRandom Random;
        for (size_t Index = Probe.size(); Index > 1; --Index)
        {
            const size_t Swap = static_cast<size_t>(Random.Next() % Index);
            std::swap(Probe[Index - 1], Probe[Swap]);
        }

        TBaselineMap<uint32, uint32> Baseline;
        TLuminaMap<uint32, uint32> Lumina;
        for (uint32 Key : Keys)
        {
            Baseline[Key] = Key;
            Lumina[Key] = Key;
        }

        const double BaselineMillis = BestMillisOf(kRepeats, [&Probe, &Baseline]
        {
            uint64 Sum = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (uint32 Key : Probe)
                {
                    Sum += Baseline.find(Key)->second;
                }
            }
            GBenchSink += Sum;
        });

        const double LuminaMillis = BestMillisOf(kRepeats, [&Probe, &Lumina]
        {
            uint64 Sum = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (uint32 Key : Probe)
                {
                    Sum += Lumina.find(Key)->second;
                }
            }
            GBenchSink += Sum;
        });

        ReportHeader("1,000,000 uint32 lookups over 200,000 entries, shuffled probe order");
        ReportRow("THashMap", BaselineMillis, 0.0);
        ReportRow("Lumina THashMap", LuminaMillis, BaselineMillis);
        SUCCEED();
    }

    // Same key type and slot size as the random case; only the distribution changes.
    TEST(HashMapBench, FindHitUInt32Sequential)
    {
        std::vector<uint32> Keys;
        Keys.reserve(kElementCount);
        for (int Index = 0; Index < kElementCount; ++Index)
        {
            Keys.push_back(static_cast<uint32>(Index));
        }

        TBaselineMap<uint32, uint32> Baseline;
        TLuminaMap<uint32, uint32> Lumina;
        for (uint32 Key : Keys)
        {
            Baseline[Key] = Key;
            Lumina[Key] = Key;
        }

        const double BaselineMillis = BestMillisOf(kRepeats, [&Keys, &Baseline]
        {
            uint64 Sum = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (uint32 Key : Keys)
                {
                    Sum += Baseline.find(Key)->second;
                }
            }
            GBenchSink += Sum;
        });

        const double LuminaMillis = BestMillisOf(kRepeats, [&Keys, &Lumina]
        {
            uint64 Sum = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (uint32 Key : Keys)
                {
                    Sum += Lumina.find(Key)->second;
                }
            }
            GBenchSink += Sum;
        });

        ReportHeader("1,000,000 sequential uint32 lookups over 200,000 entries");
        ReportRow("THashMap", BaselineMillis, 0.0);
        ReportRow("Lumina THashMap", LuminaMillis, BaselineMillis);
        SUCCEED();
    }

    /** One multiply and a fold, against splitmix64's two multiplies and three shifts. */
    struct FCheapMixHash
    {
        using is_transparent = void;

        uint64 operator()(uint32 Key) const noexcept
        {
            const uint64 Product = static_cast<uint64>(Key) * 0x9e3779b97f4a7c15ull;
            return Product ^ (Product >> 32);
        }
    };

    TEST(HashMapBench, MixCostOnLookup)
    {
        std::vector<uint32> Random;
        std::vector<uint32> Sequential;
        Random.reserve(kElementCount);
        Sequential.reserve(kElementCount);
        FBenchRandom Source;
        for (int Index = 0; Index < kElementCount; ++Index)
        {
            Random.push_back(static_cast<uint32>(Source.Next()));
            Sequential.push_back(static_cast<uint32>(Index));
        }

        const auto Measure = [](const std::vector<uint32>& Keys, auto& Map)
        {
            for (uint32 Key : Keys)
            {
                Map[Key] = Key;
            }
            return BestMillisOf(kRepeats, [&Keys, &Map]
            {
                uint64 Sum = 0;
                for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
                {
                    for (uint32 Key : Keys)
                    {
                        Sum += Map.find(Key)->second;
                    }
                }
                GBenchSink += Sum;
            });
        };

        TLuminaMap<uint32, uint32> SplitmixRandom;
        Lumina::Containers::THashMap<uint32, uint32, FCheapMixHash> CheapRandom;
        const double SplitmixRandomMillis = Measure(Random, SplitmixRandom);
        const double CheapRandomMillis = Measure(Random, CheapRandom);

        TLuminaMap<uint32, uint32> SplitmixSequential;
        Lumina::Containers::THashMap<uint32, uint32, FCheapMixHash> CheapSequential;
        const double SplitmixSequentialMillis = Measure(Sequential, SplitmixSequential);
        const double CheapSequentialMillis = Measure(Sequential, CheapSequential);

        ReportHeader("1,000,000 random uint32 lookups, hash function swapped");
        ReportRow("splitmix64 finalizer", SplitmixRandomMillis, 0.0);
        ReportRow("single multiply fold", CheapRandomMillis, SplitmixRandomMillis);

        ReportHeader("1,000,000 sequential uint32 lookups, hash function swapped");
        ReportRow("splitmix64 finalizer", SplitmixSequentialMillis, 0.0);
        ReportRow("single multiply fold", CheapSequentialMillis, SplitmixSequentialMillis);
        SUCCEED();
    }

    TEST(HashMapBench, FindHitString)
    {
        const std::vector<FString>& Keys = StringKeys();

        TBaselineMap<FString, uint32> Baseline;
        TLuminaMap<FString, uint32> Lumina;
        uint32 Value = 0;
        for (const FString& Key : Keys)
        {
            Baseline[Key] = Value;
            Lumina[Key] = Value;
            ++Value;
        }

        const double BaselineMillis = BestMillisOf(kRepeats, [&Keys, &Baseline]
        {
            uint64 Sum = 0;
            for (const FString& Key : Keys)
            {
                Sum += Baseline.find(Key)->second;
            }
            GBenchSink += Sum;
        });

        const double LuminaMillis = BestMillisOf(kRepeats, [&Keys, &Lumina]
        {
            uint64 Sum = 0;
            for (const FString& Key : Keys)
            {
                Sum += Lumina.find(Key)->second;
            }
            GBenchSink += Sum;
        });

        ReportHeader("200,000 successful FString lookups over 200,000 entries");
        ReportRow("THashMap", BaselineMillis, 0.0);
        ReportRow("Lumina THashMap", LuminaMillis, BaselineMillis);
        SUCCEED();
    }

    // The standard map has no transparent lookup, so a view has to be widened into an owning key first.
    TEST(HashMapBench, StringLookupByView)
    {
        const std::vector<FString>& Keys = StringKeys();

        TBaselineMap<FString, uint32> Baseline;
        TLuminaMap<FString, uint32> Lumina;
        uint32 Value = 0;
        for (const FString& Key : Keys)
        {
            Baseline[Key] = Value;
            Lumina[Key] = Value;
            ++Value;
        }

        std::vector<FStringView> Views;
        Views.reserve(Keys.size());
        for (const FString& Key : Keys)
        {
            Views.push_back(FStringView(Key.data(), Key.size()));
        }

        const double BaselineMillis = BestMillisOf(kRepeats, [&Views, &Baseline]
        {
            uint64 Sum = 0;
            for (FStringView View : Views)
            {
                Sum += Baseline.find(FString(View.data(), View.size()))->second;
            }
            GBenchSink += Sum;
        });

        const double LuminaMillis = BestMillisOf(kRepeats, [&Views, &Lumina]
        {
            uint64 Sum = 0;
            for (FStringView View : Views)
            {
                Sum += Lumina.find(View)->second;
            }
            GBenchSink += Sum;
        });

        ReportHeader("200,000 view lookups into an FString-keyed map");
        ReportRow("std + temporary key", BaselineMillis, 0.0);
        ReportRow("Lumina transparent", LuminaMillis, BaselineMillis);
        SUCCEED();
    }

    TEST(HashMapBench, NodeVersusFlatLookup)
    {
        const std::vector<uint32>& Keys = UInt32Keys();

        TBaselineMap<uint32, uint32> Baseline;
        TLuminaMap<uint32, uint32> Flat;
        Lumina::Containers::TNodeHashMap<uint32, uint32> Node;
        for (uint32 Key : Keys)
        {
            Baseline[Key] = Key;
            Flat[Key] = Key;
            Node[Key] = Key;
        }

        const auto Probe = [&Keys](auto& Map)
        {
            return BestMillisOf(kRepeats, [&Keys, &Map]
            {
                uint64 Sum = 0;
                for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
                {
                    for (uint32 Key : Keys)
                    {
                        Sum += Map.find(Key)->second;
                    }
                }
                GBenchSink += Sum;
            });
        };

        const double BaselineMillis = Probe(Baseline);
        const double FlatMillis = Probe(Flat);
        const double NodeMillis = Probe(Node);

        ReportHeader("1,000,000 uint32 lookups, flat against pointer-stable node storage");
        ReportRow("THashMap", BaselineMillis, 0.0);
        ReportRow("Lumina flat", FlatMillis, BaselineMillis);
        ReportRow("Lumina node", NodeMillis, BaselineMillis);
        SUCCEED();
    }

    TEST(HashMapBench, NodeVersusFlatInsert)
    {
        const std::vector<uint32>& Keys = UInt32Keys();

        const double BaselineMillis = BestMillisOf(kRepeats, [&Keys]
        {
            TBaselineMap<uint32, uint32> Map;
            for (uint32 Key : Keys)
            {
                Map[Key] = Key;
            }
            GBenchSink += Map.size();
        });

        const double FlatMillis = BestMillisOf(kRepeats, [&Keys]
        {
            TLuminaMap<uint32, uint32> Map;
            for (uint32 Key : Keys)
            {
                Map[Key] = Key;
            }
            GBenchSink += Map.size();
        });

        const double NodeMillis = BestMillisOf(kRepeats, [&Keys]
        {
            Lumina::Containers::TNodeHashMap<uint32, uint32> Map;
            for (uint32 Key : Keys)
            {
                Map[Key] = Key;
            }
            GBenchSink += Map.size();
        });

        ReportHeader("insert 200,000 uint32, flat against pointer-stable node storage");
        ReportRow("THashMap", BaselineMillis, 0.0);
        ReportRow("Lumina flat", FlatMillis, BaselineMillis);
        ReportRow("Lumina node", NodeMillis, BaselineMillis);
        SUCCEED();
    }

    // Short-lived small maps are where an inline buffer should pay, with no allocator traffic.
    TEST(HashMapBench, InlineVersusFlatSmallTables)
    {
        constexpr int kTables = 200'000;
        constexpr int kEntries = 12;

        const double BaselineMillis = BestMillisOf(3, []
        {
            uint64 Sum = 0;
            for (int Table = 0; Table < kTables; ++Table)
            {
                TBaselineMap<uint32, uint32> Map;
                for (int Index = 0; Index < kEntries; ++Index)
                {
                    Map[static_cast<uint32>(Index)] = static_cast<uint32>(Index);
                }
                Sum += Map.find(static_cast<uint32>(Table % kEntries))->second;
            }
            GBenchSink += Sum;
        });

        const double FlatMillis = BestMillisOf(3, []
        {
            uint64 Sum = 0;
            for (int Table = 0; Table < kTables; ++Table)
            {
                TLuminaMap<uint32, uint32> Map;
                for (int Index = 0; Index < kEntries; ++Index)
                {
                    Map[static_cast<uint32>(Index)] = static_cast<uint32>(Index);
                }
                Sum += Map.find(static_cast<uint32>(Table % kEntries))->second;
            }
            GBenchSink += Sum;
        });

        const double InlineMillis = BestMillisOf(3, []
        {
            uint64 Sum = 0;
            for (int Table = 0; Table < kTables; ++Table)
            {
                Lumina::Containers::TInlineHashMap<uint32, uint32, 15> Map;
                for (int Index = 0; Index < kEntries; ++Index)
                {
                    Map[static_cast<uint32>(Index)] = static_cast<uint32>(Index);
                }
                Sum += Map.find(static_cast<uint32>(Table % kEntries))->second;
            }
            GBenchSink += Sum;
        });

        ReportHeader("build, probe and destroy 200,000 maps of 12 entries");
        ReportRow("THashMap", BaselineMillis, 0.0);
        ReportRow("Lumina flat", FlatMillis, BaselineMillis);
        ReportRow("Lumina inline 15", InlineMillis, BaselineMillis);
        SUCCEED();
    }

    TEST(HashMapBench, EraseChurn)
    {
        constexpr int kLiveCount = 100'000;
        constexpr int kRounds = 20;

        const double Baseline = BestMillisOf(3, []
        {
            TBaselineMap<uint32, uint32> Map;
            for (int Index = 0; Index < kLiveCount; ++Index)
            {
                Map[static_cast<uint32>(Index)] = static_cast<uint32>(Index);
            }
            for (int Round = 0; Round < kRounds; ++Round)
            {
                for (int Index = 0; Index < kLiveCount; ++Index)
                {
                    Map.erase(static_cast<uint32>(Round * kLiveCount + Index));
                    Map[static_cast<uint32>((Round + 1) * kLiveCount + Index)] = 0;
                }
            }
            GBenchSink += Map.size();
        });

        const double Lumina = BestMillisOf(3, []
        {
            TLuminaMap<uint32, uint32> Map;
            for (int Index = 0; Index < kLiveCount; ++Index)
            {
                Map[static_cast<uint32>(Index)] = static_cast<uint32>(Index);
            }
            for (int Round = 0; Round < kRounds; ++Round)
            {
                for (int Index = 0; Index < kLiveCount; ++Index)
                {
                    Map.erase(static_cast<uint32>(Round * kLiveCount + Index));
                    Map[static_cast<uint32>((Round + 1) * kLiveCount + Index)] = 0;
                }
            }
            GBenchSink += Map.size();
        });

        ReportHeader("erase and reinsert 100,000 entries, 20 rounds, steady size");
        ReportRow("THashMap", Baseline, 0.0);
        ReportRow("Lumina THashMap", Lumina, Baseline);
        SUCCEED();
    }

    TEST(HashMapBench, Iterate)
    {
        const std::vector<uint32>& Keys = UInt32Keys();

        TBaselineMap<uint32, uint32> Baseline;
        TLuminaMap<uint32, uint32> Lumina;
        for (uint32 Key : Keys)
        {
            Baseline[Key] = Key;
            Lumina[Key] = Key;
        }

        const double BaselineMillis = BestMillisOf(kRepeats, [&Baseline]
        {
            uint64 Sum = 0;
            for (int Round = 0; Round < 5; ++Round)
            {
                for (const auto& Pair : Baseline)
                {
                    Sum += Pair.second;
                }
            }
            GBenchSink += Sum;
        });

        const double LuminaMillis = BestMillisOf(kRepeats, [&Lumina]
        {
            uint64 Sum = 0;
            for (int Round = 0; Round < 5; ++Round)
            {
                for (const auto& Pair : Lumina)
                {
                    Sum += Pair.second;
                }
            }
            GBenchSink += Sum;
        });

        ReportHeader("5 full iterations over 200,000 entries");
        ReportRow("THashMap", BaselineMillis, 0.0);
        ReportRow("Lumina THashMap", LuminaMillis, BaselineMillis);
        SUCCEED();
    }

    TEST(HashMapBench, SetInsertAndContains)
    {
        const std::vector<uint32>& Keys = UInt32Keys();
        const std::vector<uint32>& Missing = MissingUInt32Keys();

        const double BaselineInsert = BestMillisOf(kRepeats, [&Keys]
        {
            THashSet<uint32> Set;
            for (uint32 Key : Keys)
            {
                Set.insert(Key);
            }
            GBenchSink += Set.size();
        });

        const double LuminaInsert = BestMillisOf(kRepeats, [&Keys]
        {
            TLuminaSet<uint32> Set;
            for (uint32 Key : Keys)
            {
                Set.insert(Key);
            }
            GBenchSink += Set.size();
        });

        THashSet<uint32> BaselineSet;
        TLuminaSet<uint32> LuminaSet;
        for (uint32 Key : Keys)
        {
            BaselineSet.insert(Key);
            LuminaSet.insert(Key);
        }

        const double BaselineProbe = BestMillisOf(kRepeats, [&Missing, &BaselineSet]
        {
            uint64 Hits = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (uint32 Key : Missing)
                {
                    Hits += BaselineSet.find(Key) != BaselineSet.end() ? 1 : 0;
                }
            }
            GBenchSink += Hits;
        });

        const double LuminaProbe = BestMillisOf(kRepeats, [&Missing, &LuminaSet]
        {
            uint64 Hits = 0;
            for (int Round = 0; Round < kLookupCount / kElementCount; ++Round)
            {
                for (uint32 Key : Missing)
                {
                    Hits += LuminaSet.contains(Key) ? 1 : 0;
                }
            }
            GBenchSink += Hits;
        });

        ReportHeader("insert 200,000 uint32 into a set");
        ReportRow("THashSet", BaselineInsert, 0.0);
        ReportRow("Lumina THashSet", LuminaInsert, BaselineInsert);

        ReportHeader("1,000,000 failed set probes over 200,000 entries");
        ReportRow("THashSet", BaselineProbe, 0.0);
        ReportRow("Lumina THashSet", LuminaProbe, BaselineProbe);
        SUCCEED();
    }
}
