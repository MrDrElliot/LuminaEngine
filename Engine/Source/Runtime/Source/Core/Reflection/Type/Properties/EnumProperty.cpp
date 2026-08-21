#include "RuntimePCH.h"
#include "EnumProperty.h"

namespace Lumina
{
    void FEnumProperty::SetEnum(CEnum* InEnum)
    {
        Enum = InEnum;
    }

    void FEnumProperty::Serialize(FArchive& Ar, void* Value)
    {
        bool bIsBitmaskEnum = Enum->IsBitmaskEnum();
        
        if (Ar.IsReading())
        {
            if (bIsBitmaskEnum)
            {
                FName BitfieldString;
                Ar << BitfieldString;
                
                int64 EnumValue = 0;

                FString Str(BitfieldString.c_str());
                FString Delimiter(" | ");
                size_t Start = 0;
                size_t End = 0;

                while ((End = Str.find(Delimiter, Start)) != FString::npos)
                {
                    FName Token(Str.substr(Start, End - Start).c_str());
                    EnumValue |= (int64)Enum->GetEnumValueByName(Token);
                    Start = End + Delimiter.size();
                }

                FName Token(Str.substr(Start).c_str());
                EnumValue |= (int64)Enum->GetEnumValueByName(Token);

                InnerProperty->SetIntPropertyValue(Value, EnumValue);
            }
            else
            {
                FName EnumName;
                Ar << EnumName;

                int64 EnumValue = (int64)Enum->GetEnumValueByName(EnumName);
                InnerProperty->SetIntPropertyValue(Value, EnumValue);
            }
        }
        else
        {
            const int64 IntValue = InnerProperty->GetSignedIntPropertyValue(Value);

            if (bIsBitmaskEnum)
            {
                FName BitfieldString = Enum->GetValueOrBitFieldAsString(IntValue);
                Ar << BitfieldString;
            }
            else
            {
                FName EnumName = Enum->GetNameAtValue(IntValue);
                Ar << EnumName;
            }
        }
    }

    void FEnumProperty::SerializeItem(IStructuredArchive::FSlot Slot, void* Value, void const* Defaults)
    {
        const bool bIsBitmaskEnum = Enum->IsBitmaskEnum();

        if (Slot.GetArchiver().IsReading())
        {
            FName EnumName;
            Slot.Serialize(EnumName);

            if (EnumName.IsNone())
            {
                // A missing or unknown key leaves the property at its default.
                return;
            }

            int64 EnumValue = 0;
            if (bIsBitmaskEnum)
            {
                FString Str(EnumName.c_str());
                FString Delimiter(" | ");
                size_t Start = 0;
                size_t End = 0;
                while ((End = Str.find(Delimiter, Start)) != FString::npos)
                {
                    FName Token(Str.substr(Start, End - Start).c_str());
                    EnumValue |= (int64)Enum->GetEnumValueByName(Token);
                    Start = End + Delimiter.size();
                }
                FName Token(Str.substr(Start).c_str());
                EnumValue |= (int64)Enum->GetEnumValueByName(Token);
            }
            else
            {
                EnumValue = (int64)Enum->GetEnumValueByName(EnumName);
            }

            InnerProperty->SetIntPropertyValue(Value, EnumValue);
        }
        else
        {
            const int64 IntValue = InnerProperty->GetSignedIntPropertyValue(Value);
            FName EnumName = bIsBitmaskEnum ? Enum->GetValueOrBitFieldAsString(IntValue) : Enum->GetNameAtValue(IntValue);
            Slot.Serialize(EnumName);
        }
    }
}
