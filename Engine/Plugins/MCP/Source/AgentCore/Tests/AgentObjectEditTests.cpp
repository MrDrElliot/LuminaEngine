#include <gtest/gtest.h>

#include "Agent/AgentObjectEdit.h"
#include "World/Entity/Components/LifetimeComponent.h"
#include "World/Entity/Components/NameComponent.h"
#include "World/Entity/Components/ProjectileComponent.h"

using namespace Lumina;
using namespace Lumina::Agent;

namespace AgentObjectEditTestsInternal
{
    // Reflection is off for this module, so every fixture is a type the engine already reflects, and a null owner takes the no-editor path.
    template<typename T>
    FObjectEditResult Set(T& Instance, FStringView Path, const FJsonValue& Value)
    {
        return SetObjectProperty(T::StaticStruct(), &Instance, nullptr, Path, Value, "Test Edit");
    }
}

using namespace AgentObjectEditTestsInternal;

TEST(AgentObjectEdit, AValidWriteUpdatesTheFieldAndReportsBothValues)
{
    SLifetimeComponent Lifetime;
    Lifetime.Lifetime = 1.5f;

    const FObjectEditResult Result = Set(Lifetime, "Lifetime", FJsonValue(2.5f));

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_FLOAT_EQ(Lifetime.Lifetime, 2.5f);
    EXPECT_EQ(Result.Previous, FJsonValue(1.5f).ToString());
    EXPECT_EQ(Result.Current, FJsonValue(2.5f).ToString());
}

TEST(AgentObjectEdit, WithNoOwningEditorTheEditAppliesButIsNotUndoable)
{
    SLifetimeComponent Lifetime;

    const FObjectEditResult Result = Set(Lifetime, "Lifetime", FJsonValue(4.0f));

    ASSERT_TRUE(Result.IsValid()) << *Result.Error;
    EXPECT_FLOAT_EQ(Lifetime.Lifetime, 4.0f);
    EXPECT_FALSE(Result.bUndoable);
}

TEST(AgentObjectEdit, AWrongTypeIsRefusedBeforeAnythingChanges)
{
    SLifetimeComponent Lifetime;
    Lifetime.Lifetime = 1.5f;

    const FObjectEditResult Result = Set(Lifetime, "Lifetime", FJsonValue("soon"));

    EXPECT_FALSE(Result.IsValid());
    EXPECT_FLOAT_EQ(Lifetime.Lifetime, 1.5f);
    EXPECT_TRUE(Result.Previous.IsEmpty());
    EXPECT_TRUE(Result.Current.IsEmpty());
}

TEST(AgentObjectEdit, AReadOnlyFieldIsRefused)
{
    SNameComponent Name;
    Name.Name = "Torch";

    const FObjectEditResult Result = Set(Name, "Name", FJsonValue("Lamp"));

    EXPECT_NE(Result.Error.Find("'Name' is read-only"), Constants::kIndexNone);
    EXPECT_EQ(Name.Name, FName("Torch"));
}

TEST(AgentObjectEdit, ABadPathReturnsThePathError)
{
    SLifetimeComponent Lifetime;

    const FObjectEditResult Result = Set(Lifetime, "Nope", FJsonValue(1.0f));

    EXPECT_NE(Result.Error.Find("'Nope' is not a field of"), Constants::kIndexNone);
}

TEST(AgentObjectEdit, ADelegateFieldIsListedAsUnwritableUnderThePrefix)
{
    TVector<FString> Paths;
    CollectUnwritableFields(SProjectileComponent::StaticStruct(), "Projectile", Paths);

    ASSERT_EQ(Paths.Num(), 1);
    EXPECT_EQ(Paths[0], FString("Projectile.OnHit"));
}
