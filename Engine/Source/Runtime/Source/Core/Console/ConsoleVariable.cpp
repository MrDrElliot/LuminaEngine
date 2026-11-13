#include "RuntimePCH.h"
#include "ConsoleVariable.h"
#include "Core/Assertions/Assert.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Containers/StringFormat.h"
#include "Platform/Filesystem/PlatformFilesystem.h"


namespace Lumina
{
    namespace
    {
        bool ApplySavedValue(FConsoleVariable& ConsoleVar, const FJsonValue& Saved)
        {
            return Visit([&]<typename T0>(T0& Value) -> bool
            {
                if constexpr (std::is_same_v<std::decay_t<T0>, FStringView>)
                {
                    return false;
                }
                else
                {
                    return Saved.TryGet(Value);
                }
            }, *ConsoleVar.ValuePtr);
        }
    }

    FConsoleRegistry& FConsoleRegistry::Get() noexcept
    {
        static FConsoleRegistry Instance;
        return Instance;
    }

    void FConsoleRegistry::Register(FConsoleVariable&& Var) noexcept
    {
        FStringView VarName = Var.Name.GetData();
        if (!ConsoleVariables.TryAdd(VarName, Move(Var)) || !LoadedConfig.IsObject())
        {
            return;
        }

        // OnChange is skipped since nothing has read the variable yet and its module may still be running static initializers.
        if (const FJsonValue* Member = LoadedConfig.Find(VarName))
        {
            ApplySavedValue(*Find(VarName), *Member);
        }
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

    FConsoleLineParts FConsoleRegistry::SplitLine(FStringView Line)
    {
        FConsoleLineParts Parts;
        Parts.Line = Line.TrimStartAndEnd();
        const int32 Space = Parts.Line.FindFirstOf(' ');
        Parts.Name = Space == Constants::kIndexNone ? Parts.Line : Parts.Line.Mid(0, Space);
        Parts.Value = Space == Constants::kIndexNone ? FStringView() : Parts.Line.Mid(Space + 1);
        return Parts;
    }

    FConsoleLineResult FConsoleRegistry::ExecuteLine(FStringView InLine, bool bEchoToLog)
    {
        const auto [Line, Name, Value] = SplitLine(InLine);

        FConsoleLineResult Result;
        if (Line.IsEmpty())
        {
            return Result;
        }
        if (bEchoToLog)
        {
            LOG_INFO("> {}", Line);
        }

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
        if (Result.Kind == EConsoleLineResult::SetVariable)
        {
            SaveToConfig();
        }
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
        SaveToFile(GetConfigFilePath());
    }

    void FConsoleRegistry::LoadFromConfig()
    {
        LoadFromFile(GetConfigFilePath());
    }

    FString FConsoleRegistry::GetConfigFilePath()
    {
        return Paths::GetUserDataDirectory() + "/Lumina/ConsoleVariables.json";
    }

    bool FConsoleRegistry::SaveToFile(FStringView Path)
    {
        FJsonValue Saved;
        FString Text;
        if (!Filesystem::Exists(Path) || !Filesystem::ReadFile(Text, Path) || !FJsonValue::Parse(Text.View(), Saved) || !Saved.IsObject())
        {
            Saved = FJsonValue::MakeObject();
        }

        for (const auto& [Name, Var] : ConsoleVariables)
        {
            // String variables hold a view into storage they do not own, so they are not persisted.
            if (*Var.ValuePtr == Var.DefaultValue || Containers::GetIf<FStringView>(Var.ValuePtr) != nullptr)
            {
                Saved.Remove(Name);
                continue;
            }

            Visit([&]<typename T0>(const T0& Value)
            {
                if constexpr (!std::is_same_v<std::decay_t<T0>, FStringView>)
                {
                    Saved.Set(Name, FJsonValue(Value));
                }
            }, *Var.ValuePtr);
        }

        Text = Saved.ToString(4);
        if (!Filesystem::MakeParentDirectoryTree(Path)
            || !Filesystem::WriteFile(Path, TSpan<const uint8>(reinterpret_cast<const uint8*>(Text.GetData()), Text.Len())))
        {
            LOG_ERROR("Console variables could not be written to {}.", Path);
            return false;
        }
        return true;
    }

    bool FConsoleRegistry::LoadFromFile(FStringView Path)
    {
        FString Text;
        if (!Filesystem::Exists(Path) || !Filesystem::ReadFile(Text, Path))
        {
            return false;
        }

        FJsonValue Saved;
        FJsonParseError Error;
        if (!FJsonValue::Parse(Text.View(), Saved, &Error) || !Saved.IsObject())
        {
            LOG_WARN("Console variables at {} could not be read ({}); keeping the defaults.", Path, Error.Message);
            return false;
        }

        for (const FJsonMember& Member : Saved.Members())
        {
            FConsoleVariable* ConsoleVar = Find(Member.Key.View());
            if (ConsoleVar != nullptr && ApplySavedValue(*ConsoleVar, Member.Value) && ConsoleVar->OnChange != nullptr)
            {
                ConsoleVar->OnChange(*ConsoleVar->ValuePtr);
            }
        }

        // Plugin modules register their variables after this, so their saved values wait here for Register.
        LoadedConfig = Move(Saved);
        return true;
    }
}
