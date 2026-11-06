#include "RuntimePCH.h"

#include "Core/Object/Class.h"
#include "Core/Object/Field.h"
#include "Containers/StringFormat.h"

IMPLEMENT_INTRINSIC_CLASS(CEnum, CField, RUNTIME_API)
namespace Lumina
{
    FName CEnum::GetNameAtValue(uint64 Value)
    {
        for (const auto& [EnumName, EnumValue] : Names)
        {
            if (EnumValue == Value)
            {
                return EnumName;
            }
        }

        return NAME_None;
    }

    uint64 CEnum::GetEnumValueByName(const FName& Name)
    {
        for (const auto& [EnumName, EnumValue] : Names)
        {
            if (EnumName == Name)
            {
                return EnumValue;
            }
        }

        return 0;
    }

    FFixedString CEnum::GetValueOrBitFieldAsString(int64 Value)
    {
        FFixedString BitfieldString;
        bool bWroteFirstFlag = false;
        while (Value != 0)
        {
            int64 NextValue = 1ll << Math::CountTrailingZeros64(Value);
            Value = Value & ~NextValue;
            
            AppendFormat(BitfieldString, "{}{}", bWroteFirstFlag ? " | " : "", *GetNameAtValue(NextValue));
            bWroteFirstFlag = true;
        }
        
        return BitfieldString;
    }

    void CEnum::AddEnum(FName FullName, uint64 Value)
    {
        const FString& FullString = FullName.ToString();

        const int32 DelimPos = FullString.FindLast(':');
        if (DelimPos > 0 && FullString[DelimPos - 1] == ':')
        {
            FString ShortName = FullString.Mid(DelimPos + 1);
            Names.Add(Lumina::Containers::MakePair(ShortName, Value));
        }
        else
        {
            Names.Add(Lumina::Containers::MakePair(FullName, Value));
        }
    }

    void CEnum::ForEachEnum(const TFunction<void(const TPair<FName, uint64>&)>& Functor)
    {
        for (const auto& Pair : Names)
        {
            Functor(Pair);
        }
    }

    FFixedString CEnum::MakeDisplayName() const
    {
        FFixedString DisplayName = *GetName();

        // Erasing unconditionally ate the first letter of any enum not following the convention.
        if (DisplayName.Len() >= 2 && DisplayName[0] == 'E' && std::isupper((unsigned char)DisplayName[1]))
        {
            DisplayName.RemoveAt(0, 1);
        }

        return DisplayName;
    }
}
