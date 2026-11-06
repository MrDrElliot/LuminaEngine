#include <gtest/gtest.h>

#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Scripting/ScriptStruct.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/Component.h"
#include "ScriptTestUtil.h"

using namespace Lumina;
using namespace Lumina::Scripting;

namespace
{
    // What a reload publishes for a [Reflect] C# struct implementing IComponent.
    void PublishHealth(bool bWithArmor)
    {
        FScriptExportSchema Schema;
        Schema.Fields.Add(ScriptTest::ScalarField("Health", EPropertyTypeFlags::Float));
        Schema.Fields.Add(ScriptTest::ScalarField("Label", EPropertyTypeFlags::String));
        if (bWithArmor)
        {
            Schema.Fields.Add(ScriptTest::ScalarField("Armor", EPropertyTypeFlags::Int32));
        }
        FManagedTypeDefinition Definition = ScriptTest::StructDefinition("SComp_GTestHealth", std::move(Schema));
        Definition.Meta.Add({ "Component", "" });
        ScriptTest::PublishStructs({ Definition });
    }

    template<typename T>
    T& Field(CStruct* Struct, void* Value, const char* Name)
    {
        return *Struct->GetProperty(FName(Name))->GetValuePtr<T>(Value);
    }
}

// The same type-erased ops serve a C# component as a native one, and a reshape moves every live value onto the new layout.
TEST(ScriptComponentType, APublishedComponentKeepsItsValuesAcrossAReshape)
{
    PublishHealth(false);
    const FComponentOps* Ops = FindComponentOps("SComp_GTestHealth");
    ASSERT_NE(Ops, nullptr) << "a struct published with the Component specifier must be an ECS component";

    ECS::FRegistry Registry;
    const ECS::FEntity First = Registry.Create();
    const ECS::FEntity Second = Registry.Create();

    CStruct* Original = Ops->StaticStruct();
    void* FirstValue = Ops->Emplace(Registry, First);
    ASSERT_NE(FirstValue, nullptr);
    Field<float>(Original, FirstValue, "Health") = 42.0f;
    Field<FString>(Original, FirstValue, "Label") = "long enough that the string owns a heap buffer";
    Ops->Emplace(Registry, Second);
    EXPECT_EQ(Ops->Has(Registry, First), 1);

    PublishHealth(true);
    EXPECT_EQ(FindComponentOps("SComp_GTestHealth"), Ops) << "the ops keep their identity, so a held C# token stays valid";

    CStruct* Reshaped = Ops->StaticStruct();
    ASSERT_NE(Reshaped, Original);
    void* Moved = Ops->Get(Registry, First);
    ASSERT_NE(Moved, nullptr);
    EXPECT_FLOAT_EQ(Field<float>(Reshaped, Moved, "Health"), 42.0f);
    EXPECT_EQ(Field<FString>(Reshaped, Moved, "Label"), FString("long enough that the string owns a heap buffer"));
    EXPECT_EQ(Field<int32>(Reshaped, Moved, "Armor"), 0);
    EXPECT_EQ(Ops->Has(Registry, Second), 1);

    EXPECT_EQ(Ops->Remove(Registry, First), 1);
    EXPECT_EQ(Ops->Has(Registry, First), 0);
}

// A pool made after the reshape is built at the new size, since the type registry carries the new layout too.
TEST(ScriptComponentType, ANewPoolTakesTheCurrentLayout)
{
    PublishHealth(false);
    PublishHealth(true);
    const FComponentOps* Ops = FindComponentOps("SComp_GTestHealth");
    ASSERT_NE(Ops, nullptr);

    ECS::FRegistry Registry;
    const ECS::FEntity Entity = Registry.Create();
    void* Value = Ops->EmplaceDefault(Registry, Entity);
    ASSERT_NE(Value, nullptr);
    Field<int32>(Ops->StaticStruct(), Value, "Armor") = 7;
    EXPECT_EQ(Registry.FindStorage(Ops->GetTypeID())->GetTypeInfo().Size, Ops->StaticStruct()->GetAlignedSize());
}
