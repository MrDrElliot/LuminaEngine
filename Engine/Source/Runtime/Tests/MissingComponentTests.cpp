#include <gtest/gtest.h>

#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/Package/PackageNameTable.h"
#include "Core/Serialization/PortableBytes.h"
#include "Scripting/ManagedTypeRegistry.h"
#include "Scripting/ScriptStruct.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Components/MissingComponents.h"
#include "ScriptTestUtil.h"
#include "Assets/AssetTypes/Prefabs/PrefabOverrideTestTypes.h"

using namespace Lumina;
using namespace Lumina::Scripting;

namespace
{
    FManagedTypeDefinition HeldDefinition(const char* Name)
    {
        FScriptExportSchema Schema;
        Schema.Fields.Add(ScriptTest::ScalarField("Health", EPropertyTypeFlags::Float));
        Schema.Fields.Add(ScriptTest::ScalarField("Label", EPropertyTypeFlags::String));
        Schema.Fields.Add(ScriptTest::ScalarField("Tag", EPropertyTypeFlags::Name));
        FManagedTypeDefinition Definition = ScriptTest::StructDefinition(Name, std::move(Schema));
        Definition.Meta.Add({ "Component", "" });
        return Definition;
    }

    template<typename T>
    T& Field(CStruct* Struct, void* Value, const char* Name)
    {
        return *Struct->GetProperty(FName(Name))->GetValuePtr<T>(Value);
    }

    void Fill(CStruct* Struct, void* Value)
    {
        Field<float>(Struct, Value, "Health") = 42.5f;
        Field<FString>(Struct, Value, "Label") = "long enough that the string owns a heap buffer";
        Field<FName>(Struct, Value, "Tag") = FName("HeldTag", 7);
    }

    void ExpectFilled(CStruct* Struct, void* Value)
    {
        EXPECT_FLOAT_EQ(Field<float>(Struct, Value, "Health"), 42.5f);
        EXPECT_EQ(Field<FString>(Struct, Value, "Label"), FString("long enough that the string owns a heap buffer"));
        EXPECT_EQ(Field<FName>(Struct, Value, "Tag"), FName("HeldTag", 7));
    }

    // Writes Holder through a destination whose name table already holds other names, then reads it back.
    SMissingComponents ThroughAnotherPackage(SMissingComponents& Holder)
    {
        TVector<uint8> Stream;
        TVector<uint8> TableBytes;
        {
            FPackageContainerWriter Writer(Stream);
            FName Unrelated[] = { FName("Unrelated_A"), FName("Unrelated_B"), FName("Unrelated_C") };
            for (FName& Name : Unrelated)
            {
                Writer << Name;
            }
            Holder.Serialize(Writer);
            FMemoryWriter TableWriter(TableBytes);
            Writer.GetNameMap().Serialize(TableWriter);
        }

        FPackageNameTable Table;
        FMemoryReader TableReader(TableBytes);
        Table.Serialize(TableReader);

        FPackageContainerReader Reader(Stream);
        Reader.SetNameTable(&Table);
        FName Skipped;
        for (int32 Index = 0; Index < 3; ++Index)
        {
            Reader << Skipped;
        }
        SMissingComponents Copy;
        Copy.Serialize(Reader);
        EXPECT_FALSE(Reader.HasError());
        return Copy;
    }
}

// A held value's name slots are its own, so a destination with a different table still reads every name back.
TEST(MissingComponents, HeldBytesReadBackThroughAnotherNameTable)
{
    ScriptTest::PublishStructs({ HeldDefinition("SComp_GTestHeldBytes") });
    const FComponentOps* Ops = FindComponentOps("SComp_GTestHeldBytes");
    ASSERT_NE(Ops, nullptr);
    CStruct* Struct = Ops->StaticStruct();

    void* Source = Ops->New();
    Fill(Struct, Source);

    SMissingComponents Holder;
    SHeldValue& Entry = Holder.Entries.EmplaceGetRef();
    Entry.TypeName = FName("SComp_GTestHeldBytes");
    Entry.Data.Capture(*Struct, Source);
    Ops->Delete(Source);

    SMissingComponents Copy = ThroughAnotherPackage(Holder);
    ASSERT_EQ(Copy.Entries.Num(), 1);
    EXPECT_EQ(Copy.Entries[0].TypeName, FName("SComp_GTestHeldBytes"));

    void* Restored = Ops->New();
    const TUniquePtr<FArchive> Reader = Copy.Entries[0].Data.MakeReader();
    Struct->SerializeProperties(*Reader, Restored);
    ExpectFilled(Struct, Restored);
    Ops->Delete(Restored);
}

// A C# component whose type a reload removed is held under its own name, saved, and restored when the type returns.
TEST(MissingComponents, ARemovedTypeIsHeldThenRestoredWhenItReturns)
{
    ScriptTest::PublishStructs({ HeldDefinition("SComp_GTestHeldType") });
    const FComponentOps* Ops = FindComponentOps("SComp_GTestHeldType");
    ASSERT_NE(Ops, nullptr);

    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();
    Fill(Ops->StaticStruct(), Ops->Emplace(Registry, Entity));

    ScriptTest::PublishStructs({});
    EXPECT_EQ(MissingComponents::HoldOrphaned(Registry), 1);
    EXPECT_EQ(Ops->Has(Registry, Entity), 0);
    SMissingComponents* Holder = Registry.TryGet<SMissingComponents>(Entity);
    ASSERT_NE(Holder, nullptr);
    ASSERT_EQ(Holder->Entries.Num(), 1);
    EXPECT_EQ(Holder->Entries[0].TypeName, FName("SComp_GTestHeldType")) << "held under the C# name, not the retired one";

    *Holder = ThroughAnotherPackage(*Holder);
    EXPECT_EQ(MissingComponents::Restore(Registry), 0) << "nothing comes back while the type is still gone";

    ScriptTest::PublishStructs({ HeldDefinition("SComp_GTestHeldType") });
    EXPECT_EQ(MissingComponents::Restore(Registry), 1);
    EXPECT_EQ(Registry.TryGet<SMissingComponents>(Entity), nullptr) << "an empty holder is removed";

    const FComponentOps* Returned = FindComponentOps("SComp_GTestHeldType");
    ASSERT_NE(Returned, nullptr);
    void* Value = Returned->Get(Registry, Entity);
    ASSERT_NE(Value, nullptr);
    ExpectFilled(Returned->StaticStruct(), Value);
}

// A struct renamed with [Alias] takes over the old pool, and a stored old name resolves to the new type.
TEST(MissingComponents, ARenamedTypeTakesOverItsOldPool)
{
    ScriptTest::PublishStructs({ HeldDefinition("SComp_GTestBefore") });
    const FComponentOps* Before = FindComponentOps("SComp_GTestBefore");
    ASSERT_NE(Before, nullptr);

    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();
    Fill(Before->StaticStruct(), Before->Emplace(Registry, Entity));

    const FManagedTypeAlias Alias{ "SComp_GTestBefore", "SComp_GTestAfter" };
    FScriptableRegistry::SetRedirects(TSpan<const FManagedTypeAlias>(&Alias, 1));
    ScriptTest::PublishStructs({ HeldDefinition("SComp_GTestAfter") });

    const FComponentOps* After = FindComponentOps("SComp_GTestAfter");
    ASSERT_NE(After, nullptr);
    EXPECT_EQ(After->GetTypeID(), Before->GetTypeID()) << "the renamed type keeps the pool its live components sit in";
    void* Value = After->Get(Registry, Entity);
    ASSERT_NE(Value, nullptr);
    ExpectFilled(After->StaticStruct(), Value);
    EXPECT_EQ(FScriptableRegistry::ResolveStruct(FName("SComp_GTestBefore")), After->StaticStruct());

    FScriptableRegistry::SetRedirects({});
}

// Copies made while the type is gone stack their remaps, and the restore applies them as if each had run on the live value.
TEST(MissingComponents, HeldEntityRemapsComposeAcrossCopies)
{
    RegisterPendingTypes();
    const CStruct* Layout = SPrefabLinkTestComponent::StaticStruct();
    auto E = [](uint32 Packed) { return ECS::FEntity(Packed); };

    SHeldValue Held;
    Held.RemapEntities({ { E(1), E(10) }, { E(2), E(20) } }, /*bClearUnmapped*/ false);
    Held.RemapEntities({ { E(10), E(100) } }, /*bClearUnmapped*/ true);

    auto Resolve = [&](uint32 Original)
    {
        SPrefabLinkTestComponent Value;
        Value.Target = E(Original);
        Held.ApplyEntityRemap(Layout, &Value);
        return Value.Target;
    };

    EXPECT_EQ(Resolve(1), E(100)) << "both copies apply in order";
    EXPECT_EQ(Resolve(2), ECS::FEntity(ECS::NullEntity)) << "the second copy did not carry the first copy's target";
    EXPECT_EQ(Resolve(10), E(100)) << "a handle the first copy left alone takes the second map";
    EXPECT_EQ(Resolve(5), ECS::FEntity(ECS::NullEntity)) << "an unmapped handle clears once a copy says so";

    // Round trips with the value, so a level saved while the type is gone keeps the pending remap.
    TVector<uint8> Bytes;
    FMemoryWriter Writer(Bytes);
    Writer << Held;
    SHeldValue Loaded;
    FMemoryReader Reader(Bytes);
    Reader << Loaded;
    EXPECT_EQ(Loaded.RemapFrom, Held.RemapFrom);
    EXPECT_EQ(Loaded.RemapTo, Held.RemapTo);
    EXPECT_TRUE(Loaded.bClearUnmappedEntities);
}
