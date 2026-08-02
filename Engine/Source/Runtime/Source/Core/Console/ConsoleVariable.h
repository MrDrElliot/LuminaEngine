#pragma once
#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Core/Templates/Optional.h"
#include "Core/Variant/Variant.h"

namespace Lumina
{
    
    using CVarValueType = TVariant<int32, float, bool, FStringView>;

    template<typename T, typename Variant>
    struct IsVariantMember;
    
    template<typename T, typename... Types>
    struct IsVariantMember<T, TVariant<Types...>>  : eastl::disjunction<eastl::is_same<T, Types>...> {};
    
    template<typename T>
    concept ValidConsoleVarType = IsVariantMember<T, CVarValueType>::value;

    namespace ConsoleHelpers
    {
        template<typename T>
        requires(eastl::is_same_v<T, int32>)
        TOptional<T> ParseValue(FStringView Str)
        {
            char* End = nullptr;
            long long Value = std::strtoll(Str.data(), &End, 10);
            if (End != Str.data() && Value >= eastl::numeric_limits<T>::min() && Value <= eastl::numeric_limits<T>::max())
            {
                return static_cast<T>(Value);
            }
            return eastl::nullopt;
        }

        template<typename T>
        requires(eastl::is_floating_point_v<T>)
        TOptional<T> ParseValue(FStringView Str)
        {
            char* End = nullptr;
            T Value = static_cast<T>(std::strtod(Str.data(), &End));
            if (End != Str.data())
            {
                return Value;
            }
            return eastl::nullopt;
        }

        template<typename T>
        requires(eastl::is_same_v<T, bool>)
        TOptional<T> ParseValue(FStringView Str)
        {
            if (Str == "true" || Str == "1" || Str == "True" || Str == "TRUE")
            {
                return true;
            }
            if (Str == "false" || Str == "0" || Str == "False" || Str == "FALSE")
            {
                return false;
            }
            return eastl::nullopt;
        }

        template<typename T>
        requires(eastl::is_same_v<T, FStringView> || eastl::is_convertible_v<T, FStringView>)
        TOptional<T> ParseValue(FStringView Str)
        {
            return Str;
        }
    }

    struct RUNTIME_API FConsoleVariable
    {
        FStringView Name;
        FStringView Hint;
        CVarValueType* ValuePtr;
        CVarValueType DefaultValue;
        void (*OnChange)(const CVarValueType&);

        constexpr FConsoleVariable(FStringView InName, FStringView InHint, CVarValueType* InPtr, const CVarValueType& InDefault, void (*InCallback)(const CVarValueType&) = nullptr)
            : Name(InName)
            , Hint(InHint)
            , ValuePtr(InPtr)
            , DefaultValue(InDefault)
            , OnChange(InCallback)
        {}
    };

    struct RUNTIME_API FConsoleCommand
    {
        FStringView Name;
        FStringView Hint;
        void (*Execute)();

        constexpr FConsoleCommand(FStringView InName, FStringView InHint, void(*InExec)())
            : Name(InName)
            , Hint(InHint)
            , Execute(InExec)
        {}
    };

    class RUNTIME_API FConsoleRegistry
    {
    public:

        using FConsoleContainer = TFixedHashMap<FStringView, FConsoleVariable, 100>;
        using FCommandContainer = TFixedHashMap<FStringView, FConsoleCommand, 100>;


        static FConsoleRegistry& Get() noexcept;


        void Register(FConsoleVariable&& Var) noexcept;
        void RegisterCommand(FConsoleCommand&& Cmd) noexcept;

        FConsoleVariable* Find(FStringView Name);
        FConsoleCommand* FindCommand(FStringView Name);

        template<ValidConsoleVarType T>
        const T& GetAs(FStringView Name)
        {
			return eastl::get<T>(*Find(Name)->ValuePtr);
        }
        
        template<ValidConsoleVarType T>
        const T* TryGetAs(FStringView Name)
        {
			return eastl::get_if<T>(Find(Name)->ValuePtr);
        }
        
        template<ValidConsoleVarType T>
        decltype(auto) SetAs(FStringView Name, const T& Value)
        {
            FConsoleVariable* ConsoleVar = Find(Name);
            return ConsoleVar->ValuePtr->emplace<T>(Value);
        }

        const FConsoleContainer& GetAll() const;
        const FCommandContainer& GetAllCommands() const;

        bool SetValueFromString(FStringView TargetName, FStringView StrValue);
        TOptional<FString> GetValueAsString(FStringView VariableName);

        bool ExecuteCommand(FStringView Name);

        void SaveToConfig();
        void LoadFromConfig();

    private:

        FConsoleContainer ConsoleVariables;
        FCommandContainer ConsoleCommands;
    };


    
    template<ValidConsoleVarType T>
    class TConsoleVar
    {
    public:
        
        static void DefaultCallback(const CVarValueType&) {}
        
        TConsoleVar(FStringView Name, T DefaultValue, FStringView Hint, void(*InCallback)(const CVarValueType&) = DefaultCallback) noexcept
            : Storage(DefaultValue)
        {
            FConsoleVariable Var(Name, Hint, &Storage, Storage, InCallback);
            FConsoleRegistry::Get().Register(Move(Var));
        }


        T GetValue() const
        {
            return eastl::get<T>(Storage);
        }

        T* GetValuePtr() const
        {
            return eastl::get_if<T>(&Storage);
        }

        explicit operator bool() const
        {
            return GetValue();
        }
        
    private:

        CVarValueType Storage;
    };

    class FAutoConsoleCommand
    {
    public:

        FAutoConsoleCommand(FStringView Name, FStringView Hint, void(*Execute)()) noexcept
        {
            FConsoleRegistry::Get().RegisterCommand(FConsoleCommand(Name, Hint, Execute));
        }
    };
}
