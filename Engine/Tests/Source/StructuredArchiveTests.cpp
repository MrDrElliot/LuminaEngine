#include <gtest/gtest.h>

#include "Config/InputSettings.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Serialization/Structured/JsonStructuredArchive.h"
#include "Input/InputAction.h"

using namespace Lumina;
using Json = nlohmann::json;

// The JSON backend keeps a node stack that mirrors the record/array scope. Every scope is closed by an
// RAII destructor, so anything that copies a scope handle closes it twice and the stack ends up one level
// too shallow -- which silently writes the rest of the struct into the PARENT node. These tests pin the
// shape of the produced JSON, not just the round-trip, because a collapsed stack still round-trips the
// first few fields and only loses everything after the first nested container.

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
        Action.Bindings.push_back(MakeBinding(Lumina::EKey::D, 1.0f, Lumina::EInputAxisChannel::X));
        Action.Bindings.push_back(MakeBinding(Lumina::EKey::A, -1.0f, Lumina::EInputAxisChannel::X));
        Action.Bindings.push_back(MakeBinding(Lumina::EKey::W, 1.0f, Lumina::EInputAxisChannel::Y));
        return Action;
    }
}

// A struct whose fields come AFTER a nested array of structs. Those trailing fields are the first thing an
// unbalanced scope loses.
TEST(StructuredArchive, StructWithNestedArrayKeepsEveryFieldInItsOwnNode)
{
    const SInputAction Source = MakeAction("Move");

    Json Node;
    FJsonStructuredArchive::SaveStruct(Node, SInputAction::StaticStruct(), (void*)&Source);

    ASSERT_TRUE(Node.is_object());
    EXPECT_EQ(Node.value("Name", std::string()), "Move");
    EXPECT_EQ(Node.value("Type", std::string()), "Axis2D");
    EXPECT_TRUE(Node.value("bRunsInUI", false));
    EXPECT_TRUE(Node.value("bInvert", false));
    EXPECT_FLOAT_EQ(Node.value("DeadZone", 0.0f), 0.25f);
    EXPECT_FLOAT_EQ(Node.value("Sensitivity", 0.0f), 2.5f);
    EXPECT_FLOAT_EQ(Node.value("HoldTime", 0.0f), 0.5f);
    EXPECT_FLOAT_EQ(Node.value("TapTime", 0.0f), 0.125f);

    // Every binding belongs to Bindings, and each one carries its own nested SKey record.
    ASSERT_TRUE(Node.contains("Bindings"));
    ASSERT_TRUE(Node["Bindings"].is_array());
    ASSERT_EQ(Node["Bindings"].size(), 3u);
    for (const Json& Binding : Node["Bindings"])
    {
        ASSERT_TRUE(Binding.is_object());
        EXPECT_TRUE(Binding.contains("Key"));
        EXPECT_TRUE(Binding["Key"].is_object());
        EXPECT_TRUE(Binding.contains("Scale"));
        EXPECT_TRUE(Binding.contains("Source"));
        EXPECT_TRUE(Binding.contains("Channel"));
    }
    EXPECT_EQ(Node["Bindings"][0]["Key"].value("Key", std::string()), "D");
    EXPECT_FLOAT_EQ(Node["Bindings"][1].value("Scale", 0.0f), -1.0f);
    EXPECT_EQ(Node["Bindings"][2].value("Channel", std::string()), "Y");

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

    ASSERT_EQ(Loaded.Bindings.size(), Source.Bindings.size());
    for (size_t i = 0; i < Source.Bindings.size(); ++i)
    {
        EXPECT_EQ(Loaded.Bindings[i].Key.Key, Source.Bindings[i].Key.Key);
        EXPECT_EQ(Loaded.Bindings[i].Key.Device, Source.Bindings[i].Key.Device);
        EXPECT_FLOAT_EQ(Loaded.Bindings[i].Scale, Source.Bindings[i].Scale);
        EXPECT_EQ(Loaded.Bindings[i].Source, Source.Bindings[i].Source);
        EXPECT_EQ(Loaded.Bindings[i].Channel, Source.Bindings[i].Channel);
    }
}

// The CInputSettings::Actions shape: an array of structs that each carry their own array of structs. A
// collapsed scope wrote the SECOND element of the inner array into the OUTER array instead. Driven through
// the slot API rather than the settings CDO because the test harness stops short of the deferred
// registration that attaches properties to a CClass.
TEST(StructuredArchive, ArrayOfStructsWithNestedArraysStaysNested)
{
    TVector<SInputAction> Actions;
    Actions.push_back(MakeAction("Move"));
    Actions.push_back(MakeAction("Look"));

    Json Node;
    {
        FJsonStructuredArchive Ar(Node, false);
        FArchiveSlot Root = Ar.Open();
        int32 Count = int32(Actions.size());
        FArchiveArray Array = Root.EnterArray(Count);
        for (SInputAction& Action : Actions)
        {
            FArchiveSlot Element = Array.EnterElement();
            FArchiveRecord Record = Element.EnterRecord();
            SInputAction::StaticStruct()->SerializeTaggedProperties(Record, &Action);
        }
    }

    ASSERT_TRUE(Node.is_array()) << Node.dump(2);
    ASSERT_EQ(Node.size(), 2u) << Node.dump(2);
    EXPECT_EQ(Node[0].value("Name", std::string()), "Move");
    EXPECT_EQ(Node[1].value("Name", std::string()), "Look");
    ASSERT_TRUE(Node[0]["Bindings"].is_array());
    EXPECT_EQ(Node[0]["Bindings"].size(), 3u) << Node.dump(2);
    EXPECT_EQ(Node[1]["Bindings"].size(), 3u) << Node.dump(2);

    TVector<SInputAction> Loaded;
    {
        FJsonStructuredArchive Ar(Node, true);
        FArchiveSlot Root = Ar.Open();
        int32 Count = 0;
        FArchiveArray Array = Root.EnterArray(Count);
        Loaded.resize(size_t(Count));
        for (SInputAction& Action : Loaded)
        {
            FArchiveSlot Element = Array.EnterElement();
            FArchiveRecord Record = Element.EnterRecord();
            SInputAction::StaticStruct()->SerializeTaggedProperties(Record, &Action);
        }
    }

    ASSERT_EQ(Loaded.size(), 2u);
    EXPECT_EQ(Loaded[0].Name, FName("Move"));
    EXPECT_EQ(Loaded[1].Name, FName("Look"));
    EXPECT_FLOAT_EQ(Loaded[0].Sensitivity, 2.5f);
    ASSERT_EQ(Loaded[0].Bindings.size(), 3u);
    ASSERT_EQ(Loaded[1].Bindings.size(), 3u);
    EXPECT_EQ(Loaded[1].Bindings[2].Channel, Lumina::EInputAxisChannel::Y);
}
