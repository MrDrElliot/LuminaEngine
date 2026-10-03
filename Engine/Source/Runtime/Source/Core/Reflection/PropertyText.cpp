#include "RuntimePCH.h"
#include "PropertyText.h"

#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
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

        bool IsScalarTextConvertible(const FProperty* Property)
        {
            switch (Property->GetType())
            {
            case EPropertyTypeFlags::Int8:
            case EPropertyTypeFlags::Int16:
            case EPropertyTypeFlags::Int32:
            case EPropertyTypeFlags::Int64:
            case EPropertyTypeFlags::UInt8:
            case EPropertyTypeFlags::UInt16:
            case EPropertyTypeFlags::UInt32:
            case EPropertyTypeFlags::Entity:
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

        const FProperty* ArrayElementProperty(const FProperty* Property)
        {
            if (Property->GetType() != EPropertyTypeFlags::Vector)
            {
                return nullptr;
            }
            const FProperty* Inner = static_cast<const FArrayProperty*>(Property)->GetInternalProperty();
            return Inner != nullptr && IsScalarTextConvertible(Inner) ? Inner : nullptr;
        }

        bool IsBlank(char C)
        {
            return C == ' ' || C == '\t';
        }

        FStringView Trim(FStringView Text)
        {
            while (!Text.empty() && IsBlank(Text.front()))
            {
                Text.remove_prefix(1);
            }
            while (!Text.empty() && IsBlank(Text.back()))
            {
                Text.remove_suffix(1);
            }
            return Text;
        }

        // An element that would otherwise read back differently is quoted, with an embedded quote doubled as CSV does.
        void AppendArrayElement(FString& Out, const FString& Element)
        {
            const bool bNeedsQuotes = Element.empty() || IsBlank(Element.front()) || IsBlank(Element.back())
                || Element.find_first_of(",()\"") != FString::npos;
            if (!bNeedsQuotes)
            {
                Out += Element;
                return;
            }
            Out += '"';
            for (char C : Element)
            {
                if (C == '"')
                {
                    Out += '"';
                }
                Out += C;
            }
            Out += '"';
        }

        // Splits (a,"b,c",d) into its elements, or fails on anything that is not one parenthesized list.
        bool SplitArrayText(FStringView Text, TVector<FString>& OutElements)
        {
            Text = Trim(Text);
            OutElements.clear();
            if (Text.empty())
            {
                return true;
            }
            if (Text.size() < 2 || Text.front() != '(' || Text.back() != ')')
            {
                return false;
            }
            Text = Text.substr(1, Text.size() - 2);
            if (Trim(Text).empty())
            {
                return true;
            }

            size_t At = 0;
            while (true)
            {
                while (At < Text.size() && IsBlank(Text[At]))
                {
                    ++At;
                }

                FString Element;
                if (At < Text.size() && Text[At] == '"')
                {
                    ++At;
                    bool bClosed = false;
                    while (At < Text.size())
                    {
                        if (Text[At] == '"')
                        {
                            if (At + 1 < Text.size() && Text[At + 1] == '"')
                            {
                                Element += '"';
                                At += 2;
                                continue;
                            }
                            ++At;
                            bClosed = true;
                            break;
                        }
                        Element += Text[At++];
                    }
                    if (!bClosed)
                    {
                        return false;
                    }
                    while (At < Text.size() && IsBlank(Text[At]))
                    {
                        ++At;
                    }
                }
                else
                {
                    const size_t Start = At;
                    while (At < Text.size() && Text[At] != ',')
                    {
                        ++At;
                    }
                    const FStringView Bare = Trim(Text.substr(Start, At - Start));
                    Element.assign(Bare.data(), Bare.size());
                }

                OutElements.push_back(Move(Element));
                if (At >= Text.size())
                {
                    return true;
                }
                if (Text[At] != ',')
                {
                    return false;
                }
                ++At;
            }
        }

    }

    static FString ValueToText(const FProperty* Property, const void* ConstValue);
    static bool ValueFromText(const FProperty* Property, void* Value, FStringView Text);

    bool IsTextConvertible(const FProperty* Property)
    {
        if (Property == nullptr)
        {
            return false;
        }
        if (ArrayElementProperty(Property) != nullptr)
        {
            return true;
        }

        switch (Property->GetType())
        {
        case EPropertyTypeFlags::Int8:
        case EPropertyTypeFlags::Int16:
        case EPropertyTypeFlags::Int32:
        case EPropertyTypeFlags::Int64:
        case EPropertyTypeFlags::UInt8:
        case EPropertyTypeFlags::UInt16:
        case EPropertyTypeFlags::UInt32:
        case EPropertyTypeFlags::Entity:
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

        if (const FProperty* Element = ArrayElementProperty(Prop))
        {
            const FArrayProperty* Array = static_cast<const FArrayProperty*>(Prop);
            FString Out = "(";
            const SIZE_T Num = Array->GetNum(Value);
            for (SIZE_T i = 0; i < Num; ++i)
            {
                if (i > 0)
                {
                    Out += ',';
                }
                AppendArrayElement(Out, ValueToText(Element, Array->GetAt(Value, i)));
            }
            Out += ')';
            return Out;
        }

        return ValueToText(Prop, Value);
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

        if (const FProperty* Element = ArrayElementProperty(Prop))
        {
            TVector<FString> Elements;
            if (!SplitArrayText(Text, Elements))
            {
                return false;
            }

            // Parsed into a tail past the old elements, so one bad element is undone by trimming that tail.
            const FArrayProperty* Array = static_cast<const FArrayProperty*>(Prop);
            const SIZE_T OldNum = Array->GetNum(Value);
            const SIZE_T NewNum = Elements.size();
            Array->Resize(Value, OldNum + NewNum);
            for (SIZE_T i = 0; i < NewNum; ++i)
            {
                if (!ValueFromText(Element, Array->GetAt(Value, OldNum + i), FStringView(Elements[i].c_str(), Elements[i].size())))
                {
                    Array->Resize(Value, OldNum);
                    return false;
                }
            }
            for (SIZE_T i = 0; i < NewNum && OldNum > 0; ++i)
            {
                Array->Swap(Value, i, OldNum + i);
            }
            Array->Resize(Value, NewNum);
            return true;
        }

        return ValueFromText(Prop, Value, Text);
    }

    static FString ValueToText(const FProperty* Property, const void* ConstValue)
    {
        FProperty* Prop = const_cast<FProperty*>(Property);
        void* Value = const_cast<void*>(ConstValue);

        switch (Prop->GetType())
        {
        case EPropertyTypeFlags::Int8:    return Format("{}", (int32)*static_cast<int8*>(Value)).c_str();
        case EPropertyTypeFlags::Int16:   return Format("{}", (int32)*static_cast<int16*>(Value)).c_str();
        case EPropertyTypeFlags::Int32:   return Format("{}", *static_cast<int32*>(Value)).c_str();
        case EPropertyTypeFlags::Int64:   return Format("{}", *static_cast<int64*>(Value)).c_str();
        case EPropertyTypeFlags::UInt8:   return Format("{}", (uint32)*static_cast<uint8*>(Value)).c_str();
        case EPropertyTypeFlags::UInt16:  return Format("{}", (uint32)*static_cast<uint16*>(Value)).c_str();
        case EPropertyTypeFlags::UInt32:
        case EPropertyTypeFlags::Entity:  return Format("{}", *static_cast<uint32*>(Value)).c_str();
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

    static bool ValueFromText(const FProperty* Property, void* Value, FStringView Text)
    {
        FProperty* Prop = const_cast<FProperty*>(Property);

        switch (Prop->GetType())
        {
        case EPropertyTypeFlags::Int8:    return ParseSigned(Text, *static_cast<int8*>(Value));
        case EPropertyTypeFlags::Int16:   return ParseSigned(Text, *static_cast<int16*>(Value));
        case EPropertyTypeFlags::Int32:   return ParseSigned(Text, *static_cast<int32*>(Value));
        case EPropertyTypeFlags::Int64:   return ParseSigned(Text, *static_cast<int64*>(Value));
        case EPropertyTypeFlags::UInt8:   return ParseUnsigned(Text, *static_cast<uint8*>(Value));
        case EPropertyTypeFlags::UInt16:  return ParseUnsigned(Text, *static_cast<uint16*>(Value));
        case EPropertyTypeFlags::UInt32:
        case EPropertyTypeFlags::Entity:  return ParseUnsigned(Text, *static_cast<uint32*>(Value));
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
