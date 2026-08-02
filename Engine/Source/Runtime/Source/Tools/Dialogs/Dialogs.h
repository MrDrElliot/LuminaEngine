#pragma once
#include "Containers/String.h"

namespace Lumina::Dialogs
{
    enum class RUNTIME_API EType
    {
        Ok = 0,
        OkCancel,
        YesNo,
        YesNoCancel,
        RetryCancel,
        AbortRetryIgnore,
        CancelTryContinue,
    };

    enum class RUNTIME_API EResult
    {
        No,
        Cancel,
        Yes,
        Retry,
        Continue,
    };

    enum class RUNTIME_API ESeverity
    {
        Info = 0,
        Warning,
        Error,
        FatalError,
    };
    
    RUNTIME_API EResult ShowInternal(ESeverity Severity, EType Type, const FString& Title, const FString& Message);

    template <typename... TArgs>
    void Info(const FString& Title, std::format_string<TArgs...> fmt, TArgs&&... Args)
    {
        std::string Msg = std::format(fmt, std::forward<TArgs>(Args)...);
        ShowInternal(ESeverity::Info, EType::Ok, Title, Msg.c_str());
    }

    template <typename... TArgs>
    void Warning(const FString& Title, std::format_string<TArgs...> fmt, TArgs&&... Args)
    {
        std::string Msg = std::format(fmt, std::forward<TArgs>(Args)...);
        ShowInternal(ESeverity::Warning, EType::Ok, Title, Msg.c_str());
    }

    template <typename... TArgs>
    void Error(const FString& Title, std::format_string<TArgs...> fmt, TArgs&&... Args)
    {
        std::string Msg = std::format(fmt, std::forward<TArgs>(Args)...);

        ShowInternal(ESeverity::Error, EType::Ok, Title, Msg.c_str());
    }

    template <typename... TArgs>
    bool Confirmation(const FString& Title, std::format_string<TArgs...> fmt, TArgs&&... Args)
    {
        std::string Msg = std::format(fmt, std::forward<TArgs>(Args)...);

        return ShowInternal(ESeverity::Info, EType::YesNo, Title, Msg.c_str()) == EResult::Yes;
    }

    
}
