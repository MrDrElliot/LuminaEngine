#include <gtest/gtest.h>

#include "Config/InputSettings.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Serialization/Structured/JsonStructuredArchive.h"
#include "Input/InputAction.h"

using namespace Lumina;
using Json = Lumina::FJsonValue;

// A copied scope handle closes twice and writes the rest of the struct into the PARENT node.

namespace
{
    SInputActionBinding MakeBinding(Lumina::EKey Key, float Scale, Lumina::EInputAxisChannel Channel)
    {
        SInputActionBinding Binding;
        Binding.Key.SetKey(Key);
        Binding.Scale = Scale;
        Binding.Channel = Channel;
        return Binding;
    }

    SInputAction MakeAction(const char* Name)
    {
        SInputAction Action;
        Action.Name = Name;
        Action.Type = Lumina::EInputActionType::Axis2D;
        Action.bRunsInUI = true;
        Action.DeadZone = 0.25f;
        Action.Sensitivity = 2.5f;
        Action.bInvert = true;
        Action.HoldTime = 0.5f;
        Action.TapTime = 0.125f;
        Action.Bindings.Add(MakeBinding(Lumina::EKey::D, 1.0f, Lumina::EInputAxisChannel::X));
        Action.Bindings.Add(MakeBinding(Lumina::EKey::A, -1.0f, Lumina::EInputAxisChannel::X));
        Action.Bindings.Add(MakeBinding(Lumina::EKey::W, 1.0f, Lumina::EInputAxisChannel::Y));
        return Action;
    }
}

// Fields after a nested array are the first thing an unbalanced scope loses.
TEST(StructuredArchive, StructWithNestedArrayKeepsEveryFieldInItsOwnNode)
{
    const SInputAction Source = MakeAction("Move");

    Json Node;
    FJsonStructuredArchive::SaveStruct(Node, SInputAction::StaticStruct(), (void*)&Source);

    ASSERT_TRUE(Node.IsObject());
    EXPECT_EQ(Node.GetString("Name"), "Move");
    EXPECT_EQ(Node.GetString("Type"), "Axis2D");
    EXPECT_TRUE(Node.GetValue("bRunsInUI", false));
    EXPECT_TRUE(Node.GetValue("bInvert", false));
    EXPECT_FLOAT_EQ(Node.GetValue("DeadZone", 0.0f), 0.25f);
    EXPECT_FLOAT_EQ(Node.GetValue("Sensitivity", 0.0f), 2.5f);
    EXPECT_FLOAT_EQ(Node.GetValue("HoldTime", 0.0f), 0.5f);
    EXPECT_FLOAT_EQ(Node.GetValue("TapTime", 0.0f), 0.125f);

    // Every binding belongs to Bindings, and each one carries its own nested SKey record.
    ASSERT_TRUE(Node.Contains("Bindings"));
    ASSERT_TRUE(Node["Bindings"].IsArray());
    ASSERT_EQ(Node["Bindings"].Num(), 3);
    for (const Json& Binding : Node["Bindings"].AsArray())
    {
        ASSERT_TRUE(Binding.IsObject());
        EXPECT_TRUE(Binding.Contains("Key"));
        EXPECT_TRUE(Binding["Key"].IsObject());
        EXPECT_TRUE(Binding.Contains("Scale"));
        EXPECT_TRUE(Binding.Contains("Source"));
        EXPECT_TRUE(Binding.Contains("Channel"));
    }
    EXPECT_EQ(Node["Bindings"][0]["Key"].GetString("Key"), "D");
    EXPECT_FLOAT_EQ(Node["Bindings"][1].GetValue("Scale", 0.0f), -1.0f);
    // An enum is written through FName, which keeps whichever spelling of the name was interned first.
    EXPECT_STRCASEEQ(*Node["Bindings"][2].GetString("Channel"), "Y");

    SInputAction Loaded;
    FJsonStructuredArchive::LoadStruct(Node, SInputAction::StaticStruct(), &Loaded);

    EXPECT_EQ(Loaded.Name, Source.Name);
    EXPECT_EQ(Loaded.Type, Source.Type);
    EXPECT_EQ(Loaded.bRunsInUI, Source.bRunsInUI);
    EXPECT_EQ(Loaded.bInvert, Source.bInvert);
    EXPECT_FLOAT_EQ(Loaded.DeadZone, Source.DeadZone);
    EXPECT_FLOAT_EQ(Loaded.Sensitivity, Source.Sensitivity);
    EXPECT_FLOAT_EQ(Loaded.HoldTime, Source.HoldTime);
    EXPECT_FLOAT_EQ(Loaded.TapTime, Source.TapTime);

    ASSERT_EQ(Loaded.Bindings.Num(), Source.Bindings.Num());
    for (int32 i = 0; i < Source.Bindings.Num(); ++i)
    {
        EXPECT_EQ(Loaded.Bindings[i].Key.Key, Source.Bindings[i].Key.Key);
        EXPECT_EQ(Loaded.Bindings[i].Key.Device, Source.Bindings[i].Key.Device);
        EXPECT_FLOAT_EQ(Loaded.Bindings[i].Scale, Source.Bindings[i].Scale);
        EXPECT_EQ(Loaded.Bindings[i].Source, Source.Bindings[i].Source);
        EXPECT_EQ(Loaded.Bindings[i].Channel, Source.Bindings[i].Channel);
    }
}

// An array of structs each carrying their own array of structs must stay nested.
TEST(StructuredArchive, ArrayOfStructsWithNestedArraysStaysNested)
{
    TVector<SInputAction> Actions;
    Actions.Add(MakeAction("Move"));
    Actions.Add(MakeAction("Look"));

    Json Node;
    {
        FJsonStructuredArchive Ar(Node, false);
        FArchiveSlot Root = Ar.Open();
        int32 Count = Actions.Num();
        FArchiveArray Array = Root.EnterArray(Count);
        for (SInputAction& Action : Actions)
        {
            FArchiveSlot Element = Array.EnterElement();
            FArchiveRecord Record = Element.EnterRecord();
            SInputAction::StaticStruct()->SerializeProperties(Record, &Action);
        }
    }

    ASSERT_TRUE(Node.IsArray()) << Node.ToString(2);
    ASSERT_EQ(Node.Num(), 2) << Node.ToString(2);
    EXPECT_EQ(Node[0].GetString("Name"), "Move");
    EXPECT_EQ(Node[1].GetString("Name"), "Look");
    ASSERT_TRUE(Node[0]["Bindings"].IsArray());
    EXPECT_EQ(Node[0]["Bindings"].Num(), 3) << Node.ToString(2);
    EXPECT_EQ(Node[1]["Bindings"].Num(), 3) << Node.ToString(2);

    TVector<SInputAction> Loaded;
    {
        FJsonStructuredArchive Ar(Node, true);
        FArchiveSlot Root = Ar.Open();
        int32 Count = 0;
        FArchiveArray Array = Root.EnterArray(Count);
        Loaded.SetNum(size_t(Count));
        for (SInputAction& Action : Loaded)
        {
            FArchiveSlot Element = Array.EnterElement();
            FArchiveRecord Record = Element.EnterRecord();
            SInputAction::StaticStruct()->SerializeProperties(Record, &Action);
        }
    }

    ASSERT_EQ(Loaded.Num(), 2);
    EXPECT_EQ(Loaded[0].Name, FName("Move"));
    EXPECT_EQ(Loaded[1].Name, FName("Look"));
    EXPECT_FLOAT_EQ(Loaded[0].Sensitivity, 2.5f);
    ASSERT_EQ(Loaded[0].Bindings.Num(), 3);
    ASSERT_EQ(Loaded[1].Bindings.Num(), 3);
    EXPECT_EQ(Loaded[1].Bindings[2].Channel, Lumina::EInputAxisChannel::Y);
}
