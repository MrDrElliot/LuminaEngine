#include "RuntimePCH.h"
#include "ConsoleVariable.h"
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
        FStringView VarName = Var.Name.GetData();
        ConsoleVariables.TryAdd(VarName, Move(Var));
    }

    void FConsoleRegistry::RegisterCommand(FConsoleCommand&& Cmd) noexcept
    {
        FStringView CmdName = Cmd.Name.GetData();
        ConsoleCommands.TryAdd(CmdName, Move(Cmd));
    }

    FConsoleVariable* FConsoleRegistry::Find(FStringView Name)
    {
        auto It = ConsoleVariables.Find(Name);
        return It != nullptr ? &(*It) : nullptr;
    }

    FConsoleCommand* FConsoleRegistry::FindCommand(FStringView Name)
    {
        auto It = ConsoleCommands.Find(Name);
        return It != nullptr ? &(*It) : nullptr;
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

    FConsoleLineResult FConsoleRegistry::ExecuteLine(FStringView Line, bool bEchoToLog)
    {
        while (!Line.IsEmpty() && (Line[0] == ' ' || Line[0] == '\t'))
        {
            Line.RemovePrefix(1);
        }
        while (!Line.IsEmpty() && (Line.Last() == ' ' || Line.Last() == '\t'))
        {
            Line.RemoveSuffix(1);
        }

        FConsoleLineResult Result;
        if (Line.IsEmpty())
        {
            return Result;
        }
        if (bEchoToLog)
        {
            LOG_INFO("> {}", Line);
        }

        const int32 Space = Line.FindFirstOf(' ');
        const FStringView Name = Space == Constants::kIndexNone ? Line : Line.Mid(0, Space);
        const FStringView Value = Space == Constants::kIndexNone ? FStringView() : Line.Mid(Space + 1);
        Result.Name = FStringView(Name.GetData(), Name.Len());

        if (FindCommand(Name) != nullptr)
        {
            if (bEchoToLog && !Value.IsEmpty())
            {
                LOG_WARN("'{}' is a command and takes no arguments; ignoring '{}'", Name, Value);
            }
            ExecuteCommand(Name);
            Result.Kind = EConsoleLineResult::RanCommand;
            return Result;
        }

        if (Find(Name) == nullptr)
        {
            if (bEchoToLog)
            {
                LOG_WARN("Unknown console command or variable: '{}'", Name);
            }
            Result.Kind = EConsoleLineResult::Unknown;
            return Result;
        }

        if (!Value.IsEmpty() && !SetValueFromString(Name, Value))
        {
            if (bEchoToLog)
            {
                LOG_WARN("Failed to set '{}' to '{}'", Name, Value);
            }
            Result.Kind = EConsoleLineResult::RejectedValue;
            Result.Value = FStringView(Value.GetData(), Value.Len());
            return Result;
        }

        const TOptional<FString> Current = GetValueAsString(Name);
        Result.Value = Current.IsSet() ? *Current : FString("<unprintable>");
        Result.Kind = Value.IsEmpty() ? EConsoleLineResult::ReadVariable : EConsoleLineResult::SetVariable;
        if (bEchoToLog)
        {
            LOG_INFO("{} = {}", Name, Result.Value);
        }
        return Result;
    }

    bool FConsoleRegistry::SetValueFromString(FStringView TargetName, FStringView StrValue)
    {
        FConsoleVariable* ConsoleVar = Find(TargetName);
        if (ConsoleVar == nullptr)
        {
            return false;
        }

        bool bSuccess = Visit([&]<typename T0>(T0&&) -> bool
        {
            using T = std::decay_t<T0>;
            
            TOptional<T> ParsedValue = ConsoleHelpers::ParseValue<T>(StrValue);
            if (ParsedValue.IsSet())
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

        bool bSuccess = Visit([&]<typename T0>(T0&& Value) -> bool
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
