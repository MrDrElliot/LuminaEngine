#include <gtest/gtest.h>

#include <cstdio>

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
    using Lumina::FScriptCallback;
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
            Lumina::ProcessNewlyLoadedCObjects();
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
            Lumina::FInteropOpaqueStruct Opaque;
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

    // The reflection dispatcher every [ScriptFunction] goes through, which is the one path that boxes.
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
        Properties.Fields.push_back(Anchor);
        Lumina::Scripting::AppendScriptPropertiesToClass(Class, Properties);

        auto* Record = Lumina::Cast<Lumina::CScriptStruct>(Class->LayoutRecord.Get());
        ASSERT_NE(Record, nullptr);

        Lumina::Scripting::FScriptExportSchema Params;
        Lumina::Scripting::FScriptExportField In;
        In.Name = Lumina::FName("In");
        In.Type = Lumina::MakeShared<Lumina::Scripting::FScriptExportType>();
        In.Type->Kind = Lumina::EPropertyTypeFlags::Int32;
        Params.Fields.push_back(In);
        Lumina::Scripting::FScriptExportField Out;
        Out.Name = Lumina::FName("Out");
        Out.Type = Lumina::MakeShared<Lumina::Scripting::FScriptExportType>();
        Out.Type->Kind = Lumina::EPropertyTypeFlags::Int32;
        Out.Flags = (Lumina::uint32)Lumina::EPropertyFlags::OutParam;
        Params.Fields.push_back(Out);

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
        TextParams.Fields.push_back(Text);
        Lumina::Scripting::FScriptExportField Length;
        Length.Name = Lumina::FName("Length");
        Length.Type = Lumina::MakeShared<Lumina::Scripting::FScriptExportType>();
        Length.Type->Kind = Lumina::EPropertyTypeFlags::Int32;
        Length.Flags = (Lumina::uint32)Lumina::EPropertyFlags::OutParam;
        TextParams.Fields.push_back(Length);

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
        Bone.Type->NativeName = Lumina::FName("FAnimGraphBoneMaskBone");
        ViewParams.Fields.push_back(Bone);
        Lumina::Scripting::FScriptExportField Weight;
        Weight.Name = Lumina::FName("Weight");
        Weight.Type = Lumina::MakeShared<Lumina::Scripting::FScriptExportType>();
        Weight.Type->Kind = Lumina::EPropertyTypeFlags::Float;
        Weight.Flags = (Lumina::uint32)Lumina::EPropertyFlags::OutParam;
        ViewParams.Fields.push_back(Weight);

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
                    FScriptCallback Once;
                    Once.Token = Bind();
                    Scripting::InvokeScriptCallback(Once, (uint64)i);
                }
            });
            ReportRow("FScriptCallback bind, fire and free", Callback, Native);
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
        ASSERT_NE(Walk, nullptr);
        ASSERT_NE(Write, nullptr);
        ASSERT_NE(RandomGet, nullptr);

        Lumina::CWorld* World = Lumina::NewObject<Lumina::CWorld>(nullptr, Lumina::NAME_None,
            Lumina::FGuid::New(), Lumina::OF_Transient);
        ASSERT_NE(World, nullptr);

        constexpr int32 kEntities = 10000;
        constexpr int32 kTicks = 40;

        Lumina::ECS::FRegistry& Registry = Lumina::ECS::GetWorldRegistry(*World);
        Lumina::TVector<Lumina::uint32> Ids;
        Ids.reserve(kEntities);
        for (int32 Index = 0; Index < kEntities; ++Index)
        {
            const Lumina::ECS::FEntity Entity = Registry.Create();
            Registry.Emplace<Lumina::STransformComponent>(Entity);
            Ids.push_back((Lumina::uint32)Entity);
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

        int32 SeenWalk = 0;
        int32 SeenWrite = 0;
        double BestWalk = 1e30;
        double BestWrite = 1e30;
        double BestRandom = 1e30;
        double BestStep = 1e30;
        int32 SeenStep = 0;
        for (int32 Attempt = 0; Attempt < kRepeats; ++Attempt)
        {
            const double A = Walk(World, kTicks, &SeenWalk);
            const double B = Write(World, kTicks, &SeenWrite);
            const double C = RandomGet(World, kTicks, Ids.data(), kEntities);
            const double D = StepOnly != nullptr ? StepOnly(World, kTicks, &SeenStep) : -1.0;
            BestWalk = A < BestWalk ? A : BestWalk;
            BestWrite = B < BestWrite ? B : BestWrite;
            BestRandom = C < BestRandom ? C : BestRandom;
            BestStep = D >= 0.0 && D < BestStep ? D : BestStep;
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

        SUCCEED();
    }
}
