#include "RuntimePCH.h"


#if defined(LE_PLATFORM_WINDOWS)
#include "DirectoryWatcher.h"
#include <windows.h>
#include "PlatformFilesystem.h"
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
        WatchThread.RequestStop();
        WatchThread.Join();
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
        
        if (!Filesystem::IsDirectory(Path))
        {
            return false;
        }

        bRunning.store(true, Atomic::MemoryOrderRelaxed);
        WatchThread = FThread(FThreadOptions{ "DirectoryWatcher" }, [this](const FThreadStop& Stop) { WatchThreadFunc(Stop); });
        
        return true;
    }

    void FDirectoryWatcher::WatchThreadFunc(const FThreadStop& Stop)
    {
        HANDLE hDir = CreateFileA(
            *Path,
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
        Buffer.SetNum(BufferSize);
        
        OVERLAPPED Overlapped = {};
        Overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

        bool bReadPending = false;
        while (!Stop.ShouldStop())
        {
            DWORD BytesReturned = 0;

            // One read stays outstanding across timeouts, since a second one on the same buffer and event loses the first's results.
            if (!bReadPending)
            {
                ResetEvent(Overlapped.hEvent);
                if (!ReadDirectoryChangesW(
                    hDir,
                    Buffer.GetData(),
                    BufferSize,
                    bWatchRecursive ? TRUE : FALSE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
                    nullptr,
                    &Overlapped,
                    nullptr))
                {
                    break;
                }
                bReadPending = true;
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

            bReadPending = false;
            if (!GetOverlappedResult(hDir, &Overlapped, &BytesReturned, FALSE))
            {
                break;
            }

            if (BytesReturned == 0)
            {
                continue;
            }

            FILE_NOTIFY_INFORMATION* Info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(Buffer.GetData());
            
            while (true)
            {
                FWString FileName(Info->FileName, Info->FileNameLength / sizeof(WCHAR));
                FString FileNameUtf8 = WideToUtf8(FileName);
                
                FString FullPathStr(*Path, Path.Len());
                FullPathStr.AppendChar('/');
                FullPathStr.Append(FileNameUtf8);
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
        }

        if (bReadPending)
        {
            DWORD Ignored = 0;
            CancelIo(hDir);
            GetOverlappedResult(hDir, &Overlapped, &Ignored, TRUE);
        }
        CloseHandle(Overlapped.hEvent);
        CloseHandle(hDir);
        
        bRunning.store(false, Atomic::MemoryOrderRelaxed);
    }

    FString FDirectoryWatcher::WideToUtf8(const FWString& Wide)
    {
        if (Wide.IsEmpty())
        {
            return FString();
        }
        
        int SizeNeeded = WideCharToMultiByte(CP_UTF8, 0, Wide.GetData(), Wide.Len(), nullptr, 0, nullptr, nullptr);
        FString Result;
        Result.SetLen(SizeNeeded);
        WideCharToMultiByte(CP_UTF8, 0, Wide.GetData(), Wide.Len(), Result.GetData(), SizeNeeded, nullptr, nullptr);
        
        return Result;
    }
}

#endif
