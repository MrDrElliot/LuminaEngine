#include "RuntimePCH.h"
#include "PropertyValidation.h"

#include "Core/Object/Class.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Log/Log.h"

namespace Lumina::PropertyValidation
{
    namespace
    {
        const FFunction* FindValidator(const FProperty* Property)
        {
            const FStringView Name = Property->GetMetadata("EditValidate");
            if (Name.IsEmpty())
            {
                return nullptr;
            }

            const CStruct* Owner = Property->GetOwnerStruct();
            const FFunction* Function = Owner != nullptr ? Owner->FindFunction(FName(Name)) : nullptr;
            const bool bFits = Function != nullptr && Function->GetArguments().Num() == 1
                && Function->GetReturnParam() != nullptr && Function->GetReturnParam()->GetType() == EPropertyTypeFlags::String
                && Function->GetArguments()[0]->GetType() == Property->GetType();
            if (!bFits)
            {
                LOG_ERROR_ONCE("EditValidate \"{}\" on {} names no function taking one value of the property's type and returning an FString, so nothing checks it.",
                               Name, Property->GetPropertyDisplayName());
                return nullptr;
            }
            return Function;
        }
    }

    FString Check(const FProperty* Property, const void* Container, const void* Candidate)
    {
        const FFunction* Function = Property != nullptr && Candidate != nullptr ? FindValidator(Property) : nullptr;
        if (Function == nullptr || (Container == nullptr && !Function->IsStatic()))
        {
            return FString();
        }

        FFunctionFrame Frame(*Function);
        FProperty* Argument = Function->GetArguments()[0];
        Argument->CopyCompleteValue(Argument->GetValuePtr<uint8>(Frame.GetMemory()), Candidate);
        Frame.Invoke(Function->IsStatic() ? nullptr : const_cast<void*>(Container));
        return Frame.Return<FString>();
    }

    FString CheckCurrent(const FProperty* Property, const void* Container)
    {
        if (Property == nullptr || Container == nullptr)
        {
            return FString();
        }
        return Check(Property, Container, Property->GetValuePtr<uint8>(Container));
    }

    TVector<FPropertyError> CollectErrors(const CStruct* Type, const void* Container)
    {
        TVector<FPropertyError> Errors;
        if (Type == nullptr || Container == nullptr)
        {
            return Errors;
        }

        for (const FProperty* Property : Type->GetProperties())
        {
            if (Property->GetMetadata("EditValidate").IsEmpty())
            {
                continue;
            }
            if (FString Reason = CheckCurrent(Property, Container); !Reason.IsEmpty())
            {
                Errors.Add({ Property, Move(Reason) });
            }
        }
        return Errors;
    }
}
