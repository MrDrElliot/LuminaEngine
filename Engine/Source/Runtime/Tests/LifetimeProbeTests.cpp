#include <gtest/gtest.h>

#include "Core/Object/Class.h"
#include "Core/Reflection/LifetimeProbeTestTypes.h"
#include "Core/Reflection/PropertySnapshot.h"
#include "Core/Reflection/PropertyText.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Core/Reflection/Type/Properties/OptionalProperty.h"
#include "Core/Reflection/Type/Properties/PropertyHeader.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/NetArchive.h"
#include "Memory/Memory.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/Component.h"
#include "ScriptTestUtil.h"

using namespace Lumina;

namespace LifetimeProbeTestsInternal
{
    SLifetimeProbeStruct MakeItem(int32 Value)
    {
        SLifetimeProbeStruct Item;
        Item.Value = Value;
        Item.Text = "an item label long enough that the string owns a heap buffer";
        return Item;
    }

    void Fill(SLifetimeProbeComponent& Component, int32 Count)
    {
        Component.Single = MakeItem(1);
        Component.List.Reset();
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Component.List.Add(MakeItem(Index));
        }
        Component.Maybe = MakeItem(2);
        Component.Map.Reset();
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Component.Map[Index] = MakeItem(Index * 10);
        }
        Component.Payload.InitializeAs<SLifetimeProbeStruct>() = MakeItem(3);
        Component.Payloads.Reset();
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Component.Payloads.EmplaceGetRef().InitializeAs<SLifetimeProbeStruct>() = MakeItem(Index + 100);
        }
    }

    FProperty* Property(const char* Name)
    {
        return SLifetimeProbeComponent::StaticStruct()->GetProperty(FName(Name));
    }

    TVector<FProperty*> AllProperties()
    {
        TVector<FProperty*> Out;
        for (const char* Name : { "Single", "List", "Maybe", "Map", "Payload", "Payloads" })
        {
            Out.Add(Property(Name));
        }
        return Out;
    }

    Scripting::FScriptExportField StructField(const char* Name, TSharedPtr<Scripting::FScriptExportType> Type)
    {
        Scripting::FScriptExportField Field;
        Field.Name = FName(Name);
        Field.Type = std::move(Type);
        return Field;
    }

    TSharedPtr<Scripting::FScriptExportType> ProbeType()
    {
        TSharedPtr<Scripting::FScriptExportType> Type = MakeShared<Scripting::FScriptExportType>();
        Type->Kind = EPropertyTypeFlags::Struct;
        Type->NativeName = SLifetimeProbeStruct::StaticStruct()->GetName();
        return Type;
    }

    TSharedPtr<Scripting::FScriptExportType> Wrap(EPropertyTypeFlags Kind, TSharedPtr<Scripting::FScriptExportType> Element)
    {
        TSharedPtr<Scripting::FScriptExportType> Type = MakeShared<Scripting::FScriptExportType>();
        Type->Kind = Kind;
        Type->ElementType = std::move(Element);
        return Type;
    }

    Scripting::FManagedTypeDefinition ScriptDefinition(const char* Name, bool bExtraField)
    {
        Scripting::FScriptExportSchema Schema;
        if (bExtraField)
        {
            Schema.Fields.Add(ScriptTest::ScalarField("Leading", EPropertyTypeFlags::Int64));
        }
        Schema.Fields.Add(StructField("Single", ProbeType()));
        Schema.Fields.Add(StructField("List", Wrap(EPropertyTypeFlags::Vector, ProbeType())));
        Schema.Fields.Add(StructField("Maybe", Wrap(EPropertyTypeFlags::Optional, ProbeType())));

        TSharedPtr<Scripting::FScriptExportType> Map = MakeShared<Scripting::FScriptExportType>();
        Map->Kind = EPropertyTypeFlags::Map;
        Map->KeyType = ScriptTest::ScalarField("Key", EPropertyTypeFlags::Int32).Type;
        Map->ValueType = ProbeType();
        Schema.Fields.Add(StructField("Map", Map));

        Scripting::FManagedTypeDefinition Definition = ScriptTest::StructDefinition(Name, std::move(Schema));
        Definition.Meta.Add({ "Component", "" });
        return Definition;
    }

    void FillScript(CStruct* Struct, void* Value, int32 Count)
    {
        *Struct->GetProperty(FName("Single"))->GetValuePtr<SLifetimeProbeStruct>(Value) = MakeItem(5);

        auto* List = static_cast<FArrayProperty*>(Struct->GetProperty(FName("List")));
        void* ListValue = List->GetValuePtr<void>(Value);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const SLifetimeProbeStruct Item = MakeItem(Index);
            List->Add(ListValue, &Item);
        }

        auto* Maybe = static_cast<FOptionalProperty*>(Struct->GetProperty(FName("Maybe")));
        const SLifetimeProbeStruct Item = MakeItem(6);
        Maybe->SetValue(Maybe->GetValuePtr<void>(Value), &Item);

        auto* Map = static_cast<FMapProperty*>(Struct->GetProperty(FName("Map")));
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const SLifetimeProbeStruct Entry = MakeItem(Index * 7);
            Map->Add(Map->GetValuePtr<void>(Value), &Index, &Entry);
        }
    }
}

class FLifetimeProbeTest : public ::testing::Test
{
protected:

    void SetUp() override
    {
        LiveBefore = FLifetimeProbe::GetLiveCount();
        ErrorsBefore = FLifetimeProbe::GetErrorCount();
    }

    void TearDown() override
    {
        EXPECT_EQ(FLifetimeProbe::GetErrorCount(), ErrorsBefore) << "a probe was touched unconstructed, after destruction, or after a bytewise move";
        EXPECT_EQ(FLifetimeProbe::GetLiveCount(), LiveBefore) << "constructions and destructions do not balance";
    }

    // A minted type keeps a default value of its own for as long as it exists, superseded or not.
    void PublishMintingOneDefault(std::initializer_list<Scripting::FManagedTypeDefinition> Definitions)
    {
        const int64 Before = FLifetimeProbe::GetLiveCount();
        ScriptTest::PublishStructs(Definitions);
        EXPECT_EQ(FLifetimeProbe::GetLiveCount(), Before + 1) << "a reload leaked or double destroyed a migrated value";
        LiveBefore += 1;
    }

    int64 LiveBefore = 0;
    int64 ErrorsBefore = 0;
};

TEST_F(FLifetimeProbeTest, NativeContainersBalance)
{
    SLifetimeProbeComponent A;
    LifetimeProbeTestsInternal::Fill(A, 6);
    SLifetimeProbeComponent B = A;
    SLifetimeProbeComponent C = std::move(B);
    B = C;
    A.List.RemoveAt(1);
    A.Map.Remove(2);
}

TEST_F(FLifetimeProbeTest, StructOpsBalance)
{
    CStruct* Struct = SLifetimeProbeComponent::StaticStruct();
    ASSERT_TRUE(Struct->RequiresValueLifecycle());

    void* First = Memory::Malloc(Struct->GetSize(), Struct->GetAlignment());
    void* Second = Memory::Malloc(Struct->GetSize(), Struct->GetAlignment());
    Struct->InitializeStruct(First);
    Struct->InitializeStruct(Second);
    LifetimeProbeTestsInternal::Fill(*static_cast<SLifetimeProbeComponent*>(First), 5);

    Struct->CopyStruct(Second, First);
    EXPECT_TRUE(Struct->CompareStruct(First, Second));
    LifetimeProbeTestsInternal::Fill(*static_cast<SLifetimeProbeComponent*>(First), 9);
    Struct->CopyStruct(Second, First);
    LifetimeProbeTestsInternal::Fill(*static_cast<SLifetimeProbeComponent*>(First), 2);
    Struct->CopyStruct(Second, First);

    Struct->DestroyStruct(First);
    Struct->DestroyStruct(Second);
    Memory::Free(First);
    Memory::Free(Second);
}

TEST_F(FLifetimeProbeTest, CopyPropertiesGrowsAndShrinksThroughTheOps)
{
    CStruct* Struct = SLifetimeProbeComponent::StaticStruct();
    SLifetimeProbeComponent Small;
    SLifetimeProbeComponent Large;
    LifetimeProbeTestsInternal::Fill(Small, 2);
    LifetimeProbeTestsInternal::Fill(Large, 11);

    SLifetimeProbeComponent Target;
    Struct->CopyProperties(&Target, &Small);
    Struct->CopyProperties(&Target, &Large);
    EXPECT_EQ(Target.List.Num(), 11);
    Struct->CopyProperties(&Target, &Small);
    EXPECT_EQ(Target.List.Num(), 2);
    EXPECT_EQ(Target.Map.Num(), 2);
}

TEST_F(FLifetimeProbeTest, SerializingIntoALiveValueReplacesItCleanly)
{
    CStruct* Struct = SLifetimeProbeComponent::StaticStruct();
    for (const auto [SourceCount, TargetCount] : { std::pair{ 3, 12 }, std::pair{ 12, 3 }, std::pair{ 0, 7 }, std::pair{ 7, 0 } })
    {
        SLifetimeProbeComponent Source;
        LifetimeProbeTestsInternal::Fill(Source, SourceCount);

        TVector<uint8> Bytes;
        {
            FMemoryWriter Writer(Bytes);
            Struct->SerializeProperties(Writer, &Source);
        }

        SLifetimeProbeComponent Target;
        LifetimeProbeTestsInternal::Fill(Target, TargetCount);
        FMemoryReader Reader(Bytes);
        Struct->SerializeProperties(Reader, &Target);

        EXPECT_EQ(Target.List.Num(), SourceCount);
        EXPECT_EQ(Target.Map.Num(), SourceCount);
        EXPECT_EQ(Target.Payloads.Num(), SourceCount);
    }
}

TEST_F(FLifetimeProbeTest, ReflectedContainerEditsBalance)
{
    SLifetimeProbeComponent Value;
    LifetimeProbeTestsInternal::Fill(Value, 4);

    auto* List = static_cast<FArrayProperty*>(LifetimeProbeTestsInternal::Property("List"));
    const SLifetimeProbeStruct Item = LifetimeProbeTestsInternal::MakeItem(42);
    for (int32 Index = 0; Index < 20; ++Index)
    {
        List->Add(&Value.List, &Item);
    }
    List->RemoveAt(&Value.List, 0);
    List->Swap(&Value.List, 1, 5);
    List->SetNum(&Value.List, 40);
    List->SetNum(&Value.List, 3);
    List->Reserve(&Value.List, 64);
    List->Reset(&Value.List);

    auto* Maybe = static_cast<FOptionalProperty*>(LifetimeProbeTestsInternal::Property("Maybe"));
    Maybe->SetValue(&Value.Maybe, &Item);
    Maybe->SetValue(&Value.Maybe, &Item);
    Maybe->Reset(&Value.Maybe);
    Maybe->SetValue(&Value.Maybe, &Item);

    auto* Map = static_cast<FMapProperty*>(LifetimeProbeTestsInternal::Property("Map"));
    for (int32 Key = 0; Key < 30; ++Key)
    {
        Map->Add(&Value.Map, &Key, &Item);
    }
    const int32 Existing = 3;
    Map->Add(&Value.Map, &Existing, nullptr);
    Map->Remove(&Value.Map, &Existing);
    Map->Reset(&Value.Map);
}

TEST_F(FLifetimeProbeTest, InstancedStructsBalance)
{
    FInstancedStruct A;
    A.InitializeAs<SLifetimeProbeStruct>() = LifetimeProbeTestsInternal::MakeItem(1);
    FInstancedStruct B = A;
    FInstancedStruct C = std::move(B);
    B = C;
    A = std::move(C);
    A.InitializeAs(SLifetimeProbeComponent::StaticStruct());
    A.InitializeAs(SLifetimeProbeStruct::StaticStruct());
    B.Reset();
    EXPECT_TRUE(A.Identical(A));
}

TEST_F(FLifetimeProbeTest, TextRoundTripsBalance)
{
    SLifetimeProbeComponent Source;
    LifetimeProbeTestsInternal::Fill(Source, 5);
    SLifetimeProbeComponent Target;
    LifetimeProbeTestsInternal::Fill(Target, 9);

    for (FProperty* Field : LifetimeProbeTestsInternal::AllProperties())
    {
        if (!Reflection::IsTextConvertible(Field))
        {
            continue;
        }
        const FString Text = Reflection::ToText(Field, &Source);
        EXPECT_TRUE(Reflection::FromText(Field, &Target, Text)) << *Field->GetPropertyName();
    }
}

TEST_F(FLifetimeProbeTest, SnapshotsBalance)
{
    SLifetimeProbeComponent Value;
    LifetimeProbeTestsInternal::Fill(Value, 5);

    FPropertySnapshot Snapshot;
    Snapshot.Capture(LifetimeProbeTestsInternal::AllProperties(), &Value);
    LifetimeProbeTestsInternal::Fill(Value, 8);
    for (int32 Index = 0; Index < Snapshot.Num(); ++Index)
    {
        Snapshot.Matches(Index, &Value);
        Snapshot.Store(Index, &Value);
    }
    Snapshot.Capture(LifetimeProbeTestsInternal::AllProperties(), &Value);
    Snapshot.Reset();
}

TEST_F(FLifetimeProbeTest, ComponentStorageBalances)
{
    const FComponentOps* Ops = FindComponentOps("SLifetimeProbeComponent");
    ASSERT_NE(Ops, nullptr);
    {
        ECS::FRegistry Registry;
        TVector<ECS::FEntity> Entities;
        for (int32 Index = 0; Index < 300; ++Index)
        {
            const ECS::FEntity Entity = Registry.Create();
            Entities.Add(Entity);
            LifetimeProbeTestsInternal::Fill(*static_cast<SLifetimeProbeComponent*>(Ops->Emplace(Registry, Entity)), 2);
        }
        for (int32 Index = 0; Index < Entities.Num(); Index += 3)
        {
            Ops->Remove(Registry, Entities[Index]);
        }

        SLifetimeProbeComponent Source;
        LifetimeProbeTestsInternal::Fill(Source, 4);
        for (int32 Index = 0; Index < 50; ++Index)
        {
            Ops->EmplaceCopy(Registry, Registry.Create(), &Source);
        }

        TVector<uint8> Bytes;
        {
            FMemoryWriter Writer(Bytes);
            SLifetimeProbeComponent::StaticStruct()->SerializeProperties(Writer, &Source);
        }
        FMemoryReader Reader(Bytes);
        Ops->EmplaceSerialized(Registry, Registry.Create(), Reader);

        for (int32 Index = 1; Index < Entities.Num(); Index += 3)
        {
            Registry.Destroy(Entities[Index]);
        }

        void* Loose = Ops->New();
        Ops->Delete(Loose);
    }
}

TEST_F(FLifetimeProbeTest, ScriptStructOpsBalance)
{
    PublishMintingOneDefault({ LifetimeProbeTestsInternal::ScriptDefinition("SComp_GTestLifetimeProbe", false) });
    const FComponentOps* Ops = FindComponentOps("SComp_GTestLifetimeProbe");
    ASSERT_NE(Ops, nullptr);
    CStruct* Struct = Ops->StaticStruct();
    {
        void* A = Ops->New();
        void* B = Ops->New();
        LifetimeProbeTestsInternal::FillScript(Struct, A, 6);
        Struct->CopyStruct(B, A);
        LifetimeProbeTestsInternal::FillScript(Struct, A, 20);
        Struct->CopyProperties(B, A);

        TVector<uint8> Bytes;
        {
            FMemoryWriter Writer(Bytes);
            Struct->SerializeProperties(Writer, A);
        }
        FMemoryReader Reader(Bytes);
        Struct->SerializeProperties(Reader, B);

        ECS::FRegistry Registry;
        for (int32 Index = 0; Index < 200; ++Index)
        {
            Ops->EmplaceCopy(Registry, Registry.Create(), Index % 2 == 0 ? A : B);
        }
        Ops->Delete(A);
        Ops->Delete(B);
    }
    ScriptTest::PublishStructs({});
}

TEST_F(FLifetimeProbeTest, ScriptComponentsSurviveAReshapingReload)
{
    PublishMintingOneDefault({ LifetimeProbeTestsInternal::ScriptDefinition("SComp_GTestLifetimeReload", false) });
    const FComponentOps* Ops = FindComponentOps("SComp_GTestLifetimeReload");
    ASSERT_NE(Ops, nullptr);
    {
        ECS::FRegistry Registry;
        for (int32 Index = 0; Index < 40; ++Index)
        {
            LifetimeProbeTestsInternal::FillScript(Ops->StaticStruct(), Ops->Emplace(Registry, Registry.Create()), 3);
        }

        PublishMintingOneDefault({ LifetimeProbeTestsInternal::ScriptDefinition("SComp_GTestLifetimeReload", true) });
        PublishMintingOneDefault({ LifetimeProbeTestsInternal::ScriptDefinition("SComp_GTestLifetimeReload", false) });
    }
    ScriptTest::PublishStructs({});
}

TEST_F(FLifetimeProbeTest, NetRoundTripsOptionalsAndInstancedStructs)
{
    using namespace LifetimeProbeTestsInternal;
    SLifetimeProbeComponent Source;
    Fill(Source, 3);

    TVector<uint8> Bytes;
    {
        FNetArchive Writer(Bytes);
        bool bLeadingBit = true;
        Writer.SerializeBit(bLeadingBit);
        for (const char* Name : { "Maybe", "Payload", "Payloads" })
        {
            Property(Name)->NetSerialize(Writer, Property(Name)->GetValuePtr<void>(&Source));
        }
    }

    SLifetimeProbeComponent Target;
    FNetArchive Reader(Bytes.GetData(), Bytes.Num());
    bool bLeadingBit = false;
    Reader.SerializeBit(bLeadingBit);
    for (const char* Name : { "Maybe", "Payload", "Payloads" })
    {
        Property(Name)->NetSerialize(Reader, Property(Name)->GetValuePtr<void>(&Target));
    }

    ASSERT_FALSE(Reader.HasError());
    EXPECT_TRUE(bLeadingBit) << "an unaligned value ahead of the optional survives";
    ASSERT_TRUE(Target.Maybe.IsSet());
    EXPECT_EQ(Target.Maybe->Value, 2);
    ASSERT_NE(Target.Payload.GetPtr<SLifetimeProbeStruct>(), nullptr);
    EXPECT_EQ(Target.Payload.GetPtr<SLifetimeProbeStruct>()->Value, 3);
    ASSERT_EQ(Target.Payloads.Num(), 3);
    ASSERT_NE(Target.Payloads[2].GetPtr<SLifetimeProbeStruct>(), nullptr);
    EXPECT_EQ(Target.Payloads[2].GetPtr<SLifetimeProbeStruct>()->Text, Source.Payloads[2].GetPtr<SLifetimeProbeStruct>()->Text);
}

TEST_F(FLifetimeProbeTest, UnresolvedInstancedStructSkipsItsPayload)
{
    using namespace LifetimeProbeTestsInternal;
    SLifetimeProbeComponent Source;
    Fill(Source, 3);

    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        Property("Payloads")->Serialize(Writer, Property("Payloads")->GetValuePtr<void>(&Source));
    }

    const char Key[] = "LifetimeProbeStruct";
    const int32 KeyLength = FStringView(Key).Len();
    int32 Found = Bytes.Num();
    for (int32 Index = 0; Index + KeyLength <= Bytes.Num(); ++Index)
    {
        if (Memory::Memcmp(Bytes.GetData() + Index, Key, KeyLength) == 0)
        {
            Found = Index;
            break;
        }
    }
    ASSERT_LT(Found, Bytes.Num()) << "the first element's type key is in the bytes";
    Bytes[Found + KeyLength - 1] = 'X';

    SLifetimeProbeComponent Target;
    FMemoryReader Reader(Bytes);
    Property("Payloads")->Serialize(Reader, Property("Payloads")->GetValuePtr<void>(&Target));

    EXPECT_FALSE(Reader.HasError());
    ASSERT_EQ(Target.Payloads.Num(), 3);
    EXPECT_EQ(Target.Payloads[0].GetScriptStruct(), nullptr);
    ASSERT_NE(Target.Payloads[1].GetPtr<SLifetimeProbeStruct>(), nullptr) << "the element after an unknown type still lines up";
    EXPECT_EQ(Target.Payloads[1].GetPtr<SLifetimeProbeStruct>()->Value, 101);
    ASSERT_NE(Target.Payloads[2].GetPtr<SLifetimeProbeStruct>(), nullptr);
    EXPECT_EQ(Target.Payloads[2].GetPtr<SLifetimeProbeStruct>()->Value, 102);
}

TEST(SerializationGuards, PropertySizePastTheEndIsRefused)
{
    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        uint32 Count = 1;
        Writer << Count;
        FPropertyHeader Header;
        Header.Type = EPropertyTypeFlags::Int32;
        Header.Name = FName("Value");
        Header.Size = 1 << 30;
        Writer << Header;
        int32 Payload = 7;
        Writer << Payload;
    }

    SLifetimeProbeStruct Target;
    FMemoryReader Reader(Bytes);
    SLifetimeProbeStruct::StaticStruct()->SerializeProperties(Reader, &Target);
    EXPECT_TRUE(Reader.HasError());
}

TEST(SerializationGuards, NetSeekOutsideThePacketFails)
{
    uint8 Data[4] = {};
    FNetArchive Past(Data, sizeof(Data));
    Past.Seek(5);
    EXPECT_TRUE(Past.HasError());

    FNetArchive Before(Data, sizeof(Data));
    Before.Seek(-1);
    EXPECT_TRUE(Before.HasError());

    FNetArchive End(Data, sizeof(Data));
    End.Seek(4);
    EXPECT_FALSE(End.HasError());
}

TEST(SerializationGuards, MapCountPastTheEndIsRefused)
{
    TVector<uint8> Bytes;
    {
        FMemoryWriter Writer(Bytes);
        uint32 Count = 0x7FFFFFF0u;
        Writer << Count;
    }
    THashMap<int32, int32> Map;
    Map[1] = 2;
    FMemoryReader Reader(Bytes);
    Reader << Map;
    EXPECT_TRUE(Reader.HasError());
}
