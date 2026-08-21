#include "RuntimePCH.h"

#include "Core/Object/Class.h"
#include "Containers/StringFormat.h"

IMPLEMENT_INTRINSIC_CLASS(CEnum, CField, RUNTIME_API)
namespace Lumina
{
    FName CEnum::GetNameAtValue(uint64 Value)
    {
        for (const auto& Pair : Names)
        {
            if (Pair.second == Value)
            {
                return Pair.first;
            }
        }

        return NAME_None;
    }

    uint64 CEnum::GetEnumValueByName(const FName& Name)
    {
        for (const auto& Pair : Names)
        {
            if (Pair.first == Name)
            {
                return Pair.second;
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
            
            AppendFormat(BitfieldString, "{}{}", bWroteFirstFlag ? " | " : "", GetNameAtValue(NextValue).c_str());
            bWroteFirstFlag = true;
        }
        
        return BitfieldString;
    }

    void CEnum::AddEnum(FName FullName, uint64 Value)
    {
        const FString& FullString = FullName.ToString();

        size_t DelimPos = FullString.find_last_of(L':');
        if (DelimPos != std::wstring::npos && DelimPos > 0 && FullString[DelimPos - 1] == L':')
        {
            DelimPos--;
            FString ShortName = FullString.substr(DelimPos + 2);
            Names.push_back(Lumina::Containers::MakePair(ShortName, Value));
        }
        else
        {
            Names.push_back(Lumina::Containers::MakePair(FullName, Value));
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
        FFixedString DisplayName = GetName().c_str();

        // Erasing unconditionally ate the first letter of any enum not following the convention.
        if (DisplayName.size() >= 2 && DisplayName[0] == 'E' && std::isupper((unsigned char)DisplayName[1]))
        {
            DisplayName.erase(0, 1);
        }

        return DisplayName;
    }
}
