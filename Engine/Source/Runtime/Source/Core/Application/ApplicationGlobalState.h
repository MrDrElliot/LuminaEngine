#pragma once
namespace Lumina
{
    struct RUNTIME_API FApplicationGlobalState
    {
        FApplicationGlobalState(char const* MainThreadName = nullptr);
        ~FApplicationGlobalState();
    };
}