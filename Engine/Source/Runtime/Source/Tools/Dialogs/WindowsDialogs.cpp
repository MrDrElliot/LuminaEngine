#include "RuntimePCH.h"
#ifdef LE_PLATFORM_WINDOWS
#include <windows.h>
#include "Dialogs.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::Dialogs
{
    EResult ShowInternal(ESeverity Severity, EType Type, const FString& Title, const FString& Message)
    {
        UINT Style = MB_TOPMOST | MB_SYSTEMMODAL;

        switch (Severity)
        {
            case ESeverity::Info:
            {
                if (Type == EType::Ok)
                {
                    Style |= MB_ICONINFORMATION;
                }
                else
                {
                    Style |= MB_ICONQUESTION;
                }
            }
            break;

            case ESeverity::Warning:
            {
                Style |= MB_ICONWARNING;
            }
            break;

            case ESeverity::Error:
            case ESeverity::FatalError:
            {
                Style |= MB_ICONERROR;
            }
            break;
        }

        switch (Type)
        {
            case EType::Ok:                 Style |= MB_OK; break;
            case EType::OkCancel:           Style |= MB_OKCANCEL; break;
            case EType::YesNo:              Style |= MB_YESNO; break;
            case EType::YesNoCancel:        Style |= MB_YESNOCANCEL; break;
            case EType::RetryCancel:        Style |= MB_RETRYCANCEL; break;
            case EType::AbortRetryIgnore:   Style |= MB_ABORTRETRYIGNORE; break;
            case EType::CancelTryContinue:  Style |= MB_CANCELTRYCONTINUE; break;
        }

        EResult Result = EResult::Yes;
        MessageBeep(MB_ICONEXCLAMATION);
        int32 ResultCode = MessageBoxA(nullptr, Message.c_str(), Title.c_str(), Style);
        switch(ResultCode)
        {
            case IDOK:
            case IDYES:
            {
                Result = EResult::Yes;
            }
            break;

            case IDNO:
            {
                Result = EResult::No;
            }
            break;

            case IDCANCEL:
            case IDABORT:
            {
                Result = EResult::Cancel;
            }
            break;

            case IDRETRY:
            case IDTRYAGAIN:
            {
                Result = EResult::Retry;
            }
            break;

            case IDIGNORE:
            case IDCONTINUE:
            {
                Result = EResult::Continue;
            }
            break;

            case IDCLOSE:
            {
                Result = EResult::Cancel;
            }
            break;

            default:
            {
                Result = EResult::Yes;
            }
            break;
        }

        return Result;
    }
}

#endif