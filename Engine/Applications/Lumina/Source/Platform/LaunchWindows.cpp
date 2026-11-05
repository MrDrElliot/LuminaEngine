#ifdef _WIN32

#include <Windows.h>
#include <shellapi.h>

#include "Containers/String.h"
#include "Containers/Vector.h"

#pragma comment(lib, "Shell32.lib")

#include "CpuCheck.h"

extern int LuminaMain(int ArgC, char** ArgV);

int WINAPI WinMain(_In_ HINSTANCE hInInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ char* pCmdLine, _In_ int nCmdShow)
{
    if (!LuminaHasRequiredCpu())
    {
        LuminaReportUnsupportedCpu();
        return 1;
    }

    int ArgC = 0;
    LPWSTR* WideArgs = CommandLineToArgvW(GetCommandLineW(), &ArgC);
    if (WideArgs == nullptr)
    {
        return 1;
    }

    Lumina::TVector<Lumina::FString> Utf8Args;
    Utf8Args.Reserve(ArgC);
    for (int Index = 0; Index < ArgC; ++Index)
    {
        const int Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, WideArgs[Index], -1, nullptr, 0, nullptr, nullptr);
        if (Length == 0)
        {
            LocalFree(WideArgs);
            return 1;
        }

        Lumina::FString& Arg = Utf8Args.EmplaceGetRef(Length, '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, WideArgs[Index], -1, Arg.GetData(), Length, nullptr, nullptr) == 0)
        {
            LocalFree(WideArgs);
            return 1;
        }
        Arg.LeftChopInline(1);
    }
    LocalFree(WideArgs);

    Lumina::TVector<char*> ArgV;
    ArgV.Reserve(Utf8Args.Num());
    for (Lumina::FString& Arg : Utf8Args)
    {
        ArgV.Add(Arg.GetData());
    }
    return LuminaMain(ArgC, ArgV.GetData());
}

#endif
