#include "RuntimePCH.h"


#if defined(LE_PLATFORM_WINDOWS)
#include "DirectoryWatcher.h"
#include <windows.h>
#include <filesystem>
#include "Core/Templates/LuminaTemplate.h"
#include "Paths/Paths.h"

namespace Lumina
{
    FDirectoryWatcher::FDirectoryWatcher()
        : Callback()
    {
    }

    FDirectoryWatcher::~FDirectoryWatcher()
    {
        Stop();
    }

    bool FDirectoryWatcher::Stop()
    {
        if (bRunning.load(Atomic::MemoryOrderRelaxed))
        {
            bRunning.store(false, Atomic::MemoryOrderRelaxed);
            if (WatchThread.joinable())
            {
                WatchThread.join();
                return true;
            }
        }

        return true;
    }

    bool FDirectoryWatcher::Watch(const FFixedString& InPath, FFileEventCallback InCallback, bool bRecursive)
    {
        if (bRunning.load(Atomic::MemoryOrderRelaxed))
        {
            return false;
        }

        Path = InPath;
        Callback = Move(InCallback);
        bWatchRecursive = bRecursive;
        
        std::filesystem::path FSPath(Path.c_str());
        if (!std::filesystem::exists(FSPath) || !std::filesystem::is_directory(FSPath))
        {
            return false;
        }

        bRunning.store(true, Atomic::MemoryOrderRelaxed);
        WatchThread = FThread([this]() { WatchThreadFunc(); });
        
        return true;
    }

    void FDirectoryWatcher::WatchThreadFunc()
    {
        HANDLE hDir = CreateFileA(
            Path.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr
        );

        if (hDir == INVALID_HANDLE_VALUE)
        {
            bRunning.store(false, Atomic::MemoryOrderRelaxed);
            return;
        }

        constexpr DWORD BufferSize = 64 * 1024;
        TVector<BYTE> Buffer;
        Buffer.resize(BufferSize);
        
        OVERLAPPED Overlapped = {};
        Overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

        while (bRunning.load(Atomic::MemoryOrderRelaxed))
        {
            DWORD BytesReturned = 0;
            BOOL Success = ReadDirectoryChangesW(
                hDir,
                Buffer.data(),
                BufferSize,
                bWatchRecursive ? TRUE : FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | 
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
                &BytesReturned,
                &Overlapped,
                nullptr
            );

            if (!Success)
            {
                break;
            }

            DWORD WaitResult = WaitForSingleObject(Overlapped.hEvent, 100);
            if (WaitResult == WAIT_TIMEOUT)
            {
                continue;
            }
            else if (WaitResult != WAIT_OBJECT_0)
            {
                break;
            }

            if (!GetOverlappedResult(hDir, &Overlapped, &BytesReturned, FALSE))
            {
                break;
            }

            if (BytesReturned == 0)
            {
                continue;
            }

            FILE_NOTIFY_INFORMATION* Info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(Buffer.data());
            
            while (true)
            {
                FWString FileName(Info->FileName, Info->FileNameLength / sizeof(WCHAR));
                FString FileNameUtf8 = WideToUtf8(FileName);
                
                std::filesystem::path FullPath = std::filesystem::path(Path.c_str()) / FileNameUtf8.c_str();
                FString FullPathStr = FullPath.string().c_str();
                Paths::Normalize(FullPathStr);
                FFileEvent Event;
                Event.Path = FullPathStr;
                

                switch (Info->Action)
                {
                    case FILE_ACTION_ADDED:
                        Event.Action = EFileAction::Added;
                        break;
                    case FILE_ACTION_REMOVED:
                        Event.Action = EFileAction::Removed;
                        break;
                    case FILE_ACTION_MODIFIED:
                        Event.Action = EFileAction::Modified;
                        break;
                    case FILE_ACTION_RENAMED_OLD_NAME:
                        Event.Action = EFileAction::Renamed;
                        Event.OldPath = Move(FullPathStr);
                        break;
                    case FILE_ACTION_RENAMED_NEW_NAME:
                        Event.Action = EFileAction::Renamed;
                        break;
                }

                if (Callback)
                {
                    Callback(Event);
                }

                if (Info->NextEntryOffset == 0)
                {
                    break;
                }

                Info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<BYTE*>(Info) + Info->NextEntryOffset);
            }

            ResetEvent(Overlapped.hEvent);
        }

        CloseHandle(Overlapped.hEvent);
        CloseHandle(hDir);
        
        bRunning.store(false, Atomic::MemoryOrderRelaxed);
    }

    FString FDirectoryWatcher::WideToUtf8(const FWString& Wide)
    {
        if (Wide.empty())
        {
            return FString();
        }
        
        int SizeNeeded = WideCharToMultiByte(CP_UTF8, 0, Wide.data(), (int)Wide.size(), nullptr, 0, nullptr, nullptr);
        FString Result;
        Result.resize(SizeNeeded);
        WideCharToMultiByte(CP_UTF8, 0, Wide.data(), (int)Wide.size(), Result.data(), SizeNeeded, nullptr, nullptr);
        
        return Result;
    }
}

#endif