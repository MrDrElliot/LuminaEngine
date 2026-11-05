#include <gtest/gtest.h>

#include <cstdio>
#include <optional>

#include "Platform/Time/PlatformTime.h"
#include "Containers/Algorithm.h"
#include "Containers/Function.h"
#include "Containers/FunctionRef.h"
#include "Containers/HashTable.h"
#include "Containers/Optional.h"
#include "Containers/String.h"
#include "Containers/Variant.h"
#include "Containers/Vector.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaContainerBench
{
    using Lumina::int32;
    using Lumina::uint32;
    using Lumina::uint64;
    using Lumina::FString;
    using Lumina::FStringView;
    using Lumina::TVector;
    using Lumina::Containers::TOptional;
    using Lumina::TFunction;
    using Lumina::TFunctionRef;
    using Lumina::Containers::TVariant;
    using Lumina::Containers::THashMap;

    volatile uint64 GBenchSink = 0;

    constexpr int kRepeats = 5;

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
            const uint64 Start = Lumina::PlatformTime::Cycles();
            Body();
            const uint64 Stop = Lumina::PlatformTime::Cycles();
            const double Millis = Lumina::PlatformTime::ToMilliseconds(Stop - Start);
            Best = Millis < Best ? Millis : Best;
        }
        return Best;
    }

    void Report(const char* Name, double Millis)
    {
        std::printf("  %-52s %10.3f ms\n", Name, Millis);
    }

    FString MakeAssetPath(int32 Index)
    {
        char Buffer[96];
        const int Written = std::snprintf(Buffer, sizeof(Buffer), "/Game/Content/Environment/Props/SM_Prop_%06d.lasset", Index);
        return FString(Buffer, Written);
    }

    FString MakeShortName(int32 Index)
    {
        char Buffer[16];
        const int Written = std::snprintf(Buffer, sizeof(Buffer), "Bone_%d", Index);
        return FString(Buffer, Written);
    }

    const TVector<FString>& AssetPaths()
    {
        static const TVector<FString> Paths = []
        {
            TVector<FString> Result;
            Result.Reserve(100'000);
            for (int32 Index = 0; Index < 100'000; ++Index)
            {
                Result.Add(MakeAssetPath(Index));
            }
            return Result;
        }();
        return Paths;
    }

    TEST(ContainerBench, Strings)
    {
        std::printf("\nstrings\n");

        Report("AppendChar, 20,000 strings of 64 chars", BestMillisOf(kRepeats, []
        {
            uint64 Sum = 0;
            for (int32 Round = 0; Round < 20'000; ++Round)
            {
                FString Text;
                for (int32 Index = 0; Index < 64; ++Index)
                {
                    Text.AppendChar(static_cast<char>('a' + (Index & 15)));
                }
                Sum += static_cast<uint64>(Text.Len());
            }
            GBenchSink += Sum;
        }));

        const FString Prefix("EmitterName");
        const FString Middle("0123456789ABCDEF0123456789ABCDEF");
        Report("A + \"_\" + B + \"_CS\", 200,000 times", BestMillisOf(kRepeats, [&Prefix, &Middle]
        {
            uint64 Sum = 0;
            for (int32 Round = 0; Round < 200'000; ++Round)
            {
                const FString Joined = Prefix + "_" + Middle + "_CS";
                Sum += static_cast<uint64>(Joined.Len());
            }
            GBenchSink += Sum;
        }));

        TVector<FString> Short;
        for (int32 Index = 0; Index < 200'000; ++Index)
        {
            Short.Add(MakeShortName(Index));
        }
        Report("copy TVector<FString> of 200,000 short strings", BestMillisOf(kRepeats, [&Short]
        {
            TVector<FString> Copy(Short);
            GBenchSink += static_cast<uint64>(Copy.Num());
        }));

        const TVector<FString>& Paths = AssetPaths();
        Report("copy TVector<FString> of 100,000 asset paths", BestMillisOf(kRepeats, [&Paths]
        {
            TVector<FString> Copy(Paths);
            GBenchSink += static_cast<uint64>(Copy.Num());
        }));

        Report("grow TVector<FString> to 200,000 without reserve", BestMillisOf(kRepeats, [&Short]
        {
            TVector<FString> Grown;
            for (const FString& Name : Short)
            {
                Grown.Add(Name);
            }
            GBenchSink += static_cast<uint64>(Grown.Num());
        }));

        Report("compare 100,000 path pairs sharing a prefix", BestMillisOf(kRepeats, [&Paths]
        {
            int64_t Sum = 0;
            for (int32 Index = 1; Index < Paths.Num(); ++Index)
            {
                Sum += Paths[Index - 1].View().Compare(Paths[Index].View());
            }
            GBenchSink += static_cast<uint64>(Sum);
        }));

        Report("Find(\".lasset\") in 100,000 asset paths", BestMillisOf(kRepeats, [&Paths]
        {
            uint64 Sum = 0;
            for (const FString& Path : Paths)
            {
                Sum += static_cast<uint64>(Path.Find(".lasset"));
            }
            GBenchSink += Sum;
        }));

        TVector<FString> Shuffled(Paths);
        FBenchRandom Random;
        for (int32 Index = Shuffled.Num() - 1; Index > 0; --Index)
        {
            Shuffled.Swap(Index, static_cast<int32>(Random.Next() % static_cast<uint64>(Index + 1)));
        }
        Report("Algo::Sort 100,000 shuffled asset paths", BestMillisOf(kRepeats, [&Shuffled]
        {
            TVector<FString> Work(Shuffled);
            Lumina::Algo::Sort(Work);
            GBenchSink += static_cast<uint64>(Work[0].Len());
        }));
        SUCCEED();
    }

    TEST(ContainerBench, StringKeyedMap)
    {
        std::printf("\nstring-keyed THashMap\n");

        const TVector<FString>& Paths = AssetPaths();

        Report("insert 100,000 asset paths without reserve", BestMillisOf(kRepeats, [&Paths]
        {
            THashMap<FString, int32> Map;
            for (int32 Index = 0; Index < Paths.Num(); ++Index)
            {
                Map.Add(Paths[Index], Index);
            }
            GBenchSink += static_cast<uint64>(Map.Num());
        }));

        THashMap<FString, int32> Map;
        for (int32 Index = 0; Index < Paths.Num(); ++Index)
        {
            Map.Add(Paths[Index], Index);
        }

        Report("1,000,000 hits by FStringView", BestMillisOf(kRepeats, [&Paths, &Map]
        {
            uint64 Sum = 0;
            for (int32 Round = 0; Round < 10; ++Round)
            {
                for (const FString& Path : Paths)
                {
                    Sum += static_cast<uint64>(*Map.Find(Path.View()));
                }
            }
            GBenchSink += Sum;
        }));

        Report("copy the 100,000-entry map", BestMillisOf(kRepeats, [&Map]
        {
            THashMap<FString, int32> Copy(Map);
            GBenchSink += static_cast<uint64>(Copy.Num());
        }));

        THashMap<uint32, uint32> IntMap;
        FBenchRandom Random;
        for (int32 Index = 0; Index < 200'000; ++Index)
        {
            IntMap.Add(static_cast<uint32>(Random.Next()), static_cast<uint32>(Index));
        }
        Report("copy a 200,000-entry uint32 map", BestMillisOf(kRepeats, [&IntMap]
        {
            THashMap<uint32, uint32> Copy(IntMap);
            GBenchSink += static_cast<uint64>(Copy.Num());
        }));

        Report("churn: 2,000,000 insert/erase at ~60% load", BestMillisOf(kRepeats, []
        {
            THashMap<uint32, uint32> Churn;
            FBenchRandom Keys;
            TVector<uint32> Live;
            Live.Reserve(40'000);
            for (int32 Index = 0; Index < 40'000; ++Index)
            {
                const uint32 Key = static_cast<uint32>(Keys.Next());
                Churn.Add(Key, Key);
                Live.Add(Key);
            }
            for (int32 Step = 0; Step < 2'000'000; ++Step)
            {
                const int32 Slot = static_cast<int32>(Keys.Next() % static_cast<uint64>(Live.Num()));
                Churn.Remove(Live[Slot]);
                const uint32 Key = static_cast<uint32>(Keys.Next());
                Churn.Add(Key, Key);
                Live[Slot] = Key;
            }
            GBenchSink += static_cast<uint64>(Churn.Num()) + static_cast<uint64>(Churn.Max());
        }));
        SUCCEED();
    }

    TEST(ContainerBench, Vocabulary)
    {
        std::printf("\noptional, variant, function\n");

        Report("grow TVector<TOptional<int32>> to 1,000,000", BestMillisOf(kRepeats, []
        {
            TVector<TOptional<int32>> Values;
            for (int32 Index = 0; Index < 1'000'000; ++Index)
            {
                Values.Add(TOptional<int32>(Index));
            }
            GBenchSink += static_cast<uint64>(Values.Num());
        }));

        Report("grow TVector<std::optional<int32>> to 1,000,000", BestMillisOf(kRepeats, []
        {
            TVector<std::optional<int32>> Values;
            for (int32 Index = 0; Index < 1'000'000; ++Index)
            {
                Values.Add(std::optional<int32>(Index));
            }
            GBenchSink += static_cast<uint64>(Values.Num());
        }));

        Report("grow TVector<int64> to 1,000,000", BestMillisOf(kRepeats, []
        {
            TVector<Lumina::int64> Values;
            for (int32 Index = 0; Index < 1'000'000; ++Index)
            {
                Values.Add(static_cast<Lumina::int64>(Index));
            }
            GBenchSink += static_cast<uint64>(Values.Num());
        }));

        Report("grow TVector<TOptional<FString>> to 200,000", BestMillisOf(kRepeats, []
        {
            TVector<TOptional<FString>> Values;
            for (int32 Index = 0; Index < 200'000; ++Index)
            {
                Values.Add(TOptional<FString>(FString("value")));
            }
            GBenchSink += static_cast<uint64>(Values.Num());
        }));

        using FScalarVariant = TVariant<int32, float, uint64>;
        TVector<FScalarVariant> Variants;
        for (int32 Index = 0; Index < 1'000'000; ++Index)
        {
            switch (Index % 3)
            {
                case 0: Variants.Add(FScalarVariant(Index)); break;
                case 1: Variants.Add(FScalarVariant(static_cast<float>(Index))); break;
                default: Variants.Add(FScalarVariant(static_cast<uint64>(Index))); break;
            }
        }
        Report("Visit 1,000,000 TVariant<int32, float, uint64>", BestMillisOf(kRepeats, [&Variants]
        {
            uint64 Sum = 0;
            for (const FScalarVariant& Value : Variants)
            {
                Sum += Lumina::Containers::Visit([](const auto& Held) { return static_cast<uint64>(Held); }, Value);
            }
            GBenchSink += Sum;
        }));

        Report("copy TVector of those 1,000,000 variants", BestMillisOf(kRepeats, [&Variants]
        {
            TVector<FScalarVariant> Copy(Variants);
            GBenchSink += static_cast<uint64>(Copy.Num());
        }));

        float Scale = 1.0001f;
        const TFunction<float(float)> Function = [&Scale](float Value) { return Value * Scale + 1.0f; };
        Report("10,000,000 TFunction<float(float)> calls", BestMillisOf(kRepeats, [&Function]
        {
            float Value = 0.0f;
            for (int32 Index = 0; Index < 10'000'000; ++Index)
            {
                Value = Function(Value);
            }
            GBenchSink += static_cast<uint64>(Value);
        }));

        const auto Lambda = [&Scale](float Value) { return Value * Scale + 1.0f; };
        const TFunctionRef<float(float)> Ref = Lambda;
        Report("10,000,000 TFunctionRef<float(float)> calls", BestMillisOf(kRepeats, [&Ref]
        {
            float Value = 0.0f;
            for (int32 Index = 0; Index < 10'000'000; ++Index)
            {
                Value = Ref(Value);
            }
            GBenchSink += static_cast<uint64>(Value);
        }));
        SUCCEED();
    }

    TEST(ContainerBench, Algorithms)
    {
        std::printf("\nalgorithms\n");

        TVector<uint32> Random;
        FBenchRandom Source;
        for (int32 Index = 0; Index < 1'000'000; ++Index)
        {
            Random.Add(static_cast<uint32>(Source.Next()));
        }

        Report("Algo::Sort 1,000,000 random uint32", BestMillisOf(kRepeats, [&Random]
        {
            TVector<uint32> Work(Random);
            Lumina::Algo::Sort(Work);
            GBenchSink += Work[0];
        }));

        TVector<uint32> Sorted(Random);
        Lumina::Algo::Sort(Sorted);
        Report("Algo::Sort 1,000,000 already sorted uint32", BestMillisOf(kRepeats, [&Sorted]
        {
            TVector<uint32> Work(Sorted);
            Lumina::Algo::Sort(Work);
            GBenchSink += Work[0];
        }));

        TVector<uint32> Table;
        for (int32 Index = 0; Index < 100'000; ++Index)
        {
            Table.Add(static_cast<uint32>(Index) * 7u);
        }
        Report("1,000,000 LowerBound over 100,000 sorted uint32", BestMillisOf(kRepeats, [&Table, &Random]
        {
            uint64 Sum = 0;
            for (uint32 Key : Random)
            {
                Sum += static_cast<uint64>(Lumina::Algo::LowerBound(Table, Key % 700'000u) - Table.begin());
            }
            GBenchSink += Sum;
        }));

        TVector<uint32> Haystack;
        for (int32 Index = 0; Index < 4'096; ++Index)
        {
            Haystack.Add(static_cast<uint32>(Index));
        }
        Report("TVector<uint32>::Find, 20,000 misses over 4,096", BestMillisOf(kRepeats, [&Haystack]
        {
            uint64 Sum = 0;
            for (int32 Round = 0; Round < 20'000; ++Round)
            {
                Sum += static_cast<uint64>(Haystack.Find(static_cast<uint32>(5'000 + Round)) + 1);
            }
            GBenchSink += Sum;
        }));

        TVector<uint8> Bytes;
        for (int32 Index = 0; Index < 65'536; ++Index)
        {
            Bytes.Add(static_cast<uint8>(Index % 251));
        }
        Report("TVector<uint8>::Find, 20,000 misses over 65,536", BestMillisOf(kRepeats, [&Bytes]
        {
            uint64 Sum = 0;
            for (int32 Round = 0; Round < 20'000; ++Round)
            {
                Sum += static_cast<uint64>(Bytes.Find(static_cast<uint8>(251 + (Round & 3))) + 1);
            }
            GBenchSink += Sum;
        }));

        TVector<FString> Names;
        for (int32 Index = 0; Index < 50'000; ++Index)
        {
            Names.Add(MakeShortName(Index));
        }
        Report("TVector<FString> copy-assign over 50,000 live strings", BestMillisOf(kRepeats, [&Names]
        {
            TVector<FString> Target(Names);
            for (int32 Round = 0; Round < 4; ++Round)
            {
                Target = Names;
            }
            GBenchSink += static_cast<uint64>(Target.Num());
        }));
        SUCCEED();
    }
}
