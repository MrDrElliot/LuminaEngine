#pragma once

#include "StringHash.h"
#include <unordered_map>
#include "Reflector/Types/ReflectedType.h"

namespace Lumina::Reflection
{
    class FReflectionDatabase
    {
    public:

        FReflectionDatabase() = default;
        ~FReflectionDatabase() = default;

        void AddReflectedType(FReflectedType* Type);

        // Registers a SCRIPT_EXPORT free function under its header (parallel to ReflectedTypes). Takes
        // ownership of Fn.
        void AddFreeFunction(FReflectedHeader* Header, FReflectedFunction* Fn);

        bool IsTypeRegistered(const FStringHash& Str) const;

        bool IsCoreType(const FStringHash& Hash) const;
        
        template<typename T>
        requires(std::is_base_of_v<FReflectedType, T>)
        T* GetOrCreateReflectedType(const FStringHash& TypeName);

        template<typename T>
        requires(std::is_base_of_v<FReflectedType, T>)
        T* GetReflectedTypeChecked(const FStringHash& TypeName) const;

        template<typename T>
        T* GetReflectedType(const FStringHash& TypeName) const;

        
        std::unordered_map<FReflectedHeader*, std::vector<std::unique_ptr<FReflectedType>>>     ReflectedTypes;
        std::unordered_map<FStringHash, FReflectedType*>                                            TypeHashMap;
        std::unordered_map<FReflectedHeader*, std::vector<std::unique_ptr<FReflectedFunction>>> FreeFunctions;

    };


    template <typename T>
    requires(std::is_base_of_v<FReflectedType, T>)
    T* FReflectionDatabase::GetOrCreateReflectedType(const FStringHash& TypeName)
    {
        T* ReturnValue;
        if (IsTypeRegistered(TypeName))
        {
            ReturnValue = static_cast<T*>(TypeHashMap.at(TypeName));
        }
        else
        {
            ReturnValue = new T{};
        }
        ReturnValue->QualifiedName = TypeName.c_str();

        return ReturnValue;
    }

    template <typename T> requires (std::is_base_of_v<FReflectedType, T>)
    T* FReflectionDatabase::GetReflectedTypeChecked(const FStringHash& TypeName) const
    {
        if (!IsTypeRegistered(TypeName))
        {
            std::abort();
        }
        
        return static_cast<T*>(TypeHashMap.at(TypeName));
    }

    template <typename T>
    T* FReflectionDatabase::GetReflectedType(const FStringHash& TypeName) const
    {
        if (TypeHashMap.find(TypeName) == TypeHashMap.end())
        {
            return nullptr;
        }

        return static_cast<T*>(TypeHashMap.at(TypeName));
    }
}
