#include "RuntimePCH.h"
#include "ConsoleVariable.h"
#include <nlohmann/json.hpp>
#include "Core/Assertions/Assert.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Containers/StringFormat.h"


namespace Lumina
{
    FConsoleRegistry& FConsoleRegistry::Get() noexcept
    {
        static FConsoleRegistry Instance;
        return Instance;
    }

    void FConsoleRegistry::Register(FConsoleVariable&& Var) noexcept
    {
        FStringView VarName = Var.Name.data();
        ConsoleVariables.emplace(VarName, Move(Var));
    }

    void FConsoleRegistry::RegisterCommand(FConsoleCommand&& Cmd) noexcept
    {
        FStringView CmdName = Cmd.Name.data();
        ConsoleCommands.emplace(CmdName, Move(Cmd));
    }

    FConsoleVariable* FConsoleRegistry::Find(FStringView Name)
    {
        auto It = ConsoleVariables.find(Name);
        return It != ConsoleVariables.end() ? &It->second : nullptr;
    }

    FConsoleCommand* FConsoleRegistry::FindCommand(FStringView Name)
    {
        auto It = ConsoleCommands.find(Name);
        return It != ConsoleCommands.end() ? &It->second : nullptr;
    }

    const FConsoleRegistry::FConsoleContainer& FConsoleRegistry::GetAll() const
    {
        return ConsoleVariables;
    }

    const FConsoleRegistry::FCommandContainer& FConsoleRegistry::GetAllCommands() const
    {
        return ConsoleCommands;
    }

    bool FConsoleRegistry::ExecuteCommand(FStringView Name)
    {
        FConsoleCommand* Cmd = FindCommand(Name);
        if (Cmd == nullptr || Cmd->Execute == nullptr)
        {
            return false;
        }

        Cmd->Execute();
        return true;
    }

    bool FConsoleRegistry::SetValueFromString(FStringView TargetName, FStringView StrValue)
    {
        FConsoleVariable* ConsoleVar = Find(TargetName);
        if (ConsoleVar == nullptr)
        {
            return false;
        }

        bool bSuccess = visit([&]<typename T0>(T0&&) -> bool
        {
            using T = std::decay_t<T0>;
            
            TOptional<T> ParsedValue = ConsoleHelpers::ParseValue<T>(StrValue);
            if (ParsedValue.has_value())
            {
                *(ConsoleVar->ValuePtr) = *ParsedValue;
                return true;
            }
            
            return false;
        }, *(ConsoleVar->ValuePtr));

        if (bSuccess && ConsoleVar->OnChange)
        {
            ConsoleVar->OnChange(*(ConsoleVar->ValuePtr));
            SaveToConfig();
        }

        return bSuccess;
    }
    
    TOptional<FString> FConsoleRegistry::GetValueAsString(FStringView VariableName)
    {
        FConsoleVariable* ConsoleVar = Find(VariableName);
        if (ConsoleVar == nullptr)
        {
            return NullOpt;
        }

        FString Result;

        bool bSuccess = visit([&]<typename T0>(T0&& Value) -> bool
        {
            using T = std::decay_t<T0>;

            if constexpr (std::is_same_v<T, int>)
            {
                Result = FString(Format("{}", Value));
            }
            else if constexpr (std::is_same_v<T, float>)
            {
                Result = FString(Format("{}", Value));
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                Result = FString(Format("{}", Value));
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                Result = Value ? "true" : "false";
            }
            else if constexpr (std::is_same_v<T, FString>)
            {
                Result = Value;
            }
            else
            {
                // Unsupported type
                return false;
            }

            return true;
        }, *(ConsoleVar->ValuePtr));

        if (!bSuccess)
        {
            return NullOpt;
        }

        return Result;
    }

    void FConsoleRegistry::SaveToConfig()
    {
        // TODO serialize console variables to the config directory.
    }

    void FConsoleRegistry::LoadFromConfig()
    {
        // TODO restore console variables from the config directory.
    }
}
