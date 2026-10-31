#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "Containers/ContainerOps.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"
#include "Core/Object/InstancedStruct.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Scripting/ScriptStruct.h"
#include "Scripting/ScriptTypeRegistry.h"
#include "Scripting/ScriptableObject.h"
#include "Scripting/ScriptableTest.h"
#include "World/ECS/ComponentType.h"
#include "World/ECS/Registry.h"
#include "World/ECS/Signal.h"
#include "ScriptTestUtil.h"

using namespace Lumina;

namespace ContainerSafetyTestsInternal
{
    TSharedPtr<Scripting::FScriptExportType> MakeType(EPropertyTypeFlags Kind)
    {
        TSharedPtr<Scripting::FScriptExportType> Type = MakeShared<Scripting::FScriptExportType>();
        Type->Kind = Kind;
        return Type;
    }

    FArrayProperty* MintList(const char* ClassName, TSharedPtr<Scripting::FScriptExportType> Element, CObject*& OutObject)
    {
        TSharedPtr<Scripting::FScriptExportType> List = MakeType(EPropertyTypeFlags::Vector);
        List->ElementType = std::move(Element);

        Scripting::FScriptExportField Field;
        Field.Name = FName("Items");
        Field.Type = std::move(List);

        Scripting::FScriptExportSchema Schema;
        Schema.Fields.push_back(std::move(Field));

        CScriptClass* Class = ScriptTest::MintWith(ClassName, Schema);
        if (Class == nullptr)
        {
            return nullptr;
        }

        OutObject = NewObject(Class, nullptr, NAME_None, FGuid::New(), OF_Transient);
        return static_cast<FArrayProperty*>(Class->GetProperty(FName("Items")));
    }

    struct FSelfRemover
    {
        ECS::FComponentSignal*  Signal = nullptr;
        int32                   Calls = 0;
    };

    void RemoveSelf(void* Payload, ECS::FRegistry&, ECS::FEntity)
    {
        FSelfRemover* Self = static_cast<FSelfRemover*>(Payload);
        ++Self->Calls;
        Self->Signal->Disconnect(&RemoveSelf, Self);
    }

    void CountCall(void* Payload, ECS::FRegistry&, ECS::FEntity)
    {
        ++*static_cast<int32*>(Payload);
    }

    struct FGrower
    {
        ECS::FComponentSignal*  Signal = nullptr;
        int32*                  Counter = nullptr;
        bool                    bGrown = false;
    };

    void ConnectMany(void* Payload, ECS::FRegistry&, ECS::FEntity)
    {
        FGrower* Self = static_cast<FGrower*>(Payload);
        if (Self->bGrown)
        {
            return;
        }
        Self->bGrown = true;
        for (int32 Index = 0; Index < 64; ++Index)
        {
            (void)Self->Signal->Connect(&CountCall, Self->Counter);
        }
    }
}

TEST(ScriptContainerGrowth, InlineInstancedStructsSurviveAReallocation)
{
    using namespace ContainerSafetyTestsInternal;

    Scripting::FScriptExportSchema PayloadSchema;
    PayloadSchema.Fields.push_back(ScriptTest::ScalarField("Value", EPropertyTypeFlags::Int32));
    ScriptTest::PublishStructs({ ScriptTest::StructDefinition("GrowTest_Payload", std::move(PayloadSchema)) });

    CStruct* Payload = FScriptStructRegistry::Get().Find(FName("GrowTest_Payload"));
    ASSERT_NE(Payload, nullptr);
    ASSERT_LE(Payload->GetAlignedSize(), FInstancedStruct::kInlineSize) << "the payload has to sit inline for this to test anything";

    FProperty* ValueProperty = Payload->GetProperty(FName("Value"));
    ASSERT_NE(ValueProperty, nullptr);

    TSharedPtr<Scripting::FScriptExportType> Element = MakeType(EPropertyTypeFlags::InstancedStruct);
    Element->BaseName = FName("GrowTest_Payload");

    CObject* Object = nullptr;
    FArrayProperty* Items = MintList("ScriptGrowth_Instanced", Element, Object);
    ASSERT_NE(Items, nullptr);
    ASSERT_NE(Object, nullptr);

    const FVectorOps* Ops = Items->GetOps();
    void* Array = Items->GetValuePtr<void>(Object);
    ASSERT_EQ(Ops->ElementSize, sizeof(FInstancedStruct));

    constexpr int32 Count = 40;
    for (int32 Index = 0; Index < Count; ++Index)
    {
        FInstancedStruct Source(Payload);
        *ValueProperty->GetValuePtr<int32>(Source.GetMutableMemory()) = Index;
        Ops->PushBack(Array, &Source);
    }
    ASSERT_EQ(Ops->Size(Array), (SIZE_T)Count);

    const FInstancedStruct* Elements = static_cast<const FInstancedStruct*>(Ops->Data(Array));
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const uint8* Begin = reinterpret_cast<const uint8*>(&Elements[Index]);
        const uint8* Memory = static_cast<const uint8*>(Elements[Index].GetMemory());
        EXPECT_TRUE(Memory >= Begin && Memory < Begin + sizeof(FInstancedStruct)) << "element " << Index << " points outside itself after the list grew";
        EXPECT_EQ(*ValueProperty->GetValuePtr<int32>(const_cast<uint8*>(Memory)), Index);
    }

    Ops->Clear(Array);
}

TEST(ScriptContainerGrowth, PushingAnElementOfTheSameListCopiesItBeforeTheOldBlockGoes)
{
    using namespace ContainerSafetyTestsInternal;

    CObject* Object = nullptr;
    FArrayProperty* Items = MintList("ScriptGrowth_Strings", MakeType(EPropertyTypeFlags::String), Object);
    ASSERT_NE(Items, nullptr);
    ASSERT_NE(Object, nullptr);

    const FVectorOps* Ops = Items->GetOps();
    void* Array = Items->GetValuePtr<void>(Object);

    const FString Text = "long enough that the characters live on the heap rather than inline";
    Ops->PushBack(Array, &Text);
    for (int32 Index = 0; Index < 40; ++Index)
    {
        Ops->PushBack(Array, Ops->Data(Array));
    }

    const FString* Strings = static_cast<const FString*>(Ops->Data(Array));
    for (SIZE_T Index = 0; Index < Ops->Size(Array); ++Index)
    {
        EXPECT_EQ(Strings[Index], Text) << "element " << Index;
    }

    Ops->Clear(Array);
}

TEST(ComponentSignalReentrancy, AListenerThatDisconnectsItselfDoesNotSkipTheNext)
{
    using namespace ContainerSafetyTestsInternal;

    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();

    ECS::FComponentSignal Signal;
    FSelfRemover Remover{ &Signal };
    int32 Second = 0;
    int32 Third = 0;
    (void)Signal.Connect(&RemoveSelf, &Remover);
    (void)Signal.Connect(&CountCall, &Second);
    (void)Signal.Connect(&CountCall, &Third);

    Signal.Broadcast(Registry, Entity);
    EXPECT_EQ(Remover.Calls, 1);
    EXPECT_EQ(Second, 1);
    EXPECT_EQ(Third, 1);

    Signal.Broadcast(Registry, Entity);
    EXPECT_EQ(Remover.Calls, 1);
    EXPECT_EQ(Second, 2);
    EXPECT_EQ(Third, 2);
}

TEST(ComponentSignalReentrancy, AListenerThatConnectsMoreDoesNotWalkAFreedList)
{
    using namespace ContainerSafetyTestsInternal;

    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();

    ECS::FComponentSignal Signal;
    int32 Added = 0;
    int32 Original = 0;
    FGrower Grower{ &Signal, &Added };
    (void)Signal.Connect(&ConnectMany, &Grower);
    (void)Signal.Connect(&CountCall, &Original);

    Signal.Broadcast(Registry, Entity);
    EXPECT_EQ(Original, 1);

    Added = 0;
    Signal.Broadcast(Registry, Entity);
    EXPECT_EQ(Original, 2);
    EXPECT_EQ(Added, 64);
}

TEST(ComponentTypeRegistryThreads, TypesFirstTouchedTogetherGetDistinctIds)
{
    constexpr int32 Rounds = 100;
    constexpr int32 ThreadCount = 8;

    for (int32 Round = 0; Round < Rounds; ++Round)
    {
        FName Names[ThreadCount];
        for (int32 Index = 0; Index < ThreadCount; ++Index)
        {
            Names[Index] = FName(Format("ThreadedAcquire_{}_{}", Round, Index).c_str());
        }

        ECS::FComponentTypeID Ids[ThreadCount] = {};
        std::atomic<int32> Ready{ 0 };
        std::thread Threads[ThreadCount];

        for (int32 Index = 0; Index < ThreadCount; ++Index)
        {
            Threads[Index] = std::thread([&, Index]
            {
                ECS::FComponentTypeInfo Info;
                Info.Name = Names[Index];
                Info.Size = 4 * (Index + 1);
                Info.Alignment = 4;

                Ready.fetch_add(1);
                while (Ready.load() < ThreadCount)
                {
                }
                Ids[Index] = ECS::FComponentTypeRegistry::Get().Acquire(Info);
            });
        }

        for (std::thread& Thread : Threads)
        {
            Thread.join();
        }

        for (int32 Index = 0; Index < ThreadCount; ++Index)
        {
            const ECS::FComponentTypeInfo& Info = ECS::FComponentTypeRegistry::Get().GetInfo(Ids[Index]);
            ASSERT_EQ(Info.Name, Names[Index]) << "round " << Round << ", thread " << Index << " was handed another type's id";
            ASSERT_EQ(Info.Size, 4u * (Index + 1));
            ASSERT_EQ(ECS::FComponentTypeRegistry::Get().FindByName(Names[Index]), Ids[Index]);

            for (int32 Other = Index + 1; Other < ThreadCount; ++Other)
            {
                ASSERT_NE(Ids[Index], Ids[Other]) << "round " << Round << ": threads " << Index << " and " << Other << " share an id";
            }
        }
    }
}
