#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#include "Containers/Algorithm.h"
#include "Containers/Deque.h"
#include "Containers/Function.h"
#include "Containers/FunctionRef.h"
#include "Containers/HashTable.h"
#include "Containers/List.h"
#include "Containers/Name.h"
#include "Containers/Optional.h"
#include "Containers/Queue.h"
#include "Containers/SegmentArray.h"
#include "Containers/Span.h"
#include "Containers/StaticArray.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"
#include "Containers/Variant.h"
#include "Containers/Vector.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaContainerAuditTests
{
    using Lumina::int32;
    using Lumina::uint8;
    using Lumina::uint32;
    using Lumina::uint64;
    using Lumina::FName;
    using Lumina::FString;
    using Lumina::FStringView;
    using Lumina::TVector;
    using Lumina::TDeque;
    using Lumina::TFunctionRef;
    using Lumina::Containers::TArray;
    using Lumina::Containers::THashMap;
    using Lumina::Containers::TOptional;
    using Lumina::Containers::TVariant;

    struct FNode
    {
        int32 Value = 0;
        TVector<FNode> Children;
    };

    int32 AddOne(int32 Value)
    {
        return Value + 1;
    }

    TEST(ContainerAudit, AppendMovesOutOfOneOfOurOwnElements)
    {
        TVector<FNode> Nodes;
        for (int32 Index = 0; Index < 4; ++Index)
        {
            Nodes.Add(FNode{ Index, {} });
        }
        for (int32 Child = 0; Child < 8; ++Child)
        {
            Nodes[1].Children.Add(FNode{ 100 + Child, {} });
        }

        // The growth relocates Nodes[1], which is the very vector being appended.
        Nodes.Append(std::move(Nodes[1].Children));

        ASSERT_EQ(Nodes.Num(), 12);
        EXPECT_TRUE(Nodes[1].Children.IsEmpty());
        for (int32 Child = 0; Child < 8; ++Child)
        {
            EXPECT_EQ(Nodes[4 + Child].Value, 100 + Child);
        }
    }

    TEST(ContainerAudit, MoveAssigningAnEmptyVectorReleasesTheBlock)
    {
        TVector<int32> Values;
        Values.Reserve(1024);
        ASSERT_GE(Values.Max(), 1024);

        Values = TVector<int32>();
        EXPECT_EQ(Values.Max(), 0);
        EXPECT_EQ(Values.GetAllocatedSize(), 0u);
    }

    TEST(ContainerAudit, AppendingOurOwnReverseRangeStagesIt)
    {
        TVector<FString> Words;
        for (int32 Index = 0; Index < 5; ++Index)
        {
            Words.Add(Lumina::Format("word-number-{}-that-is-long", Index));
        }
        Words.Append(Words.rbegin(), Words.rend());

        ASSERT_EQ(Words.Num(), 10);
        for (int32 Index = 0; Index < 5; ++Index)
        {
            EXPECT_EQ(Words[5 + Index], Words[4 - Index]);
        }

        FString Text("abcdefghijklmnopqrstuvwxyz");
        Text.Append(Text.rbegin(), Text.rend());
        EXPECT_EQ(Text, "abcdefghijklmnopqrstuvwxyzzyxwvutsrqponmlkjihgfedcba");
    }

    TEST(ContainerAudit, CopyAssignOverLiveElementsMatchesAFreshCopy)
    {
        TVector<FString> Source;
        for (int32 Index = 0; Index < 40; ++Index)
        {
            Source.Add(Lumina::Format("entry {} with enough text to leave the inline buffer", Index));
        }

        TVector<FString> Target;
        for (int32 Index = 0; Index < 60; ++Index)
        {
            Target.Add(FString("stale"));
        }

        Target = Source;
        EXPECT_EQ(Target, Source);

        Source.SetNum(10);
        Target = Source;
        EXPECT_EQ(Target, Source);
    }

    TEST(ContainerAudit, FindMatchesALinearScanForEveryWidth)
    {
        TVector<uint8> Bytes;
        TVector<uint32> Words;
        TVector<uint64> Wides;
        TVector<const void*> Pointers;
        for (int32 Index = 0; Index < 300; ++Index)
        {
            Bytes.Add(static_cast<uint8>(Index * 7));
            Words.Add(static_cast<uint32>(Index * 13));
            Wides.Add(static_cast<uint64>(Index) << 33);
            Pointers.Add(reinterpret_cast<const void*>(static_cast<uintptr_t>(Index + 1) * 16));
        }

        for (int32 Index = 0; Index < 300; ++Index)
        {
            const uint8 Byte = static_cast<uint8>(Index * 7);
            EXPECT_EQ(Bytes.Find(Byte), static_cast<int32>(std::find(Bytes.begin(), Bytes.end(), Byte) - Bytes.begin()));
            EXPECT_EQ(Words.Find(static_cast<uint32>(Index * 13)), Index);
            EXPECT_EQ(Wides.Find(static_cast<uint64>(Index) << 33), Index);
            EXPECT_EQ(Pointers.Find(reinterpret_cast<const void*>(static_cast<uintptr_t>(Index + 1) * 16)), Index);
        }
        EXPECT_EQ(Words.Find(7u), Lumina::Constants::kIndexNone);
        EXPECT_EQ(TVector<uint32>().Find(0u), Lumina::Constants::kIndexNone);
    }

    TEST(ContainerAudit, HashMapCopyKeepsEveryEntryAroundTombstones)
    {
        THashMap<uint32, uint32> Source;
        for (uint32 Key = 0; Key < 500; ++Key)
        {
            Source.Add(Key, Key * 3);
        }
        for (uint32 Key = 0; Key < 500; Key += 3)
        {
            Source.Remove(Key);
        }

        const THashMap<uint32, uint32> Copy(Source);
        EXPECT_EQ(Copy.Num(), Source.Num());
        for (uint32 Key = 0; Key < 500; ++Key)
        {
            const uint32* Found = Copy.Find(Key);
            if (Key % 3 == 0)
            {
                EXPECT_EQ(Found, nullptr);
            }
            else
            {
                ASSERT_NE(Found, nullptr);
                EXPECT_EQ(*Found, Key * 3);
            }
        }

        THashMap<FString, int32> Strings;
        for (int32 Index = 0; Index < 200; ++Index)
        {
            Strings.Add(Lumina::Format("key {} long enough for the heap", Index), Index);
        }
        THashMap<FString, int32> Assigned;
        Assigned.Add(FString("old"), -1);
        Assigned = Strings;
        EXPECT_EQ(Assigned, Strings);
        EXPECT_EQ(Assigned.Find(FString("old")), nullptr);
    }

    TEST(ContainerAudit, HashMapSurvivesChurnAtEverySize)
    {
        for (uint32 Size : { 1u, 3u, 7u, 15u, 40u, 300u })
        {
            THashMap<uint32, uint32> Map;
            std::vector<uint32> Live;
            uint64 State = 0x12345678ull + Size;
            for (uint32 Step = 0; Step < Size; ++Step)
            {
                Map.Add(Step, Step);
                Live.push_back(Step);
            }
            uint32 NextKey = Size;
            for (int32 Step = 0; Step < 20'000; ++Step)
            {
                State = State * 6364136223846793005ull + 1442695040888963407ull;
                const size_t Slot = static_cast<size_t>(State >> 33) % Live.size();
                EXPECT_EQ(Map.Remove(Live[Slot]), 1);
                Live[Slot] = NextKey;
                Map.Add(NextKey, NextKey);
                ++NextKey;
            }
            ASSERT_EQ(Map.Num(), static_cast<int32>(Live.size()));
            for (uint32 Key : Live)
            {
                ASSERT_NE(Map.Find(Key), nullptr);
            }
            EXPECT_LE(Map.Max(), static_cast<int32>(Size * 4 + 16)) << "churn should rehash in place, not keep doubling";
        }
    }

    TEST(ContainerAudit, FunctionRefHoldsAFunctionPointerByValue)
    {
        const TFunctionRef<int32(int32)> Ref = &AddOne;
        volatile int32 Scratch[64] = {};
        for (int32 Index = 0; Index < 64; ++Index)
        {
            Scratch[Index] = Index;
        }
        EXPECT_EQ(Ref(41), 42);

        int32 (*Null)(int32) = nullptr;
        const TFunctionRef<int32(int32)> Empty = Null;
        EXPECT_FALSE(Empty.IsSet());
    }

    TEST(ContainerAudit, StaticArrayBindsByValue)
    {
        TArray<FString, 2> Pair = { FString("left side that is long enough"), FString("right") };
        auto [Left, Right] = Pair;
        EXPECT_EQ(Left, "left side that is long enough");
        EXPECT_EQ(Right, "right");

        auto [MovedLeft, MovedRight] = std::move(Pair);
        EXPECT_EQ(MovedLeft, "left side that is long enough");
        EXPECT_EQ(MovedRight, "right");
    }

    TEST(ContainerAudit, VocabularyTypesAreTrivialWhenTheirContentsAre)
    {
        static_assert(std::is_trivially_copyable_v<TOptional<int32>>);
        static_assert(std::is_trivially_copyable_v<TOptional<FStringView>>);
        static_assert(!std::is_trivially_copyable_v<TOptional<FString>>);
        static_assert(Lumina::TIsTriviallyRelocatable_V<TOptional<FString>>);

        static_assert(std::is_trivially_copyable_v<TVariant<int32, float>>);
        static_assert(sizeof(TVariant<int32, float>) == 8);
        static_assert(!std::is_trivially_copyable_v<TVariant<int32, FString>>);
        static_assert(Lumina::TIsTriviallyRelocatable_V<TVariant<int32, FString>>);

        static_assert(Lumina::TIsTriviallyRelocatable_V<FString>);
        static_assert(Lumina::TIsTriviallyRelocatable_V<Lumina::TPair<FString, TVector<int32>>>);
        static_assert(Lumina::TIsTriviallyRelocatable_V<THashMap<FString, int32>>);

        TOptional<int32> Number(7);
        TOptional<int32> Copy = Number;
        EXPECT_EQ(*Copy, 7);

        TVariant<int32, float> Scalar(2.5f);
        TVariant<int32, float> ScalarCopy = Scalar;
        EXPECT_FLOAT_EQ(ScalarCopy.Get<float>(), 2.5f);
        EXPECT_EQ(Lumina::Containers::Visit([](auto Held) { return static_cast<int32>(Held * 2); }, ScalarCopy), 5);
    }

    TEST(ContainerAudit, DequeIteratorsAreRandomAccess)
    {
        TDeque<int32> Values;
        for (int32 Index = 0; Index < 50; ++Index)
        {
            Values.PushLast(50 - Index);
            Values.PushFirst(100 + Index);
        }
        Lumina::Algo::Sort(Values.begin(), Values.end());
        EXPECT_TRUE(std::is_sorted(Values.begin(), Values.end()));
        EXPECT_EQ(Values.begin()[0], 1);
        EXPECT_EQ(*(2 + Values.begin()), 3);
    }

    TEST(ContainerAudit, BinarySearchesMatchTheStandard)
    {
        std::vector<int32> Sorted;
        uint64 State = 99;
        for (int32 Index = 0; Index < 1000; ++Index)
        {
            State = State * 6364136223846793005ull + 1442695040888963407ull;
            Sorted.push_back(static_cast<int32>((State >> 40) % 300));
        }
        std::sort(Sorted.begin(), Sorted.end());

        for (int32 Value = -5; Value < 310; ++Value)
        {
            EXPECT_EQ(Lumina::Algo::LowerBound(Sorted.begin(), Sorted.end(), Value), std::lower_bound(Sorted.begin(), Sorted.end(), Value));
            EXPECT_EQ(Lumina::Algo::UpperBound(Sorted.begin(), Sorted.end(), Value), std::upper_bound(Sorted.begin(), Sorted.end(), Value));
        }
        EXPECT_EQ(Lumina::Algo::LowerBound(Sorted.begin(), Sorted.begin(), 3), Sorted.begin());
    }

    TEST(ContainerAudit, SortingStringsByRelocationKeepsEveryString)
    {
        TVector<FString> Words;
        std::vector<std::string> Expected;
        uint64 State = 7;
        for (int32 Index = 0; Index < 3000; ++Index)
        {
            State = State * 6364136223846793005ull + 1442695040888963407ull;
            const FString Word = Lumina::Format("/Game/Shared/Prefix/{}{}", (State >> 50) % 97, Index % 3 == 0 ? "-and-a-longer-tail-past-sso" : "");
            Words.Add(Word);
            Expected.emplace_back(*Word);
        }
        Lumina::Algo::Sort(Words);
        std::sort(Expected.begin(), Expected.end());
        ASSERT_EQ(Words.Num(), static_cast<int32>(Expected.size()));
        for (int32 Index = 0; Index < Words.Num(); ++Index)
        {
            EXPECT_EQ(FStringView(Words[Index]), FStringView(Expected[static_cast<size_t>(Index)].c_str()));
        }
    }

    TEST(ContainerAudit, ReplaceAgreesWithTheForwardScan)
    {
        FString Text("aaaa-bb-aaaa");
        EXPECT_EQ(Text.Replace("aa", "x"), "xx-bb-xx");
        EXPECT_EQ(Text.Replace("aa", "yyy"), "yyyyyy-bb-yyyyyy");
        EXPECT_EQ(Text.Replace("aa", "zz"), "zzzz-bb-zzzz");
        EXPECT_EQ(Text.Replace("q", "zz"), Text);

        FString Path("C:\\Game\\Content\\Assets\\Thing.lasset");
        EXPECT_EQ(Path.ReplaceInline("\\", "/"), 4);
        EXPECT_EQ(Path, "C:/Game/Content/Assets/Thing.lasset");

        FString Growing("a.b.c");
        EXPECT_EQ(Growing.ReplaceInline(".", "::"), 2);
        EXPECT_EQ(Growing, "a::b::c");

        FString SelfReferential("abcabc");
        EXPECT_EQ(SelfReferential.ReplaceInline(SelfReferential.View().Left(3), SelfReferential.View().Right(1)), 2);
        EXPECT_EQ(SelfReferential, "cc");
    }

    TEST(ContainerAudit, ConcatenationChainsAndNullText)
    {
        const FString Prefix("EmitterName");
        const FString Joined = Prefix + "_" + FString("0123456789ABCDEF0123456789") + "_CS";
        EXPECT_EQ(Joined, "EmitterName_0123456789ABCDEF0123456789_CS");
        EXPECT_EQ(FStringView("left") + Prefix, "leftEmitterName");
        EXPECT_EQ(Prefix + '!', "EmitterName!");

        FString Text("x");
        const char* Null = nullptr;
        Text.Append(Null);
        Text += Null;
        EXPECT_EQ(Text + Null, "x");
        EXPECT_EQ(Text, "x");
    }

    TEST(ContainerAudit, InlineStringsGrowAndShrinkThroughTheHeap)
    {
        FString Text;
        std::string Expected;
        for (int32 Index = 0; Index < 200; ++Index)
        {
            const char Character = static_cast<char>('a' + Index % 26);
            Text.AppendChar(Character);
            Expected.push_back(Character);
            ASSERT_EQ(Text.Len(), static_cast<int32>(Expected.size()));
            ASSERT_EQ(FStringView(Text), FStringView(Expected.c_str()));
            ASSERT_EQ((*Text)[Text.Len()], '\0');
        }
        Text.LeftInline(15);
        Text.Shrink();
        EXPECT_TRUE(Text.IsInline());
        EXPECT_EQ(Text, FStringView(Expected.c_str()).Left(15));

        FString Copy(Text);
        EXPECT_EQ(Copy, Text);
        Copy.ToUpper();
        EXPECT_EQ(Copy, "ABCDEFGHIJKLMNO");
        Copy.ToLower();
        EXPECT_EQ(Copy, Text);
    }

    TEST(ContainerAudit, FormatIntoTheBufferThatIsAnArgument)
    {
        FString Path("Content/Folder/Name/That/Is/Longer/Than/Inline");
        Lumina::FormatTo(Path, "{}/{}", Path, "Child");
        EXPECT_EQ(Path, "Content/Folder/Name/That/Is/Longer/Than/Inline/Child");

        Lumina::FStringBuilder Builder;
        for (int32 Index = 0; Index < 40; ++Index)
        {
            Builder.Append("segment-");
        }
        const FString Before = Builder.ToString();
        Builder.Append(Builder.View());
        EXPECT_EQ(Builder.ToString(), Before + Before);
    }

    TEST(ContainerAudit, StringViewsAreBorrowedRanges)
    {
        static_assert(std::ranges::borrowed_range<FStringView>);
        static_assert(std::ranges::borrowed_range<Lumina::TSpan<const int32>>);
        const auto Hit = std::ranges::find(FStringView("needle in hay"), 'i');
        EXPECT_EQ(*Hit, 'i');
        EXPECT_EQ(FStringView("abc").Compare(FStringView("ab\xff")), -1) << "comparison is unsigned on every platform";
        EXPECT_EQ(FStringView("a,b;c").FindFirstOf(",;"), 1);
        EXPECT_EQ(FStringView("a,b;c").FindLastOf(",;"), 3);
        EXPECT_EQ(FStringView("  \tx ").FindFirstNotOf(" \t"), 3);
    }

    TEST(ContainerAudit, NumberedNamesSpellAndIndexWithoutAllocating)
    {
        const FName Name("SomeActor_42");
        EXPECT_EQ(Name.GetNumber(), 42u);
        EXPECT_EQ(Name.Len(), 12);
        EXPECT_STREQ(*Name, "SomeActor_42");
        EXPECT_EQ(Name.ToString(), "SomeActor_42");
        EXPECT_EQ(Name.At(0), 'S');
        EXPECT_EQ(Name.At(9), '_');
        EXPECT_EQ(Name.At(10), '4');
        EXPECT_EQ(Name.At(11), '2');
        EXPECT_EQ(Name.At(12), '\0');
        EXPECT_EQ(FName(FString("SomeActor_42")), Name);
        EXPECT_EQ(FName("someactor"), Name.GetBaseName());
    }

    TEST(ContainerAudit, ConstObjectsHandOutConstAccess)
    {
        static_assert(std::is_same_v<decltype(std::declval<const TVector<int32>&>()[0]), const int32&>);
        static_assert(std::is_same_v<decltype(std::declval<TVector<int32>&>()[0]), int32&>);
        static_assert(std::is_same_v<decltype(std::declval<const TVector<int32>&>().GetData()), const int32*>);
        static_assert(std::is_same_v<decltype(std::declval<const TVector<int32>&>().begin()), const int32*>);
        static_assert(std::is_same_v<decltype(std::declval<const TVector<int32>&>().Last()), const int32&>);
        static_assert(std::is_same_v<decltype(std::declval<const FString&>().GetData()), const char*>);
        static_assert(std::is_same_v<decltype(std::declval<FString&>().GetData()), char*>);
        static_assert(std::is_same_v<decltype(std::declval<const THashMap<int32, int32>&>().Find(0)), const int32*>);
        static_assert(std::is_same_v<decltype(std::declval<THashMap<int32, int32>&>().Find(0)), int32*>);
        static_assert(std::is_same_v<decltype(*std::declval<const THashMap<int32, int32>&>().begin()), const Lumina::TPair<const int32, int32>&>);
        static_assert(std::is_same_v<decltype(std::declval<const TDeque<int32>&>()[0]), const int32&>);
        static_assert(std::is_same_v<decltype(std::declval<const TOptional<int32>&>().GetValue()), const int32&>);
        static_assert(std::is_same_v<decltype(std::declval<TOptional<int32>&&>().GetValue()), int32&&>);
        static_assert(std::is_same_v<decltype(std::declval<const TVariant<int32, float>&>().Get<float>()), const float&>);
        static_assert(std::is_same_v<decltype(std::declval<const Lumina::Containers::TList<int32>&>().Head()), const int32&>);
        static_assert(std::is_same_v<decltype(std::declval<const Lumina::Containers::TQueue<int32>&>().Peek()), const int32&>);
        static_assert(std::is_same_v<decltype(std::declval<const Lumina::TSegmentMap<int32>&>()[Lumina::THandle<int32>{}]), const int32&>);
        SUCCEED();
    }

    TEST(ContainerAudit, SparseHashMapCopiesCompactWhileDenseOnesKeepTheirLayout)
    {
        THashMap<uint32, FString> Sparse;
        for (uint32 Key = 0; Key < 2000; ++Key)
        {
            Sparse.Add(Key, Lumina::Format("value {} long enough for the heap", Key));
        }
        for (uint32 Key = 0; Key < 2000; ++Key)
        {
            if (Key % 50 != 0)
            {
                Sparse.Remove(Key);
            }
        }
        ASSERT_EQ(Sparse.Num(), 40);

        const THashMap<uint32, FString> Copy(Sparse);
        EXPECT_EQ(Copy, Sparse);
        EXPECT_LT(Copy.Max(), Sparse.Max()) << "a source over twice too big is compacted";

        THashMap<uint32, FString> Assigned;
        for (uint32 Key = 5000; Key < 5100; ++Key)
        {
            Assigned.Add(Key, FString("stale"));
        }
        Assigned = Sparse;
        EXPECT_EQ(Assigned, Sparse);

        Lumina::Containers::TInlineHashMap<uint32, FString, 15> Inline;
        for (uint32 Key = 0; Key < 10; ++Key)
        {
            Inline.Add(Key, Lumina::Format("inline {} with a heap-sized payload", Key));
        }
        Lumina::Containers::TInlineHashMap<uint32, FString, 15> InlineCopy(Inline);
        EXPECT_TRUE(InlineCopy.IsInline());
        EXPECT_EQ(InlineCopy, Inline);
        InlineCopy = Inline;
        EXPECT_EQ(InlineCopy, Inline);
    }

    TEST(ContainerAudit, SegmentMapDestroysWithItsDefaultDestructor)
    {
        static int32 Live = 0;
        struct FCounted
        {
            FCounted() { ++Live; }
            ~FCounted() { --Live; }
            FString Text = FString("payload that is long enough for the heap");
        };

        {
            Lumina::TSegmentMap<FCounted> Map;
            TVector<Lumina::THandle<FCounted>> Handles;
            for (int32 Index = 0; Index < 300; ++Index)
            {
                Handles.Add(Map.Emplace());
            }
            EXPECT_EQ(Live, 300);
            for (int32 Index = 0; Index < 300; Index += 2)
            {
                Map.Erase(Handles[Index]);
            }
            EXPECT_EQ(Live, 150);
            EXPECT_FALSE(Map.IsLive(Handles[0]));
            EXPECT_TRUE(Map.IsLive(Handles[1]));
            const Lumina::TSegmentMap<FCounted>& ConstMap = Map;
            EXPECT_EQ(ConstMap[Handles[1]].Text.Len(), 40);
            EXPECT_EQ(ConstMap.TryGet(Handles[0]), nullptr);
        }
        EXPECT_EQ(Live, 0) << "the map's destructor destroys what is still live";
    }

    TEST(ContainerAudit, DequeKeepsOrderAcrossWrappedGrowth)
    {
        TDeque<FString> Values;
        std::vector<std::string> Expected;
        for (int32 Index = 0; Index < 200; ++Index)
        {
            const FString Text = Lumina::Format("element {} past the inline size", Index);
            if (Index % 3 == 0)
            {
                Values.PushFirst(Text);
                Expected.insert(Expected.begin(), std::string(*Text));
            }
            else
            {
                Values.PushLast(Text);
                Expected.emplace_back(*Text);
            }
            if (Index % 7 == 0)
            {
                Values.PopFirst();
                Expected.erase(Expected.begin());
            }
        }
        ASSERT_EQ(Values.Num(), static_cast<int32>(Expected.size()));
        for (int32 Index = 0; Index < Values.Num(); ++Index)
        {
            EXPECT_EQ(FStringView(Values[Index]), FStringView(Expected[static_cast<size_t>(Index)].c_str()));
        }
    }

    TEST(ContainerAudit, ByteHashDependsOnlyOnContent)
    {
        alignas(16) char Source[256];
        for (int32 Index = 0; Index < 256; ++Index)
        {
            Source[Index] = static_cast<char>(Index * 37 + 11);
        }
        alignas(16) char Shifted[272];
        THashMap<uint64, int32> Seen;
        for (int32 Length = 0; Length <= 200; ++Length)
        {
            for (int32 Offset = 1; Offset < 16; Offset += 5)
            {
                std::memcpy(Shifted + Offset, Source, static_cast<size_t>(Length));
                EXPECT_EQ(Lumina::Containers::HashBytes(Shifted + Offset, static_cast<size_t>(Length)),
                          Lumina::Containers::HashBytes(Source, static_cast<size_t>(Length))) << "length " << Length;
            }
            EXPECT_TRUE(Seen.TryAdd(Lumina::Containers::HashBytes(Source, static_cast<size_t>(Length)), Length)) << "length " << Length << " collides";
        }

        const FString Text("/Game/Content/Asset.lasset");
        EXPECT_EQ(GetTypeHash(Text), GetTypeHash(Text.View()));
    }

    TEST(ContainerAudit, InsertingTextThatPointsIntoTheString)
    {
        FString Text("0123456789abcdefghij");
        Text.InsertAt(0, Text.View().Mid(10, 5));
        EXPECT_EQ(Text, "abcde0123456789abcdefghij");

        FString Short("abc");
        Short.InsertAt(3, Short.View());
        EXPECT_EQ(Short, "abcabc");
    }

    TEST(ContainerAudit, WideVariantsVisitEveryAlternative)
    {
        using FWide = TVariant<int8, int16, int32, int64, uint8, uint16, uint32, uint64, float, double, FString>;
        static_assert(sizeof(FWide) == sizeof(FString) + 8);
        TVector<FWide> Values;
        Values.Add(FWide(int8(1)));
        Values.Add(FWide(int16(2)));
        Values.Add(FWide(int32(3)));
        Values.Add(FWide(int64(4)));
        Values.Add(FWide(uint8(5)));
        Values.Add(FWide(uint16(6)));
        Values.Add(FWide(uint32(7)));
        Values.Add(FWide(uint64(8)));
        Values.Add(FWide(9.0f));
        Values.Add(FWide(10.0));
        Values.Add(FWide(FString("eleven")));

        int32 Total = 0;
        for (const FWide& Value : Values)
        {
            Total += Lumina::Containers::Visit([](const auto& Held) -> int32
            {
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(Held)>, FString>)
                {
                    return Held.Len() + 5;
                }
                else
                {
                    return static_cast<int32>(Held);
                }
            }, Value);
        }
        EXPECT_EQ(Total, 66);

        const TVector<FWide> Copy(Values);
        EXPECT_EQ(Copy[10].Get<FString>(), "eleven");
        EXPECT_EQ(Copy[7].GetIndex(), 7u);
    }

    // The old target dies after the new one is installed, so a destructor that clears its owner runs exactly once.
    TEST(ContainerAudit, FunctionTargetMayReassignItsOwnerWhileDying)
    {
        static int32 Destroyed = 0;
        struct FResetOnDestroy
        {
            Lumina::TFunction<void()>* Owner = nullptr;
            ~FResetOnDestroy()
            {
                ++Destroyed;
                if (Owner != nullptr)
                {
                    *Owner = nullptr;
                }
            }
        };

        for (int32 Path = 0; Path < 3; ++Path)
        {
            Destroyed = 0;
            Lumina::TFunction<void()> OnDone;
            auto Guard = std::make_shared<FResetOnDestroy>();
            Guard->Owner = &OnDone;
            OnDone = [Guard] {};
            Guard.reset();

            if (Path == 0)
            {
                OnDone = [] {};
            }
            else if (Path == 1)
            {
                Lumina::TFunction<void()> Replacement = [] {};
                OnDone = Replacement;
            }
            else
            {
                OnDone.Reset();
            }
            EXPECT_EQ(Destroyed, 1) << "path " << Path;
            EXPECT_FALSE(OnDone.IsSet()) << "path " << Path;
        }
    }

    TEST(ContainerAudit, StringsOfDifferentCapacityConcatenate)
    {
        const FString Wide("left-");
        const Lumina::FFixedString Fixed("right");
        EXPECT_EQ(Wide + Fixed, "left-right");
        EXPECT_EQ(Fixed + Wide, "rightleft-");
    }

    TEST(ContainerAudit, CopyAssignRebuildsElementsThatCannotBeAssigned)
    {
        TVector<Lumina::TPair<const FString, int32>> Source;
        Source.Emplace(FString("first key that is long"), 1);
        Source.Emplace(FString("second"), 2);
        TVector<Lumina::TPair<const FString, int32>> Target;
        Target.Emplace(FString("old"), 0);
        Target = Source;
        ASSERT_EQ(Target.Num(), 2);
        EXPECT_EQ(Target[0].Key, "first key that is long");
        EXPECT_EQ(Target[1].Value, 2);
    }

    TEST(ContainerAudit, AppendingAVectorToItselfDuplicatesIt)
    {
        TVector<FString> Values;
        Values.Emplace("alpha beyond the inline buffer");
        Values.Emplace("beta");
        Values.Append(std::move(Values));
        ASSERT_EQ(Values.Num(), 4);
        EXPECT_EQ(Values[2], "alpha beyond the inline buffer");
        EXPECT_EQ(Values[3], "beta");
    }

    TEST(ContainerAudit, SortingWithNaNStaysInBounds)
    {
        for (int32 Size : { 30, 200, 5000 })
        {
            TVector<float> Values;
            uint64 State = 1234 + static_cast<uint64>(Size);
            for (int32 Index = 0; Index < Size; ++Index)
            {
                State = State * 6364136223846793005ull + 1442695040888963407ull;
                Values.Add((State >> 61) == 0 ? std::numeric_limits<float>::quiet_NaN() : static_cast<float>((State >> 40) % 1000));
            }
            Lumina::Algo::Sort(Values);
            EXPECT_EQ(Values.Num(), Size);
        }
    }

    // Many threads intern and read names at once; the table must never hand back a torn or moved entry.
    TEST(ContainerAudit, NamesInternAndResolveAcrossThreads)
    {
        constexpr int32 kThreads = 6;
        constexpr int32 kNamesPerThread = 6000;
        std::atomic<int32> Mismatches = 0;

        std::vector<std::thread> Threads;
        for (int32 Thread = 0; Thread < kThreads; ++Thread)
        {
            Threads.emplace_back([Thread, &Mismatches]
            {
                for (int32 Index = 0; Index < kNamesPerThread; ++Index)
                {
                    char Text[64];
                    const int Written = std::snprintf(Text, sizeof(Text), "AuditName.T%d.%d", Thread % 3, Index);
                    const FName Name(FStringView(Text, Written));
                    if (FStringView(*Name) != FStringView(Text, Written))
                    {
                        Mismatches.fetch_add(1);
                    }
                }
            });
        }
        for (std::thread& Thread : Threads)
        {
            Thread.join();
        }
        EXPECT_EQ(Mismatches.load(), 0);
        EXPECT_EQ(FName("AuditName.T1.5999"), FName("auditname.t1.5999"));
    }
}
