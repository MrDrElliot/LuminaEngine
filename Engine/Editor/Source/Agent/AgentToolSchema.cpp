#include "EditorPCH.h"
#include "Agent/AgentToolSchema.h"

#include "Agent/AgentReflectionUtils.h"
#include "Containers/StringFormat.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"

namespace Lumina::Agent
{
    namespace
    {
        // A struct that reaches itself would otherwise recurse until the stack runs out.
        constexpr int32 GMaxDepth = 8;

        bool BuildProperty(FProperty* Property, FStringView Path, int32 Depth, nlohmann::json& Out, FString& OutError);

        // Doc comments land in ToolTip, so a normal comment above a PROPERTY becomes the description.
        void ApplyDescription(const FProperty& Property, nlohmann::json& Out)
        {
            const FString* Text = Property.TryGetMetadata("ToolTip");
            if (Text == nullptr || Text->empty())
            {
                return;
            }

            // An object property already wrote what it wants, so the comment joins it rather than replacing it.
            const std::string Existing = Out.contains("description") ? Out["description"].get<std::string>() : std::string();
            const std::string Comment  = Detail::ToStandard(FStringView(*Text));

            Out["description"] = Existing.empty() ? Comment : Comment + " " + Existing;
        }

        bool BuildEnum(FEnumProperty* Property, nlohmann::json& Out, FString& OutError)
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

            nlohmann::json Names = nlohmann::json::array();
            for (const TPair<FName, uint64>& Entry : Enum->Names)
            {
                Names.push_back(Detail::ToStandard(FStringView(Entry.first.ToString())));
            }

            if (Names.empty())
            {
                OutError = "its enum has no values";
                return false;
            }

            Out["type"] = "string";
            Out["enum"] = Move(Names);
            return true;
        }

        bool BuildStruct(CStruct* Struct, FStringView Path, int32 Depth, nlohmann::json& Out, FString& OutError)
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

            nlohmann::json Properties = nlohmann::json::object();

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

                    const FString FieldName(Property->GetPropertyName().ToString().c_str());
                    const FString FieldPath = Path.empty()
                        ? FieldName
                        : Lumina::Format("{}.{}", Path, FieldName);

                    nlohmann::json Field;
                    if (!BuildProperty(Property, FStringView(FieldPath), Depth + 1, Field, FieldError))
                    {
                        bOk = false;
                        return;
                    }

                    Properties[Detail::ToStandard(FStringView(Property->GetPropertyName().ToString()))] = Move(Field);
                });

                if (!bOk)
                {
                    OutError = FieldError;
                    return false;
                }
            }

            Out["type"]       = "object";
            Out["properties"] = Move(Properties);

            if (Struct->HasMeta("ToolTip") && !Struct->GetMeta("ToolTip").empty())
            {
                Out["description"] = Detail::ToStandard(FStringView(Struct->GetMeta("ToolTip")));
            }

            return true;
        }

        bool BuildProperty(FProperty* Property, FStringView Path, int32 Depth, nlohmann::json& Out, FString& OutError)
        {
            Out = nlohmann::json::object();

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
                Out["type"] = "integer";
                break;

            case EPropertyTypeFlags::Float:
            case EPropertyTypeFlags::Double:
                Out["type"] = "number";
                break;

            case EPropertyTypeFlags::Bool:
                Out["type"] = "boolean";
                break;

            case EPropertyTypeFlags::String:
            case EPropertyTypeFlags::Name:
                Out["type"] = "string";
                break;

            case EPropertyTypeFlags::Object:
                {
                    CClass* Expected = static_cast<FObjectProperty*>(Property)->GetPropertyClass();

                    Out["type"] = "string";
                    Out["description"] = Detail::ToStandard(FStringView(Lumina::Format(
                        "GUID of a {} asset, or an empty string for none.",
                        Expected != nullptr ? Expected->GetName().ToString() : FString("CObject"))));
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

                    nlohmann::json Items;
                    FString Reason;
                    if (!BuildProperty(Inner, Path, Depth + 1, Items, Reason))
                    {
                        return Propagate(Reason);
                    }

                    Out["type"]  = "array";
                    Out["items"] = Move(Items);
                    break;
                }

            default:
                // Refused outright, so a tool never advertises a shape the parser would not accept.
                return Reject(Lumina::Format("{} properties are not supported yet",
                    Property->TypeName.ToString()));
            }

            ApplyDescription(*Property, Out);
            return true;
        }
    }

    FSchemaResult GeneratePropertySchema(FProperty* Property)
    {
        FSchemaResult Result;

        if (Property == nullptr)
        {
            Result.Error = "No property was given.";
            return Result;
        }

        const FString Path(Property->GetPropertyName().ToString().c_str());

        FString Error;
        if (!BuildProperty(Property, FStringView(Path), 0, Result.Schema, Error))
        {
            Result.Schema = nlohmann::json();
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
            Result.Schema = nlohmann::json();
            Result.Error  = Error;
            return Result;
        }

        return Result;
    }
}
