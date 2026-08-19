#include "ReflectionDatabase.h"

namespace Lumina::Reflection
{
    void FReflectionDatabase::AddReflectedType(FReflectedType* Type)
    {
        if(Type == nullptr || Type->DisplayName.empty())
        {
            return;
        }

        FStringHash NameHash = FStringHash(Type->QualifiedName);

        if (IsTypeRegistered(NameHash))
        {
            return;
        }
        
        auto& TypeVector = ReflectedTypes[Type->Header];
        
        std::unique_ptr<FReflectedType> UniquePtr(Type);
        
        TypeVector.push_back(std::move(UniquePtr));
        
        TypeHashMap.insert_or_assign(NameHash, Type);
    }

    void FReflectionDatabase::AddFreeFunction(FReflectedHeader* Header, FReflectedFunction* Fn)
    {
        if (Header == nullptr || Fn == nullptr)
        {
            return;
        }
        FreeFunctions[Header].push_back(std::unique_ptr<FReflectedFunction>(Fn));
    }

    bool FReflectionDatabase::IsTypeRegistered(const FStringHash& Str) const
    {
        return TypeHashMap.find(Str) != TypeHashMap.end() || IsCoreType(Str);
    }

    bool FReflectionDatabase::IsCoreType(const FStringHash& Hash) const
    {
        EPropertyTypeFlags Flags = GetCoreTypeFromName(Hash.c_str());

        return Flags != EPropertyTypeFlags::None;
    }
}
