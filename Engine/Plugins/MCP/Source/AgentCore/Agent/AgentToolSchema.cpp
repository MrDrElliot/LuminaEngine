#include "AgentCorePCH.h"
#include "Agent/AgentToolSchema.h"

#include "Agent/AgentReflectionUtils.h"
#include "Containers/StringFormat.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/ClassProperty.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/InstancedStructProperty.h"
#include "Core/Reflection/Type/Properties/MapProperty.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Core/Reflection/Type/Properties/OptionalProperty.h"
#include "Core/Reflection/Type/Properties/SoftObjectProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Core/Reflection/Type/Properties/SubStructProperty.h"

namespace Lumina::Agent
{
    namespace AgentToolSchemaInternal
    {
        // A struct that reaches itself would otherwise recurse until the stack runs out.
        constexpr int32 GMaxDepth = 8;

        bool BuildProperty(FProperty* Property, FStringView Path, int32 Depth, FJsonValue& Out, FString& OutError);

        // Doc comments land in ToolTip, so a normal comment above a PROPERTY becomes the description.
        void ApplyDescription(const FProperty& Property, FJsonValue& Out)
        {
            const FCStringView Text = Property.GetMetadata("ToolTip");
            if (Text.IsEmpty())
            {
                return;
            }

            // An object property already wrote what it wants, so the comment joins it rather than replacing it.
            const FStringView Existing = Out["description"].AsString();
            const FStringView Comment  = Text.View();

            Out.Set("description", Existing.IsEmpty() ? FString(Comment) : Lumina::Format("{} {}", Comment, Existing));
        }

        bool BuildEnum(FEnumProperty* Property, FJsonValue& Out, FString& OutError)
        {
            CEnum* Enum = Property->GetEnum();
            if (Enum == nullptr)
            {
                OutError = "its enum type is missing";
                return false;
            }

            // A bitmask is a set rather than one choice, and a plain enum list would misdescribe it.
            if (Enum->IsBitmaskEnum())
            {
                OutError = "bitmask enums are not expressible yet";
                return false;
            }

            FJsonValue Names = FJsonValue::MakeArray();
            for (const TPair<FName, uint64>& Entry : Enum->Names)
            {
                Names.Add(Entry.Key.ToString());
            }

            if (Names.IsEmpty())
            {
                OutError = "its enum has no values";
                return false;
            }

            Out.Set("type", "string");
            Out.Set("enum", Move(Names));
            return true;
        }

        bool BuildStruct(CStruct* Struct, FStringView Path, int32 Depth, FJsonValue& Out, FString& OutError)
        {
            if (Struct == nullptr)
            {
                OutError = "its struct type is missing";
                return false;
            }

            if (Depth > GMaxDepth)
            {
                OutError = Lumina::Format("it nests deeper than {} levels", GMaxDepth);
                return false;
            }

            FJsonValue Properties = FJsonValue::MakeObject();

            FString FieldError;
            bool bOk = true;

            for (CStruct* Current : Detail::CollectStructChain(Struct))
            {
                Current->ForEachProperty<FProperty>([&](FProperty* Property)
                {
                    if (!bOk || Property == nullptr)
                    {
                        return;
                    }

                    const FString FieldName(*Property->GetPropertyName().ToString());
                    const FString FieldPath = Path.IsEmpty()
                        ? FieldName
                        : Lumina::Format("{}.{}", Path, FieldName);

                    FJsonValue Field;
                    if (!BuildProperty(Property, FStringView(FieldPath), Depth + 1, Field, FieldError))
                    {
                        bOk = false;
                        return;
                    }

                    // A null schema means the field is deliberately invisible, not a failure.
                    if (Field.IsNull())
                    {
                        return;
                    }

                    Properties.Set(FieldName, Move(Field));
                });

                if (!bOk)
                {
                    OutError = FieldError;
                    return false;
                }
            }

            Out.Set("type", "object");
            Out.Set("properties", Move(Properties));

            if (Struct->HasMeta("ToolTip") && !Struct->GetMeta("ToolTip").IsEmpty())
            {
                Out.Set("description", FStringView(Struct->GetMeta("ToolTip")));
            }

            return true;
        }

        bool BuildProperty(FProperty* Property, FStringView Path, int32 Depth, FJsonValue& Out, FString& OutError)
        {
            Out = FJsonValue::MakeObject();

            const auto Reject = [&](FStringView Reason)
            {
                OutError = Lumina::Format("'{}' cannot be described because {}.", Path, Reason);
                return false;
            };

            // A nested failure already names the full path, so re-wrapping it would only stutter.
            const auto Propagate = [&](const FString& Reason)
            {
                OutError = Reason;
                return false;
            };

            switch (Property->GetType())
            {
            case EPropertyTypeFlags::Int8:
            case EPropertyTypeFlags::Int16:
            case EPropertyTypeFlags::Int32:
            case EPropertyTypeFlags::Int64:
            case EPropertyTypeFlags::UInt8:
            case EPropertyTypeFlags::UInt16:
            case EPropertyTypeFlags::UInt32:
            case EPropertyTypeFlags::UInt64:
                Out.Set("type", "integer");
                break;

            case EPropertyTypeFlags::Entity:
                Out.Set("type", "integer");
                Out.Set("description", "A packed entity handle. entity.set_property also takes an entity id.");
                break;

            case EPropertyTypeFlags::Float:
            case EPropertyTypeFlags::Double:
                Out.Set("type", "number");
                break;

            case EPropertyTypeFlags::Bool:
                Out.Set("type", "boolean");
                break;

            case EPropertyTypeFlags::String:
                // RawJson strings carry any JSON value, which the server re-encodes before the struct is read.
                if (Property->HasEditorFlag(EPropertyEditorFlags::RawJson))
                {
                    Out.Set("description", "Any JSON value.");
                    break;
                }
                Out.Set("type", "string");
                break;

            case EPropertyTypeFlags::Name:
                Out.Set("type", "string");
                break;

            case EPropertyTypeFlags::SoftObject:
                {
                    CClass* Expected = static_cast<FSoftObjectProperty*>(Property)->GetPropertyClass();

                    Out.Set("type", "string");
                    Out.Set("description", Lumina::Format(
                        "Path or GUID of a {} asset, or an empty string for none.",
                        Expected != nullptr ? Expected->GetName().ToString() : FString("CObject")));
                    break;
                }

            case EPropertyTypeFlags::Class:
                {
                    CClass* Base = static_cast<FClassProperty*>(Property)->GetMetaClass();

                    Out.Set("type", "string");
                    Out.Set("description", Lumina::Format(
                        "Name of a class deriving from {}, or an empty string for none.",
                        Base != nullptr ? Base->GetName().ToString() : FString("CObject")));
                    break;
                }

            case EPropertyTypeFlags::SubStruct:
                {
                    CStruct* Base = static_cast<FSubStructProperty*>(Property)->GetMetaStruct();

                    Out.Set("type", "string");
                    Out.Set("description", Lumina::Format(
                        "Name of a struct deriving from {}, or an empty string for none.",
                        Base != nullptr ? Base->GetName().ToString() : FString("any struct")));
                    break;
                }

            case EPropertyTypeFlags::Optional:
                {
                    FProperty* Inner = static_cast<FOptionalProperty*>(Property)->GetInternalProperty();
                    if (Inner == nullptr)
                    {
                        return Reject("its value type is missing");
                    }

                    FJsonValue Value;
                    FString Reason;
                    if (!BuildProperty(Inner, Path, Depth + 1, Value, Reason))
                    {
                        return Propagate(Reason);
                    }

                    FJsonValue Record = FJsonValue::MakeObject();
                    Record.Set("type", "object");
                    FJsonValue& RecordProperties = Record.Set("properties", FJsonValue::MakeObject());
                    RecordProperties.Set("Engaged", FJsonValue::MakeObject({ { "type", "boolean" } }));
                    RecordProperties.Set("Value", Value);

                    Out.Set("anyOf", FJsonValue::MakeArray({ FJsonValue::MakeObject({ { "type", "null" } }), Value, Move(Record) }));
                    Out.Set("description", "Optional: null for none, the value itself, or {\"Engaged\": bool, \"Value\": ...}.");
                    break;
                }

            case EPropertyTypeFlags::InstancedStruct:
                {
                    CStruct* Base = static_cast<FInstancedStructProperty*>(Property)->GetMetaStruct();

                    FJsonValue StructType = FJsonValue::MakeObject();
                    StructType.Set("type", "string");
                    StructType.Set("description", Lumina::Format(
                        "Name of a struct deriving from {}, or an empty string for none.",
                        Base != nullptr ? Base->GetName().ToString() : FString("any struct")));

                    Out.Set("type", "object");
                    FJsonValue& Fields = Out.Set("properties", FJsonValue::MakeObject());
                    Fields.Set("StructType", Move(StructType));
                    Fields.Set("Data", FJsonValue::MakeObject({ { "type", "object" } }));
                    Out.Set("description", "Instanced struct: the concrete type by name, and its fields under Data.");
                    break;
                }

            case EPropertyTypeFlags::Map:
                {
                    FMapProperty* Map = static_cast<FMapProperty*>(Property);
                    FProperty* KeyProperty = Map->GetKeyProperty();
                    FProperty* ValueProperty = Map->GetValueProperty();
                    if (KeyProperty == nullptr || ValueProperty == nullptr)
                    {
                        return Reject("its key or value type is missing");
                    }

                    FJsonValue Key;
                    FJsonValue Value;
                    FString Reason;
                    if (!BuildProperty(KeyProperty, Path, Depth + 1, Key, Reason)
                        || !BuildProperty(ValueProperty, Path, Depth + 1, Value, Reason))
                    {
                        return Propagate(Reason);
                    }

                    FJsonValue Flat = FJsonValue::MakeObject();
                    Flat.Set("type", "array");
                    Flat.Set("description", "Alternating key, value, key, value, ...");

                    FJsonValue Forms = FJsonValue::MakeArray({ Move(Flat) });

                    // Only a key JSON can spell as an object key gets the object form.
                    const bool bObjectKeyed = Key["type"].AsString() == "string" || Key["type"].AsString() == "integer";
                    if (bObjectKeyed)
                    {
                        FJsonValue Keyed = FJsonValue::MakeObject();
                        Keyed.Set("type", "object");
                        Keyed.Set("additionalProperties", Value);
                        Forms.Add(Move(Keyed));
                    }

                    Out.Set("anyOf", Move(Forms));
                    Out.Set("description", bObjectKeyed
                        ? "Map: an object keyed by the map key, or a flat [key, value, ...] array."
                        : "Map: a flat [key, value, key, value, ...] array.");
                    break;
                }

            case EPropertyTypeFlags::Delegate:
                // Not data, so it is left out of the schema rather than making the struct undescribable.
                Out = FJsonValue();
                return true;

            case EPropertyTypeFlags::Object:
                {
                    CClass* Expected = static_cast<FObjectProperty*>(Property)->GetPropertyClass();

                    Out.Set("type", "string");
                    Out.Set("description", Lumina::Format(
                        "GUID of a {} asset, or an empty string for none.",
                        Expected != nullptr ? Expected->GetName().ToString() : FString("CObject")));
                    break;
                }

            case EPropertyTypeFlags::Enum:
                {
                    FString Reason;
                    if (!BuildEnum(static_cast<FEnumProperty*>(Property), Out, Reason))
                    {
                        return Reject(Reason);
                    }
                    break;
                }

            case EPropertyTypeFlags::Struct:
                {
                    FString Reason;
                    if (!BuildStruct(static_cast<FStructProperty*>(Property)->GetStruct(), Path, Depth, Out, Reason))
                    {
                        return Propagate(Reason);
                    }
                    break;
                }

            case EPropertyTypeFlags::Vector:
                {
                    FProperty* Inner = static_cast<FArrayProperty*>(Property)->GetInternalProperty();
                    if (Inner == nullptr)
                    {
                        return Reject("its element type is missing");
                    }

                    FJsonValue Items;
                    FString Reason;
                    if (!BuildProperty(Inner, Path, Depth + 1, Items, Reason))
                    {
                        return Propagate(Reason);
                    }

                    Out.Set("type", "array");
                    Out.Set("items", Move(Items));
                    break;
                }

            default:
                // Refused outright, so a tool never advertises a shape the parser would not accept.
                return Reject(Lumina::Format("{} properties are not supported yet",
                    Property->GetTypeName().ToString()));
            }

            ApplyDescription(*Property, Out);
            return true;
        }
    }

    using namespace AgentToolSchemaInternal;

    FSchemaResult GeneratePropertySchema(FProperty* Property)
    {
        FSchemaResult Result;

        if (Property == nullptr)
        {
            Result.Error = "No property was given.";
            return Result;
        }

        const FString Path(*Property->GetPropertyName().ToString());

        FString Error;
        if (!BuildProperty(Property, FStringView(Path), 0, Result.Schema, Error))
        {
            Result.Schema = FJsonValue();
            Result.Error  = Error;
        }

        return Result;
    }

    FSchemaResult GenerateSchema(CStruct* Struct)
    {
        FSchemaResult Result;

        if (Struct == nullptr)
        {
            Result.Error = "No struct was given.";
            return Result;
        }

        FString Error;
        if (!BuildStruct(Struct, FStringView(), 0, Result.Schema, Error))
        {
            Result.Schema = FJsonValue();
            Result.Error  = Error;
            return Result;
        }

        return Result;
    }
}
