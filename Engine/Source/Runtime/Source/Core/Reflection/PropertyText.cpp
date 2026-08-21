#include "RuntimePCH.h"
#include "PropertyText.h"

#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/ObjectProperty.h"
#include "Containers/StringFormat.h"

namespace Lumina::Reflection
{
    namespace
    {
        // Trailing junk is an error rather than ignored, since a stray suffix is a typo in a CSV.
        template <typename T>
        bool ParseSigned(FStringView Text, T& Out)
        {
            char* End = nullptr;
            const FString Owned(Text.data(), Text.size());
            const long long Parsed = std::strtoll(Owned.c_str(), &End, 10);
            if (End == Owned.c_str() || *End != '\0')
            {
                return false;
            }
            Out = static_cast<T>(Parsed);
            return true;
        }

        template <typename T>
        bool ParseUnsigned(FStringView Text, T& Out)
        {
            char* End = nullptr;
            const FString Owned(Text.data(), Text.size());
            const unsigned long long Parsed = std::strtoull(Owned.c_str(), &End, 10);
            if (End == Owned.c_str() || *End != '\0')
            {
                return false;
            }
            Out = static_cast<T>(Parsed);
            return true;
        }

        template <typename T>
        bool ParseFloating(FStringView Text, T& Out)
        {
            char* End = nullptr;
            const FString Owned(Text.data(), Text.size());
            const double Parsed = std::strtod(Owned.c_str(), &End);
            if (End == Owned.c_str() || *End != '\0')
            {
                return false;
            }
            Out = static_cast<T>(Parsed);
            return true;
        }

        bool ParseBool(FStringView Text, bool& Out)
        {
            const FString Owned(Text.data(), Text.size());
            // Accept what a spreadsheet or a human is likely to have typed, not just one spelling.
            if (Owned == "1" || Owned == "true"  || Owned == "True"  || Owned == "TRUE")  { Out = true;  return true; }
            if (Owned == "0" || Owned == "false" || Owned == "False" || Owned == "FALSE") { Out = false; return true; }
            return false;
        }
    }

    bool IsTextConvertible(const FProperty* Property)
    {
        if (Property == nullptr)
        {
            return false;
        }

        switch (const_cast<FProperty*>(Property)->GetType())
        {
        case EPropertyTypeFlags::Int8:
        case EPropertyTypeFlags::Int16:
        case EPropertyTypeFlags::Int32:
        case EPropertyTypeFlags::Int64:
        case EPropertyTypeFlags::UInt8:
        case EPropertyTypeFlags::UInt16:
        case EPropertyTypeFlags::UInt32:
        case EPropertyTypeFlags::UInt64:
        case EPropertyTypeFlags::Float:
        case EPropertyTypeFlags::Double:
        case EPropertyTypeFlags::Bool:
        case EPropertyTypeFlags::Name:
        case EPropertyTypeFlags::String:
        case EPropertyTypeFlags::Enum:
        case EPropertyTypeFlags::Object:
            return true;
        default:
            return false;
        }
    }

    FString ToText(const FProperty* Property, const void* Container)
    {
        if (!IsTextConvertible(Property) || Container == nullptr)
        {
            return FString();
        }

        FProperty* Prop = const_cast<FProperty*>(Property);
        void* Value = Prop->GetValuePtr<void>(const_cast<void*>(Container));
        if (Value == nullptr)
        {
            return FString();
        }

        switch (Prop->GetType())
        {
        case EPropertyTypeFlags::Int8:    return Format("{}", (int32)*static_cast<int8*>(Value)).c_str();
        case EPropertyTypeFlags::Int16:   return Format("{}", (int32)*static_cast<int16*>(Value)).c_str();
        case EPropertyTypeFlags::Int32:   return Format("{}", *static_cast<int32*>(Value)).c_str();
        case EPropertyTypeFlags::Int64:   return Format("{}", *static_cast<int64*>(Value)).c_str();
        case EPropertyTypeFlags::UInt8:   return Format("{}", (uint32)*static_cast<uint8*>(Value)).c_str();
        case EPropertyTypeFlags::UInt16:  return Format("{}", (uint32)*static_cast<uint16*>(Value)).c_str();
        case EPropertyTypeFlags::UInt32:  return Format("{}", *static_cast<uint32*>(Value)).c_str();
        case EPropertyTypeFlags::UInt64:  return Format("{}", *static_cast<uint64*>(Value)).c_str();

        // to_string on a float emits six trailing zeroes, making a table of whole numbers unreadable.
        case EPropertyTypeFlags::Float:
            return Format("{:g}", (double)*static_cast<float*>(Value));
        case EPropertyTypeFlags::Double:
            return Format("{:g}", *static_cast<double*>(Value));

        case EPropertyTypeFlags::Bool:    return *static_cast<bool*>(Value) ? "true" : "false";
        case EPropertyTypeFlags::Name:    return static_cast<FName*>(Value)->ToString();
        case EPropertyTypeFlags::String:  return *static_cast<FString*>(Value);

        case EPropertyTypeFlags::Enum:
            {
                FEnumProperty* EnumProp = static_cast<FEnumProperty*>(Prop);
                CEnum* Enum = EnumProp->GetEnum();
                FNumericProperty* Inner = EnumProp->GetInnerProperty();
                if (Enum == nullptr || Inner == nullptr)
                {
                    return FString();
                }

                // Through the inner, which knows the underlying width and signedness.
                const uint64 Raw = (uint64)Inner->GetSignedIntPropertyValue(Value);

                // A spreadsheet column of qualified names is unreadable, and FromText accepts both forms.
                const FString Qualified = Enum->GetNameAtValue(Raw).ToString();
                const size_t Separator = Qualified.rfind("::");
                return Separator == FString::npos ? Qualified : Qualified.substr(Separator + 2);
            }

        case EPropertyTypeFlags::Object:
            {
                CObject* Object = static_cast<TObjectPtr<CObject>*>(Value)->Get();
                if (Object == nullptr || Object->GetPackage() == nullptr)
                {
                    return FString();
                }
                return Object->GetPackage()->GetName().ToString();
            }

        default:
            return FString();
        }
    }

    bool FromText(const FProperty* Property, void* Container, FStringView Text)
    {
        if (!IsTextConvertible(Property) || Container == nullptr)
        {
            return false;
        }

        FProperty* Prop = const_cast<FProperty*>(Property);
        void* Value = Prop->GetValuePtr<void>(Container);
        if (Value == nullptr)
        {
            return false;
        }

        switch (Prop->GetType())
        {
        case EPropertyTypeFlags::Int8:    return ParseSigned(Text, *static_cast<int8*>(Value));
        case EPropertyTypeFlags::Int16:   return ParseSigned(Text, *static_cast<int16*>(Value));
        case EPropertyTypeFlags::Int32:   return ParseSigned(Text, *static_cast<int32*>(Value));
        case EPropertyTypeFlags::Int64:   return ParseSigned(Text, *static_cast<int64*>(Value));
        case EPropertyTypeFlags::UInt8:   return ParseUnsigned(Text, *static_cast<uint8*>(Value));
        case EPropertyTypeFlags::UInt16:  return ParseUnsigned(Text, *static_cast<uint16*>(Value));
        case EPropertyTypeFlags::UInt32:  return ParseUnsigned(Text, *static_cast<uint32*>(Value));
        case EPropertyTypeFlags::UInt64:  return ParseUnsigned(Text, *static_cast<uint64*>(Value));
        case EPropertyTypeFlags::Float:   return ParseFloating(Text, *static_cast<float*>(Value));
        case EPropertyTypeFlags::Double:  return ParseFloating(Text, *static_cast<double*>(Value));
        case EPropertyTypeFlags::Bool:    return ParseBool(Text, *static_cast<bool*>(Value));

        case EPropertyTypeFlags::Name:
            *static_cast<FName*>(Value) = FName(FString(Text.data(), Text.size()));
            return true;

        case EPropertyTypeFlags::String:
            *static_cast<FString*>(Value) = FString(Text.data(), Text.size());
            return true;

        case EPropertyTypeFlags::Enum:
            {
                FEnumProperty* EnumProp = static_cast<FEnumProperty*>(Prop);
                CEnum* Enum = EnumProp->GetEnum();
                FNumericProperty* Inner = EnumProp->GetInnerProperty();
                if (Enum == nullptr || Inner == nullptr)
                {
                    return false;
                }

                // Accepts the bare enumerator and the qualified form a re-export would produce.
                const FString Typed(Text.data(), Text.size());
                const FName Candidates[] =
                {
                    FName(Typed),
                    FName(Enum->GetName().ToString() + "::" + Typed),
                };

                for (const FName& Entry : Candidates)
                {
                    const uint64 Raw = Enum->GetEnumValueByName(Entry);

                    // GetEnumValueByName cannot separate not-found from a legitimate 0, so confirm the round trip.
                    if (Enum->GetNameAtValue(Raw) != Entry)
                    {
                        continue;
                    }

                    Inner->SetIntPropertyValue(Value, (int64)Raw);
                    return true;
                }

                return false;
            }

        case EPropertyTypeFlags::Object:
            {
                auto* Ptr = static_cast<TObjectPtr<CObject>*>(Value);
                if (Text.empty())
                {
                    *Ptr = nullptr;
                    return true;
                }

                CObject* Loaded = StaticLoadObject(Text);
                if (Loaded == nullptr)
                {
                    return false;
                }

                // A CSV can name any asset, and writing an unrelated one in would bypass the reflection system.
                CClass* Required = static_cast<FObjectProperty*>(Prop)->GetPropertyClass();
                if (Required != nullptr && !Loaded->GetClass()->IsChildOf(Required))
                {
                    return false;
                }

                *Ptr = Loaded;
                return true;
            }

        default:
            return false;
        }
    }
}
