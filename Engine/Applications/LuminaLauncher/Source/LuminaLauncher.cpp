#ifdef _WIN32

#include <Windows.h>

#include <string>

namespace
{
    std::wstring DirectoryOf(const std::wstring& Path)
    {
        const size_t Slash = Path.find_last_of(L"\\/");
        return Slash == std::wstring::npos ? std::wstring() : Path.substr(0, Slash);
    }

    std::wstring GetOwnDirectory()
    {
        std::wstring Buffer(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD Length = GetModuleFileNameW(nullptr, Buffer.data(), (DWORD)Buffer.size());
            if (Length < Buffer.size())
            {
                Buffer.resize(Length);
                return DirectoryOf(Buffer);
            }
            Buffer.resize(Buffer.size() * 2);
        }
    }

    // A package carries one editor configuration, so whichever one is present is the one to start.
    std::wstring FindEditor(const std::wstring& BinariesDirectory)
    {
        WIN32_FIND_DATAW Found;
        const HANDLE Search = FindFirstFileW((BinariesDirectory + L"\\Lumina-Editor-*.exe").c_str(), &Found);
        if (Search == INVALID_HANDLE_VALUE)
        {
            return std::wstring();
        }
        FindClose(Search);
        return BinariesDirectory + L"\\" + Found.cFileName;
    }

    // Everything after the launcher's own name, so a double-clicked .lproject or --Project reaches the editor.
    const wchar_t* ForwardedArguments()
    {
        const wchar_t* Cursor = GetCommandLineW();
        const bool bQuoted = *Cursor == L'"';
        if (bQuoted)
        {
            ++Cursor;
        }
        while (*Cursor != L'\0' && (bQuoted ? *Cursor != L'"' : *Cursor != L' ' && *Cursor != L'\t'))
        {
            ++Cursor;
        }
        if (bQuoted && *Cursor == L'"')
        {
            ++Cursor;
        }
        while (*Cursor == L' ' || *Cursor == L'\t')
        {
            ++Cursor;
        }
        return Cursor;
    }

    void ShowError(const std::wstring& Message)
    {
        MessageBoxW(nullptr, Message.c_str(), L"Lumina", MB_OK | MB_ICONERROR);
    }
}

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ wchar_t*, _In_ int)
{
    const std::wstring BinariesDirectory = GetOwnDirectory() + L"\\Binaries\\Windows64";
    const std::wstring Editor = FindEditor(BinariesDirectory);
    if (Editor.empty())
    {
        ShowError(L"The Lumina editor was not found in\n" + BinariesDirectory
            + L"\n\nExtract the whole package and run this launcher from the extracted folder.");
        return 1;
    }

    // CreateProcessW may write into the command line, so it gets its own mutable copy.
    std::wstring CommandLine = L"\"" + Editor + L"\" " + ForwardedArguments();

    STARTUPINFOW Startup{};
    Startup.cb = sizeof(Startup);
    PROCESS_INFORMATION Process{};

    if (!CreateProcessW(Editor.c_str(), CommandLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
        BinariesDirectory.c_str(), &Startup, &Process))
    {
        ShowError(L"The Lumina editor could not be started.\n" + Editor);
        return 1;
    }

    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
    return 0;
}

#endif
