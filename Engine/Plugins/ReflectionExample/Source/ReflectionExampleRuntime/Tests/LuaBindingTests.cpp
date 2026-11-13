#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "LuaBinding.h"
#include "LuaExampleCounter.h"

using namespace Lumina;

TEST(LuaBindings, ReachesAReflectedClassThroughTheGeneratedTables)
{
    RegisterPendingTypes();
    TStrongObjectPtr<CLuaExampleCounter> Counter(NewObject<CLuaExampleCounter>(OF_Transient));

    const Lua::FClassBinding* Binding = Lua::Find(Lua::GetClassBindings(), "CLuaExampleCounter");
    ASSERT_NE(Binding, nullptr);
    EXPECT_EQ(Lua::Find(Binding->Methods, "Missing"), nullptr);

    Lua::FStack Stack{ Lua::FValue(static_cast<void*>(Counter.Get())), Lua::FValue(int64(5)) };
    EXPECT_EQ(Lua::Find(Binding->Setters, "Count")->Thunk(Stack), 0);
    EXPECT_EQ(Counter->Count, 5);

    Stack = { Lua::FValue(static_cast<void*>(Counter.Get())), Lua::FValue(FString("Clicks")) };
    Lua::Find(Binding->Setters, "Label")->Thunk(Stack);
    EXPECT_EQ(Counter->Label, "Clicks");

    Stack = { Lua::FValue(static_cast<void*>(Counter.Get())) };
    ASSERT_EQ(Lua::Find(Binding->Getters, "Count")->Thunk(Stack), 1);
    EXPECT_EQ(Stack.Last().Get<int64>(), 5);

    // Lua passes a float where the C++ wants an int32, which Check converts.
    Stack = { Lua::FValue(static_cast<void*>(Counter.Get())), Lua::FValue(2.0) };
    ASSERT_EQ(Lua::Find(Binding->Methods, "Add")->Thunk(Stack), 1);
    EXPECT_EQ(Stack.Last().Get<int64>(), 7);
    EXPECT_EQ(Counter->Count, 7);
}
