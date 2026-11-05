#include <gtest/gtest.h>

#include <cstdio>
#include <chrono>
#include <thread>

#include "Containers/Name.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "Paths/Paths.h"
#include "Platform/Time/PlatformTime.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Core/Object/Cast.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ScriptClass.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Scripting/InteropTestLibrary.h"
#include "Core/Object/ObjectArray.h"
#include "Scripting/ScriptCallback.h"
#include "Scripting/ScriptFunctionMint.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptableObject.h"
#include "World/Entity/Components/BuoyancyComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaInteropBench
{
    namespace PlatformTime = Lumina::PlatformTime;
    namespace DotNet = Lumina::DotNet;
    namespace Scripting = Lumina::Scripting;
    using Lumina::int32;
    using Lumina::uint64;
    using Lumina::FVector3;
    using Lumina::SScriptCallback;
    using Lumina::CInteropTestLibrary;

    volatile uint64 GInteropBenchSink = 0;

    constexpr int32 kIterations = 200000;
    constexpr int32 kRepeats    = 5;

    using FManagedLoopFn  = double(*)(int32);
    using FArrayLoopFn    = double(*)(int32, int32);
    using FManagedEntryFn = int32(*)(int32);
    using FBindCallbackFn = uint64(*)();
    using FPropLoopFn     = double(*)(void*, int32);
    using FMakeTargetFn   = void*(*)();
    using FFreeTargetFn   = void(*)(void*);
    using FInvokeFn       = void(*)(void*, const void*, void*);
    using FSystemWalkFn   = double(*)(void*, int32, int32*);
    using FRandomGetFn    = double(*)(void*, int32, uint32*, int32);

    // Best of several passes, since a benchmark competes with whatever else the machine is doing.
    template <typename TBody>
    double BestNanosOf(int32 Repeats, int32 Iterations, TBody&& Body)
    {
        double Best = 1e30;
        for (int32 Attempt = 0; Attempt < Repeats; ++Attempt)
        {
            const uint64 Start = PlatformTime::Cycles();
            Body();
            const double Millis = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - Start);
            const double Nanos = (Millis * 1e6) / (double)Iterations;
            Best = Nanos < Best ? Nanos : Best;
        }
        return Best;
    }

    // The loop runs managed side, because that is where a managed to native call pays its marshalling.
    double BestOfManagedLoop(const char* ExportName, int32 Iterations)
    {
        auto* Loop = (FManagedLoopFn)DotNet::ResolveManagedExport(ExportName);
        if (Loop == nullptr)
        {
            return -1.0;
        }

        double Best = 1e30;
        for (int32 Attempt = 0; Attempt < kRepeats; ++Attempt)
        {
            const double Nanos = Loop(Iterations);
            Best = Nanos < Best ? Nanos : Best;
        }
        return Best;
    }

    void ReportHeader(const char* Title)
    {
        std::printf("\n%s\n", Title);
        std::printf("  %-44s %12s %10s\n", "crossing", "ns / call", "vs floor");
        std::printf("  %-44s %12s %10s\n", "--------", "---------", "--------");
    }

    void ReportRow(const char* Name, double Nanos, double Floor)
    {
        if (Nanos < 0.0)
        {
            std::printf("  %-44s %12s %10s\n", Name, "unresolved", "-");
            return;
        }
        if (Floor > 0.0)
        {
            std::printf("  %-44s %12.2f %9.1fx\n", Name, Nanos, Nanos / Floor);
        }
        else
        {
            std::printf("  %-44s %12.2f %10s\n", Name, Nanos, "-");
        }
    }

    class FInteropBench : public ::testing::Test
    {
    public:

        static void SetUpTestSuite()
        {
            Lumina::Paths::InitializePaths();
            // Registers the reflected types, without which a property offset resolves to zero.
            Lumina::RegisterPendingTypes();
            // Read when the runtime starts, so hot loops tier up as soon as they cross the call threshold.
#if defined(_WIN32)
            _putenv_s("DOTNET_TC_CallCountingDelayMs", "0");
#else
            setenv("DOTNET_TC_CallCountingDelayMs", "0", 1);
#endif
            DotNet::Initialize();
        }

        static void TearDownTestSuite()
        {
            if (DotNet::IsInitialized())
            {
                DotNet::Shutdown();
            }
        }

        void SetUp() override
        {
            if (!DotNet::IsInitialized())
            {
                GTEST_SKIP() << "the managed host did not start in this process";
            }
        }
    };

    TEST_F(FInteropBench, ManagedToNative)
    {
        const double Floor = BestOfManagedLoop("Bench_ManagedOnly", kIterations);

        ReportHeader("managed to native, per call");
        ReportRow("managed call, no crossing (the floor)", Floor, 0.0);
        ReportRow("NativeCall int(), GC transition",
            BestOfManagedLoop("Bench_NativeCallNoArgs", kIterations), Floor);
        ReportRow("NativeCall int(), SuppressGCTransition",
            BestOfManagedLoop("Bench_NativeCallSuppressed", kIterations), Floor);
        ReportRow("NativeCall FVector3(FVector3, FVector3)",
            BestOfManagedLoop("Bench_NativeCallBlittableStruct", kIterations), Floor);
        ReportRow("NativeCall int(string), UTF-8 encode",
            BestOfManagedLoop("Bench_NativeCallStringArg", kIterations), Floor);
        ReportRow("NativeCall string(), two-pass plus alloc",
            BestOfManagedLoop("Bench_NativeCallStringReturn", kIterations), Floor);

        auto* ArrayLoop = (FArrayLoopFn)DotNet::ResolveManagedExport("Bench_NativeCallArrayReturn");
        if (ArrayLoop != nullptr)
        {
            double Best8 = 1e30;
            double Best256 = 1e30;
            for (int32 Attempt = 0; Attempt < kRepeats; ++Attempt)
            {
                const double Small = ArrayLoop(kIterations / 4, 8);
                const double Large = ArrayLoop(kIterations / 8, 256);
                Best8 = Small < Best8 ? Small : Best8;
                Best256 = Large < Best256 ? Large : Best256;
            }
            ReportRow("NativeCall T[]() out container, 8 elements", Best8, Floor);
            ReportRow("NativeCall T[]() out container, 256 elements", Best256, Floor);
        }

        SUCCEED();
    }

    TEST_F(FInteropBench, NativeCallAgainstPInvoke)
    {
        auto* Compare = (double(*)(int32, int32))DotNet::ResolveManagedExport("Bench_InteropCompare");
        ASSERT_NE(Compare, nullptr);

        auto Best = [Compare](int32 Which)
        {
            double Result = 1e30;
            for (int32 Attempt = 0; Attempt < kRepeats; ++Attempt)
            {
                const double Nanos = Compare(Which, kIterations);
                Result = Nanos < Result ? Nanos : Result;
            }
            return Result;
        };

        struct FRow { const char* Name; int32 Which; };
        const FRow Rows[] =
        {
            { "NativeCall int(), GC transition",              0 },
            { "NativeCall int(), SuppressGCTransition",       1 },
            { "DllImport int()",                              2 },
            { "LibraryImport int()",                          3 },
            { "LibraryImport int(), SuppressGCTransition",    4 },
            { "NativeCall FVector3(FVector3, FVector3)",      5 },
            { "LibraryImport FVector3(FVector3, FVector3)",   6 },
            { "NativeCall int(string), 20 chars UTF-8",       7 },
            { "LibraryImport int(string), 20 chars UTF-8",    8 },
            { "LibraryImport int(string), 20 chars UTF-16",   9 },
            { "NativeCall int(string), 400 chars UTF-8",      10 },
            { "LibraryImport int(string), 400 chars UTF-8",   11 },
            { "LibraryImport int(string), 400 chars UTF-16",  12 },
            { "NativeCall string(), two-pass FString copy",   13 },
            { "LibraryImport byte*() + PtrToStringUTF8",      14 },
            { "bare pointer int(), GC transition",            15 },
            { "bare pointer int(), SuppressGCTransition",     16 },
            { "bare pointer FVector3(FVector3, FVector3)",    17 },
        };

        const double Floor = BestOfManagedLoop("Bench_ManagedOnly", kIterations);
        ReportHeader("NativeCall against P/Invoke, same native function per pair");
        ReportRow("managed call, no crossing (the floor)", Floor, 0.0);
        for (const FRow& Row : Rows)
        {
            ReportRow(Row.Name, Best(Row.Which), Floor);
        }

        SUCCEED();
    }

    TEST_F(FInteropBench, PropertyAccess)
    {
        auto* OffsetLoop = (FPropLoopFn)DotNet::ResolveManagedExport("Bench_PropertyOffsetRead");
        auto* AccessorLoop = (FPropLoopFn)DotNet::ResolveManagedExport("Bench_PropertyAccessorRead");
        ASSERT_NE(OffsetLoop, nullptr);
        ASSERT_NE(AccessorLoop, nullptr);

        Lumina::STransformComponent Component;

        double BestOffset = 1e30;
        double BestAccessor = 1e30;
        for (int32 Attempt = 0; Attempt < kRepeats; ++Attempt)
        {
            const double Offset = OffsetLoop(&Component, kIterations);
            const double Accessor = AccessorLoop(&Component, kIterations);
            BestOffset = Offset < BestOffset ? Offset : BestOffset;
            BestAccessor = Accessor < BestAccessor ? Accessor : BestAccessor;
        }

        auto* MapLookup = (FPropLoopFn)DotNet::ResolveManagedExport("Bench_BlittableMapLookup");
        double BestMap = -1.0;
        if (MapLookup != nullptr)
        {
            Lumina::SInteropOpaqueStruct Opaque;
            BestMap = 1e30;
            for (int32 Attempt = 0; Attempt < kRepeats; ++Attempt)
            {
                const double Nanos = MapLookup(&Opaque, kIterations);
                BestMap = Nanos < BestMap ? Nanos : BestMap;
            }
        }

        ReportHeader("component property read, per read");
        ReportRow("offset load, no crossing", BestOffset, 0.0);
        ReportRow("reflected accessor, SuppressGCTransition", BestAccessor, BestOffset);
        ReportRow("blittable map lookup, through the ops table", BestMap, BestOffset);

        // A CObject wrapper validates liveness on every access, which a component view does not.
        auto* HandleLoop = (FPropLoopFn)DotNet::ResolveManagedExport("Bench_CObjectHandleResolve");
        if (HandleLoop != nullptr)
        {
            Lumina::CObject* Default = Lumina::CInteropTestLibrary::StaticClass()->GetDefaultObject();
            if (Default != nullptr)
            {
                double BestHandle = 1e30;
                for (int32 Attempt = 0; Attempt < kRepeats; ++Attempt)
                {
                    const double Nanos = HandleLoop(Default, kIterations);
                    BestHandle = Nanos < BestHandle ? Nanos : BestHandle;
                }
                ReportRow("CObject Handle resolve, per access", BestHandle, BestOffset);
            }
        }

        SUCCEED();
    }

    // The reflection dispatcher every [Function] goes through, which is the one path that boxes.
    TEST_F(FInteropBench, ScriptFunctionDispatch)
    {
        auto* MakeTarget = (FMakeTargetFn)DotNet::ResolveManagedExport("Test_MakeMarshalTarget");
        auto* FreeTarget = (FFreeTargetFn)DotNet::ResolveManagedExport("Test_FreeMarshalTarget");
        auto* Invoke = (FInvokeFn)DotNet::ResolveManagedExport("InvokeScriptFunction");
        ASSERT_NE(MakeTarget, nullptr);
        ASSERT_NE(FreeTarget, nullptr);
        ASSERT_NE(Invoke, nullptr);

        Lumina::CScriptClass* Class = Lumina::FScriptableRegistry::Mint("InteropBench_Dispatch", "CScriptableTest");
        ASSERT_NE(Class, nullptr);

        Lumina::Scripting::FScriptExportSchema Properties;
        Lumina::Scripting::FScriptExportField Anchor;
        Anchor.Name = Lumina::FName("Anchor");
        Anchor.Type = Lumina::MakeShared<Lumina::Scripting::FScriptExportType>();
        Anchor.Type->Kind = Lumina::EPropertyTypeFlags::Int32;
        Properties.Fields.Add(Anchor);
        Lumina::Scripting::AppendScriptPropertiesToClass(Class, Properties);

        auto* Record = Lumina::Cast<Lumina::CScriptStruct>(Class->LayoutRecord.Get());
        ASSERT_NE(Record, nullptr);

        Lumina::Scripting::FScriptExportSchema Params;
        Lumina::Scripting::FScriptExportField In;
        In.Name = Lumina::FName("In");
        In.Type = Lumina::MakeShared<Lumina::Scripting::FScriptExportType>();
        In.Type->Kind = Lumina::EPropertyTypeFlags::Int32;
        Params.Fields.Add(In);
        Lumina::Scripting::FScriptExportField Out;
        Out.Name = Lumina::FName("Out");
        Out.Type = Lumina::MakeShared<Lumina::Scripting::FScriptExportType>();
        Out.Type->Kind = Lumina::EPropertyTypeFlags::Int32;
        Out.Flags = (Lumina::uint32)Lumina::EPropertyFlags::OutParam;
        Params.Fields.Add(Out);

        Lumina::FFunction* Function = Lumina::Scripting::MintScriptFunction(*Class, *Record,
            Lumina::FName("MarshalOut"), Params, -1, nullptr);
        ASSERT_NE(Function, nullptr);
        Class->GetDefaultObject();

        void* Target = MakeTarget();
        ASSERT_NE(Target, nullptr);

        const int32 DispatchIterations = kIterations / 20;
        const double Dispatch = BestNanosOf(kRepeats, DispatchIterations, [&]
        {
            for (int32 i = 0; i < DispatchIterations; ++i)
            {
                Lumina::FFunctionFrame Frame(*Function);
                *Function->GetParams()[0]->GetValuePtr<int32>(Frame.GetMemory()) = i;
                Invoke(Target, Function, Frame.GetMemory());
            }
        });

        // The same loop without the crossing, so the frame setup is not read as dispatch cost.
        const double FrameOnly = BestNanosOf(kRepeats, DispatchIterations, [&]
        {
            for (int32 i = 0; i < DispatchIterations; ++i)
            {
                Lumina::FFunctionFrame Frame(*Function);
                *Function->GetParams()[0]->GetValuePtr<int32>(Frame.GetMemory()) = i;
                GInteropBenchSink += (uint64)(uintptr_t)Frame.GetMemory();
            }
        });

        // What ScriptFunctionThunk does once the binder has published, which is the production path now.
        double DirectOnly = -1.0;
        if (void* DirectInvoker = Function->GetManagedInvoker())
        {
            const Lumina::int32* PublishedOffsets = Function->GetManagedOffsets();
            using FDirect = void (*)(void*, void*, const Lumina::int32*);
            auto* Direct = reinterpret_cast<FDirect>(DirectInvoker);

            const double WithDirect = BestNanosOf(kRepeats, DispatchIterations, [&]
            {
                for (int32 i = 0; i < DispatchIterations; ++i)
                {
                    Lumina::FFunctionFrame Frame(*Function);
                    *Function->GetParams()[0]->GetValuePtr<int32>(Frame.GetMemory()) = i;
                    Direct(Target, Frame.GetMemory(), PublishedOffsets);
                }
            });
            DirectOnly = WithDirect - FrameOnly;
        }

        // A string argument, which had no generated invoker until the marshaller covered every slot kind.
        Lumina::Scripting::FScriptExportSchema TextParams;
        Lumina::Scripting::FScriptExportField Text;
        Text.Name = Lumina::FName("In");
        Text.Type = Lumina::MakeShared<Lumina::Scripting::FScriptExportType>();
        Text.Type->Kind = Lumina::EPropertyTypeFlags::String;
        TextParams.Fields.Add(Text);
        Lumina::Scripting::FScriptExportField Length;
        Length.Name = Lumina::FName("Length");
        Length.Type = Lumina::MakeShared<Lumina::Scripting::FScriptExportType>();
        Length.Type->Kind = Lumina::EPropertyTypeFlags::Int32;
        Length.Flags = (Lumina::uint32)Lumina::EPropertyFlags::OutParam;
        TextParams.Fields.Add(Length);

        Lumina::FFunction* TextFunction = Lumina::Scripting::MintScriptFunction(*Class, *Record,
            Lumina::FName("MarshalText"), TextParams, -1, nullptr);
        ASSERT_NE(TextFunction, nullptr);

        const double TextDispatch = BestNanosOf(kRepeats, DispatchIterations, [&]
        {
            for (int32 i = 0; i < DispatchIterations; ++i)
            {
                Lumina::FFunctionFrame Frame(*TextFunction);
                *TextFunction->GetParams()[0]->GetValuePtr<Lumina::FString>(Frame.GetMemory()) = "abcdefg";
                Invoke(Target, TextFunction, Frame.GetMemory());
            }
        });

        const double TextFrameOnly = BestNanosOf(kRepeats, DispatchIterations, [&]
        {
            for (int32 i = 0; i < DispatchIterations; ++i)
            {
                Lumina::FFunctionFrame Frame(*TextFunction);
                *TextFunction->GetParams()[0]->GetValuePtr<Lumina::FString>(Frame.GetMemory()) = "abcdefg";
                GInteropBenchSink += (uint64)(uintptr_t)Frame.GetMemory();
            }
        });

        // An opaque struct stays on the reflection path, so this is what that path costs per call.
        Lumina::Scripting::FScriptExportSchema ViewParams;
        Lumina::Scripting::FScriptExportField Bone;
        Bone.Name = Lumina::FName("Bone");
        Bone.Type = Lumina::MakeShared<Lumina::Scripting::FScriptExportType>();
        Bone.Type->Kind = Lumina::EPropertyTypeFlags::Struct;
        Bone.Type->NativeName = Lumina::FName("SAnimGraphBoneMaskBone");
        ViewParams.Fields.Add(Bone);
        Lumina::Scripting::FScriptExportField Weight;
        Weight.Name = Lumina::FName("Weight");
        Weight.Type = Lumina::MakeShared<Lumina::Scripting::FScriptExportType>();
        Weight.Type->Kind = Lumina::EPropertyTypeFlags::Float;
        Weight.Flags = (Lumina::uint32)Lumina::EPropertyFlags::OutParam;
        ViewParams.Fields.Add(Weight);

        Lumina::FFunction* ViewFunction = Lumina::Scripting::MintScriptFunction(*Class, *Record,
            Lumina::FName("MarshalStructView"), ViewParams, -1, nullptr);
        ASSERT_NE(ViewFunction, nullptr);

        const double ViewDispatch = BestNanosOf(kRepeats, DispatchIterations, [&]
        {
            for (int32 i = 0; i < DispatchIterations; ++i)
            {
                Lumina::FFunctionFrame Frame(*ViewFunction);
                Invoke(Target, ViewFunction, Frame.GetMemory());
            }
        });

        const double ViewFrameOnly = BestNanosOf(kRepeats, DispatchIterations, [&]
        {
            for (int32 i = 0; i < DispatchIterations; ++i)
            {
                Lumina::FFunctionFrame Frame(*ViewFunction);
                GInteropBenchSink += (uint64)(uintptr_t)Frame.GetMemory();
            }
        });

        ReportHeader("script function dispatch, per call");
        ReportRow("native frame setup alone", FrameOnly, 0.0);
        ReportRow("plus InvokeScriptFunction, 2 args", Dispatch, FrameOnly);
        ReportRow("the managed dispatch itself", Dispatch - FrameOnly, 0.0);
        ReportRow("direct to the published invoker", DirectOnly, 0.0);
        ReportRow("the managed dispatch, a string arg", TextDispatch - TextFrameOnly, 0.0);
        ReportRow("the managed dispatch, a struct view", ViewDispatch - ViewFrameOnly, 0.0);

        FreeTarget(Target);

        SUCCEED();
    }

    TEST_F(FInteropBench, NativeToManaged)
    {
        auto* Entry = (FManagedEntryFn)DotNet::ResolveManagedExport("Bench_ManagedEntryPoint");
        ASSERT_NE(Entry, nullptr);

        int32 Accumulated = 0;
        const double Managed = BestNanosOf(kRepeats, kIterations, [&]
        {
            for (int32 i = 0; i < kIterations; ++i)
            {
                Accumulated = Entry(Accumulated);
            }
        });
        GInteropBenchSink = (uint64)Accumulated;

        // The same loop against a native static, to separate the crossing from the call itself.
        int32 NativeTotal = 0;
        const double Native = BestNanosOf(kRepeats, kIterations, [&]
        {
            for (int32 i = 0; i < kIterations; ++i)
            {
                NativeTotal += CInteropTestLibrary::BenchNoOp();
            }
        });
        GInteropBenchSink += (uint64)NativeTotal;

        ReportHeader("native to managed, per call");
        ReportRow("native call, no crossing (the floor)", Native, 0.0);
        ReportRow("ManagedExport int(int)", Managed, Native);

        // A one-shot callback allocates a GCHandle per bind, so bind and fire are priced together.
        auto* Bind = (FBindCallbackFn)DotNet::ResolveManagedExport("Test_BindScriptCallback");
        if (Bind != nullptr)
        {
            const int32 CallbackIterations = kIterations / 20;
            const double Callback = BestNanosOf(kRepeats, CallbackIterations, [&]
            {
                for (int32 i = 0; i < CallbackIterations; ++i)
                {
                    SScriptCallback Once;
                    Once.Token = Bind();
                    Scripting::InvokeScriptCallback(Once, (uint64)i);
                }
            });
            ReportRow("SScriptCallback bind, fire and free", Callback, Native);
        }

        SUCCEED();
    }

    // What each property shape costs a script to touch on a native component, against the same edit in C++.
    TEST_F(FInteropBench, ComponentPropertyOps)
    {
        using FPropertyOpFn = double(*)(void*, int32, int32, Lumina::int64*);
        auto* PropertyOp = (FPropertyOpFn)DotNet::ResolveManagedExport("Bench_PropertyOp");
        ASSERT_NE(PropertyOp, nullptr);
        Lumina::RegisterPendingTypes();

        constexpr int32 kSlots = 256;
        constexpr int32 kOps = 196608;
        constexpr int32 kKinds = 14;
        Lumina::SInteropBenchComponent Component;
        Component.Floats.SetNum(kSlots, 0.0f);
        Component.Points.SetNum(kSlots);
        for (int32 Slot = 0; Slot < kSlots; ++Slot)
        {
            Component.Names.Emplace("a name of moderate length");
            Component.Scores[Slot] = (float)Slot;
        }
        Component.Label = "a label of moderate length";

        const char* Names[kKinds] = {
            "TVector<float> Add", "TVector<float> [i] += 1", "TVector<float> Count", "TVector<FVector3> [i].X += 1",
            "TVector<FString> Get(i) to string", "TVector<FString> Set(i, literal)", "FString read to string", "FString write literal",
            "FName assign from string", "FName compare to cached", "THashMap<int, float> [key]", "THashMap<int, float> Set",
            "nested struct field += 1", "TVector<float> AsSpan, per element" };

        const Lumina::FName CachedTag("BenchTag");
        float NativeTotal = 0.0f;
        double NativeNanos[kKinds] = {};
        NativeNanos[0] = BestNanosOf(kRepeats, kOps, [&] { for (int32 i = 0; i < kOps; ++i) { if ((i & (kSlots - 1)) == 0) { Component.Floats.Reset(); } Component.Floats.Add(1.0f); } });
        Component.Floats.SetNum(kSlots, 0.0f);
        NativeNanos[1] = BestNanosOf(kRepeats, kOps, [&] { for (int32 i = 0; i < kOps; ++i) { Component.Floats[i & (kSlots - 1)] += 1.0f; } });
        NativeNanos[2] = BestNanosOf(kRepeats, kOps, [&] { for (int32 i = 0; i < kOps; ++i) { NativeTotal += (float)Component.Floats.Num(); } });
        NativeNanos[3] = BestNanosOf(kRepeats, kOps, [&] { for (int32 i = 0; i < kOps; ++i) { Component.Points[i & (kSlots - 1)].x += 1.0f; } });
        NativeNanos[4] = BestNanosOf(kRepeats, kOps, [&] { for (int32 i = 0; i < kOps; ++i) { Lumina::FString Copy = Component.Names[i & (kSlots - 1)]; NativeTotal += (float)Copy.Len(); } });
        NativeNanos[5] = BestNanosOf(kRepeats, kOps, [&] { for (int32 i = 0; i < kOps; ++i) { Component.Names[i & (kSlots - 1)] = "abc"; } });
        NativeNanos[6] = BestNanosOf(kRepeats, kOps, [&] { for (int32 i = 0; i < kOps; ++i) { Lumina::FString Copy = Component.Label; NativeTotal += (float)Copy.Len(); } });
        NativeNanos[7] = BestNanosOf(kRepeats, kOps, [&] { for (int32 i = 0; i < kOps; ++i) { Component.Label = "hello"; } });
        NativeNanos[8] = BestNanosOf(kRepeats, kOps, [&] { for (int32 i = 0; i < kOps; ++i) { Component.Tag = Lumina::FName("BenchTag"); } });
        NativeNanos[9] = BestNanosOf(kRepeats, kOps, [&] { for (int32 i = 0; i < kOps; ++i) { NativeTotal += Component.Tag == CachedTag ? 1.0f : 0.0f; } });
        NativeNanos[10] = BestNanosOf(kRepeats, kOps, [&] { for (int32 i = 0; i < kOps; ++i) { NativeTotal += Component.Scores[i & (kSlots - 1)]; } });
        NativeNanos[11] = BestNanosOf(kRepeats, kOps, [&] { for (int32 i = 0; i < kOps; ++i) { Component.Scores[i & (kSlots - 1)] = 1.0f; } });
        NativeNanos[12] = BestNanosOf(kRepeats, kOps, [&] { for (int32 i = 0; i < kOps; ++i) { Component.Inner.Value += 1.0f; } });
        NativeNanos[13] = BestNanosOf(kRepeats, kOps / kSlots, [&] { for (int32 i = 0; i < kOps / kSlots; ++i) { for (float& Value : Component.Floats) { Value += 1.0f; } } }) / kSlots;
        GInteropBenchSink += (uint64)NativeTotal;

        // Past the call threshold for every loop, then a pause so the background recompile lands before timing.
        auto WarmUp = [&](auto&& Call, int32 Kinds)
        {
            for (int32 Round = 0; Round < 40; ++Round)
            {
                for (int32 Which = 0; Which < Kinds; ++Which)
                {
                    Call(Which);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
        };
        WarmUp([&](int32 Which) { Lumina::int64 Ignored = 0; PropertyOp(&Component, Which, kSlots, &Ignored); }, kKinds);

        std::printf("\ncomponent property operations from C#, per operation\n");
        std::printf("  %-40s %9s %9s %7s %10s\n", "operation", "C++ ns", "C# ns", "ratio", "C# B/op");
        for (int32 Which = 0; Which < kKinds; ++Which)
        {
            const int32 Iterations = Which == 13 ? kOps / kSlots : kOps;
            double Best = 1e30;
            Lumina::int64 Bytes = 0;
            for (int32 Attempt = 0; Attempt < kRepeats; ++Attempt)
            {
                Lumina::int64 AttemptBytes = 0;
                const double Value = PropertyOp(&Component, Which, Iterations, &AttemptBytes);
                Best = Value < Best ? Value : Best;
                Bytes = AttemptBytes;
            }
            if (Which == 13)
            {
                Best /= kSlots;
            }
            const double Native = NativeNanos[Which];
            std::printf("  %-40s %9.2f %9.2f %6.1fx %10.1f\n", Names[Which], Native, Best, Native > 0.0 ? Best / Native : 0.0, (double)Bytes / (double)Iterations);
        }

        using FStringPiecesFn = double(*)(void*, int32, int32);
        if (auto* Pieces = (FStringPiecesFn)DotNet::ResolveManagedExport("Bench_StringPieces"))
        {
            const char* PieceNames[] = { "Native.StringAssign call", "UTF-8 encode into scratch", "ReadString", "UTF8.GetString 26 bytes", "Label property write", "Label property read" };
            WarmUp([&](int32 Which) { Pieces(&Component, Which, 64); }, 6);
            for (int32 Which = 0; Which < 6; ++Which)
            {
                double Best = 1e30;
                for (int32 Attempt = 0; Attempt < kRepeats; ++Attempt)
                {
                    const double Value = Pieces(&Component, Which, kOps);
                    Best = Value < Best ? Value : Best;
                }
                std::printf("  piece %-34s %9.2f\n", PieceNames[Which], Best);
            }
        }
        SUCCEED();
    }

    // What a CEntitySystem actually costs per entity, which is the question a gameplay author is asking.
    TEST_F(FInteropBench, EntitySystemWorkload)
    {
        auto* Walk = (FSystemWalkFn)DotNet::ResolveManagedExport("Bench_SystemViewWalk");
        auto* Write = (FSystemWalkFn)DotNet::ResolveManagedExport("Bench_SystemViewWrite");
        auto* StepOnly = (FSystemWalkFn)DotNet::ResolveManagedExport("Bench_SystemViewStepOnly");
        auto* RandomGet = (FRandomGetFn)DotNet::ResolveManagedExport("Bench_SystemRandomGet");
        auto* WalkTwo = (FSystemWalkFn)DotNet::ResolveManagedExport("Bench_SystemViewWalkTwo");
        auto* LookupGet = (FRandomGetFn)DotNet::ResolveManagedExport("Bench_SystemLookupGet");
        ASSERT_NE(WalkTwo, nullptr);
        ASSERT_NE(LookupGet, nullptr);
        ASSERT_NE(Walk, nullptr);
        ASSERT_NE(Write, nullptr);
        ASSERT_NE(RandomGet, nullptr);

        Lumina::CWorld* World = Lumina::NewObject<Lumina::CWorld>(nullptr, Lumina::NAME_None,
            Lumina::FGuid::New(), Lumina::OF_Transient);
        ASSERT_NE(World, nullptr);

        constexpr int32 kEntities = 10000;
        constexpr int32 kTicks = 40;

        Lumina::ECS::FRegistry& Registry = World->GetEntityRegistry();
        Lumina::TVector<Lumina::uint32> Ids;
        Ids.Reserve(kEntities);
        for (int32 Index = 0; Index < kEntities; ++Index)
        {
            const Lumina::ECS::FEntity Entity = Registry.Create();
            Registry.Emplace<Lumina::STransformComponent>(Entity);
            Registry.Emplace<Lumina::SBuoyancyComponent>(Entity);
            Ids.Add((Lumina::uint32)Entity);
        }

        // The same walk in C++, which is what a system written natively would cost.
        float NativeTotal = 0.0f;
        const double NativeWalk = BestNanosOf(kRepeats, kTicks * kEntities, [&]
        {
            for (int32 Tick = 0; Tick < kTicks; ++Tick)
            {
                auto View = Registry.View<Lumina::STransformComponent>();
                for (Lumina::ECS::FEntity Entity : View)
                {
                    // The whole transform, so this reads the same 48 bytes the managed walk does.
                    const Lumina::FTransform Local =
                        View.Get<Lumina::STransformComponent>(Entity).LocalTransform;
                    NativeTotal += Local.GetLocation().x;
                }
            }
        });
        GInteropBenchSink += (uint64)NativeTotal;

        const double NativeWalkTwo = BestNanosOf(kRepeats, kTicks * kEntities, [&]
        {
            for (int32 Tick = 0; Tick < kTicks; ++Tick)
            {
                Registry.View<Lumina::STransformComponent, Lumina::SBuoyancyComponent>().ForEach(
                    [&](Lumina::ECS::FEntity, Lumina::STransformComponent& Transform, Lumina::SBuoyancyComponent& Buoyancy)
                {
                    NativeTotal += Transform.LocalTransform.GetLocation().x * Buoyancy.Buoyancy;
                });
            }
        });

        const double NativeLookup = BestNanosOf(kRepeats, kTicks * kEntities, [&]
        {
            for (int32 Tick = 0; Tick < kTicks; ++Tick)
            {
                for (Lumina::uint32 Id : Ids)
                {
                    NativeTotal += Registry.Get<Lumina::STransformComponent>(Lumina::ECS::FEntity(Id)).LocalTransform.GetLocation().x;
                }
            }
        });
        GInteropBenchSink += (uint64)NativeTotal;

        int32 SeenWalk = 0;
        int32 SeenWrite = 0;
        double BestWalk = 1e30;
        double BestWrite = 1e30;
        double BestRandom = 1e30;
        double BestStep = 1e30;
        double BestWalkTwo = 1e30;
        double BestLookup = 1e30;
        int32 SeenStep = 0;
        int32 SeenWalkTwo = 0;
        for (int32 Attempt = 0; Attempt < kRepeats; ++Attempt)
        {
            const double A = Walk(World, kTicks, &SeenWalk);
            const double B = Write(World, kTicks, &SeenWrite);
            const double C = RandomGet(World, kTicks, Ids.GetData(), kEntities);
            const double D = StepOnly != nullptr ? StepOnly(World, kTicks, &SeenStep) : -1.0;
            BestWalk = A < BestWalk ? A : BestWalk;
            BestWrite = B < BestWrite ? B : BestWrite;
            BestRandom = C < BestRandom ? C : BestRandom;
            BestStep = D >= 0.0 && D < BestStep ? D : BestStep;
            const double E = WalkTwo(World, kTicks, &SeenWalkTwo);
            const double F = LookupGet(World, kTicks, Ids.GetData(), kEntities);
            BestWalkTwo = E < BestWalkTwo ? E : BestWalkTwo;
            BestLookup = F < BestLookup ? F : BestLookup;
        }

        EXPECT_EQ(SeenWalk, kEntities) << "the managed view did not see every entity";
        EXPECT_EQ(SeenWrite, kEntities);

        std::printf("\nentity system workload, %d entities x %d ticks, per entity\n", kEntities, kTicks);
        std::printf("  %-44s %12s %10s\n", "operation", "ns / entity", "vs C++");
        std::printf("  %-44s %12s %10s\n", "---------", "-----------", "------");
        ReportRow("C++ view walk, read a transform", NativeWalk, 0.0);
        ReportRow("C# view step only, touching nothing", BestStep, NativeWalk);
        ReportRow("C# view walk, read a transform", BestWalk, NativeWalk);
        ReportRow("C# view walk, read and write it back", BestWrite, NativeWalk);
        ReportRow("C# TryGet per entity, read a transform", BestRandom, NativeWalk);
        ReportRow("C++ view walk, two components", NativeWalkTwo, 0.0);
        ReportRow("C# view walk, two components", BestWalkTwo, NativeWalkTwo);
        ReportRow("C++ Get per entity, read a transform", NativeLookup, 0.0);
        ReportRow("C# GetComponent per entity, read a transform", BestLookup, NativeLookup);

        using FSmallGetFn = double(*)(void*, int32, int32, uint32*, int32);
        using FKeptRefFn = double(*)(void*, int32, uint32*, int32);
        auto* SmallGet = (FSmallGetFn)DotNet::ResolveManagedExport("Bench_SmallGet");
        auto* KeptRef = (FKeptRefFn)DotNet::ResolveManagedExport("Bench_KeptRefGet");
        ASSERT_NE(SmallGet, nullptr);
        ASSERT_NE(KeptRef, nullptr);

        const double NativeSmallGet = BestNanosOf(kRepeats, kTicks * kEntities, [&]
        {
            for (int32 Tick = 0; Tick < kTicks; ++Tick)
            {
                for (Lumina::uint32 Id : Ids)
                {
                    NativeTotal += Registry.Get<Lumina::SBuoyancyComponent>(Lumina::ECS::FEntity(Id)).Buoyancy;
                }
            }
        });
        const double NativeSmallTryGet = BestNanosOf(kRepeats, kTicks * kEntities, [&]
        {
            for (int32 Tick = 0; Tick < kTicks; ++Tick)
            {
                for (Lumina::uint32 Id : Ids)
                {
                    if (const Lumina::SBuoyancyComponent* Found = Registry.TryGet<Lumina::SBuoyancyComponent>(Lumina::ECS::FEntity(Id)))
                    {
                        NativeTotal += Found->Buoyancy;
                    }
                }
            }
        });
        GInteropBenchSink += (uint64)NativeTotal;

        double BestSmall[4] = { 1e30, 1e30, 1e30, 1e30 };
        double BestKept = 1e30;
        for (int32 Attempt = 0; Attempt < kRepeats; ++Attempt)
        {
            for (int32 Which = 0; Which < 4; ++Which)
            {
                const double Value = SmallGet(World, Which, kTicks, Ids.GetData(), kEntities);
                BestSmall[Which] = Value < BestSmall[Which] ? Value : BestSmall[Which];
            }
            const double Value = KeptRef(World, kTicks, Ids.GetData(), kEntities);
            BestKept = Value < BestKept ? Value : BestKept;
        }

        std::printf("\n  small component gets, 12 bytes, per entity\n");
        ReportRow("C++ Get", NativeSmallGet, 0.0);
        ReportRow("C++ TryGet", NativeSmallTryGet, 0.0);
        ReportRow("C# GetComponent", BestSmall[0], NativeSmallGet);
        ReportRow("C# GetComponent, three fields", BestSmall[3], NativeSmallGet);
        ReportRow("C# TryGetComponent then Get", BestSmall[1], NativeSmallTryGet);
        ReportRow("C# HasComponent", BestSmall[2], NativeSmallTryGet);
        ReportRow("C# kept TComponentRef Get", BestKept, NativeSmallGet);

        using FGatherFn = double(*)(void*, int32, int32);
        if (auto* Gather = (FGatherFn)DotNet::ResolveManagedExport("Bench_ViewGatherOnly"))
        {
            double BestBegin = 1e30;
            double BestGather = 1e30;
            for (int32 Attempt = 0; Attempt < kRepeats; ++Attempt)
            {
                const double A = Gather(World, kTicks, 1);
                const double B = Gather(World, kTicks, 0);
                BestBegin = A < BestBegin ? A : BestBegin;
                BestGather = B < BestGather ? B : BestGather;
            }
            ReportRow("C# view native snapshot only", BestBegin, NativeWalk);
            ReportRow("C# view native snapshot and chunk gather", BestGather, NativeWalk);
        }

        SUCCEED();
    }
}
