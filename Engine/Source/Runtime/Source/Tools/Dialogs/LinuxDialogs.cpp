#include "RuntimePCH.h"
#ifdef LE_PLATFORM_LINUX

#include "Dialogs.h"
#include "Log/Log.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Process/PlatformProcess.h"

namespace Lumina::Dialogs
{
    namespace
    {
        constexpr int kAffirmative = 0;

        const char* SeverityIcon(ESeverity Severity)
        {
            switch (Severity)
            {
            case ESeverity::Warning:    return "warning";
            case ESeverity::Error:
            case ESeverity::FatalError: return "error";
            case ESeverity::Info:
            default:                    return "info";
            }
        }

        FString Quote(FStringView Value)
        {
            FString Result;
            Result.reserve(Value.size() + 2);
            Result.push_back('"');

            for (const char Character : Value)
            {
                if (Character == '"' || Character == '\\')
                {
                    Result.push_back('\\');
                }

                Result.push_back(Character);
            }

            Result.push_back('"');

            return Result;
        }
    }

    EResult ShowInternal(ESeverity Severity, EType Type, const FString& Title, const FString& Message)
    {
        FString Params;

        const bool bAsksAQuestion = Type != EType::Ok;

        if (bAsksAQuestion)
        {
            Params += "--question";
            Params += " --icon-name=";
            Params += SeverityIcon(Severity);
        }
        else
        {
            switch (Severity)
            {
            case ESeverity::Warning:    Params += "--warning"; break;
            case ESeverity::Error:
            case ESeverity::FatalError: Params += "--error";   break;
            case ESeverity::Info:
            default:                    Params += "--info";    break;
            }
        }

        Params += " --title=";
        Params += Quote(Title);
        Params += " --text=";
        Params += Quote(Message);

        switch (Type)
        {
        case EType::YesNo:
        case EType::YesNoCancel:
            Params += " --ok-label=Yes --cancel-label=No";
            break;

        case EType::RetryCancel:
        case EType::AbortRetryIgnore:
            Params += " --ok-label=Retry --cancel-label=Cancel";
            break;

        case EType::CancelTryContinue:
            Params += " --ok-label=Continue --cancel-label=Cancel";
            break;

        default:
            break;
        }

        const int ExitCode = Platform::RunProcessAndWait("/usr/bin/zenity", Params.c_str(), nullptr);

        if (ExitCode < 0)
        {
            LOG_WARN("Dialog '{0}': {1}", Title, Message);
            LOG_WARN("(zenity is not installed, so this could not be shown as a dialog.)");

            return bAsksAQuestion ? EResult::Cancel : EResult::Yes;
        }

        if (!bAsksAQuestion)
        {
            return EResult::Yes;
        }

        switch (Type)
        {
        case EType::RetryCancel:
        case EType::AbortRetryIgnore:
            return ExitCode == kAffirmative ? EResult::Retry : EResult::Cancel;

        case EType::CancelTryContinue:
            return ExitCode == kAffirmative ? EResult::Continue : EResult::Cancel;

        case EType::YesNo:
        case EType::YesNoCancel:
        default:
            return ExitCode == kAffirmative ? EResult::Yes : EResult::No;
        }
    }
}

#endif
