#include "RuntimePCH.h"

#if defined(LE_PLATFORM_LINUX)
#include "DirectoryWatcher.h"

#include <cerrno>
#include <cstring>
#include "PlatformFilesystem.h"
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Log/Log.h"
#include "Paths/Paths.h"

namespace Lumina
{
    namespace
    {
        using FWatchPaths = THashMap<int, FString>;

        constexpr uint32 kWatchMask =
            IN_CREATE                    // a file or directory appeared
            | IN_DELETE                  // ... or was removed
            | IN_CLOSE_WRITE             // a writable handle was closed, meaning saved
            | IN_MOVED_FROM              // the two halves of a rename, paired by cookie
            | IN_MOVED_TO
            | IN_DELETE_SELF             // the watched directory itself went away
            | IN_MOVE_SELF;

        bool AddWatch(int Notify, const FString& Directory, FWatchPaths& OutWatchPaths)
        {
            const int Descriptor = ::inotify_add_watch(Notify, Directory.c_str(), kWatchMask);

            if (Descriptor < 0)
            {
                LOG_WARN("DirectoryWatcher: cannot watch '{0}': {1}", Directory, ::strerror(errno));
                return false;
            }

            OutWatchPaths[Descriptor] = Directory;

            return true;
        }

        void ReportExistingEntries(const FString& Root, const FFileEventCallback& Callback)
        {
            if (!Callback)
            {
                return;
            }

            Filesystem::IterateDirectoryRecursive(Root, [&Callback](const Filesystem::FDirectoryEntry& Entry)
            {
                FFileEvent Event;
                Event.Path.assign(Entry.FullPath.data(), Entry.FullPath.size());
                Event.Action = EFileAction::Added;
                Paths::Normalize(Event.Path);

                Callback(Event);
            });
        }

        void AddWatchTree(int Notify, const FString& Root, bool bRecursive, FWatchPaths& OutWatchPaths)
        {
            if (!AddWatch(Notify, Root, OutWatchPaths) || !bRecursive)
            {
                return;
            }

            const bool bWalked = Filesystem::IterateDirectoryRecursive(Root,
                [Notify, &OutWatchPaths](const Filesystem::FDirectoryEntry& Entry)
                {
                    if (!Entry.IsDirectory())
                    {
                        return;
                    }

                    FString Child(Entry.FullPath.data(), Entry.FullPath.size());
                    Paths::Normalize(Child);
                    AddWatch(Notify, Child, OutWatchPaths);
                });

            if (!bWalked)
            {
                LOG_WARN("DirectoryWatcher: cannot enumerate '{0}': {1}", Root,
                    Filesystem::ToString(Filesystem::GetLastResult()));
            }
        }
    }

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

        if (!Filesystem::IsDirectory(Path))
        {
            return false;
        }

        bRunning.store(true, Atomic::MemoryOrderRelaxed);
        WatchThread = FThread([this]() { WatchThreadFunc(); });

        return true;
    }

    void FDirectoryWatcher::WatchThreadFunc()
    {
        const int Notify = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

        if (Notify < 0)
        {
            LOG_ERROR("DirectoryWatcher: inotify_init1 failed: {0}", ::strerror(errno));
            bRunning.store(false, Atomic::MemoryOrderRelaxed);
            return;
        }

        FString Root(Path.c_str());
        Paths::Normalize(Root);

        FWatchPaths WatchPaths;
        AddWatchTree(Notify, Root, bWatchRecursive, WatchPaths);

        if (WatchPaths.empty())
        {
            ::close(Notify);
            bRunning.store(false, Atomic::MemoryOrderRelaxed);
            return;
        }

        alignas(alignof(struct inotify_event)) char Buffer[64 * 1024];

        struct FPendingMove
        {
            uint32  Cookie = 0;
            FString OldPath;
        };

        TVector<FPendingMove> PendingMoves;

        while (bRunning.load(Atomic::MemoryOrderRelaxed))
        {
            pollfd Poll{};
            Poll.fd = Notify;
            Poll.events = POLLIN;

            const int Ready = ::poll(&Poll, 1, 100);

            if (Ready < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                LOG_ERROR("DirectoryWatcher: poll failed: {0}", ::strerror(errno));
                break;
            }

            if (Ready == 0)
            {
                continue;
            }

            const ssize_t Read = ::read(Notify, Buffer, sizeof(Buffer));

            if (Read <= 0)
            {
                if (Read < 0 && (errno == EAGAIN || errno == EINTR))
                {
                    continue;
                }

                break;
            }

            PendingMoves.clear();

            for (ssize_t Offset = 0; Offset < Read; )
            {
                const inotify_event* Event = reinterpret_cast<const inotify_event*>(Buffer + Offset);
                Offset += static_cast<ssize_t>(sizeof(inotify_event)) + Event->len;

                if (Event->mask & IN_Q_OVERFLOW)
                {
                    LOG_WARN("DirectoryWatcher: the kernel event queue overflowed; some changes to "
                             "'{0}' were lost.", Root);
                    continue;
                }

                if (Event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF))
                {
                    WatchPaths.erase(Event->wd);
                    continue;
                }

                const auto Found = WatchPaths.find(Event->wd);

                if (Found == WatchPaths.end() || Event->len == 0)
                {
                    continue;
                }

                FString FullPath = Found->second;
                FullPath += '/';
                FullPath += Event->name;
                Paths::Normalize(FullPath);

                const bool bIsDirectory = (Event->mask & IN_ISDIR) != 0;

                const bool bIsNewDirectory = bIsDirectory
                    && bWatchRecursive
                    && (Event->mask & (IN_CREATE | IN_MOVED_TO)) != 0;

                if (bIsNewDirectory)
                {
                    AddWatchTree(Notify, FullPath, true, WatchPaths);
                }

                if (Event->mask & IN_MOVED_FROM)
                {
                    PendingMoves.push_back(FPendingMove{ Event->cookie, FullPath });
                    continue;
                }

                FFileEvent Out;
                Out.Path = FullPath;

                if (Event->mask & IN_MOVED_TO)
                {
                    Out.Action = EFileAction::Added;

                    for (size_t Index = 0; Index < PendingMoves.size(); ++Index)
                    {
                        if (PendingMoves[Index].Cookie == Event->cookie)
                        {
                            Out.Action = EFileAction::Renamed;
                            Out.OldPath = Move(PendingMoves[Index].OldPath);
                            PendingMoves.erase(PendingMoves.begin() + static_cast<ptrdiff_t>(Index));
                            break;
                        }
                    }
                }
                else if (Event->mask & IN_CREATE)
                {
                    Out.Action = EFileAction::Added;
                }
                else if (Event->mask & IN_DELETE)
                {
                    Out.Action = EFileAction::Removed;
                }
                else if (Event->mask & IN_CLOSE_WRITE)
                {
                    Out.Action = EFileAction::Modified;
                }
                else
                {
                    continue;
                }

                if (Callback)
                {
                    Callback(Out);
                }

                if (bIsNewDirectory)
                {
                    ReportExistingEntries(FullPath, Callback);
                }
            }

            for (FPendingMove& Pending : PendingMoves)
            {
                FFileEvent Out;
                Out.Path = Move(Pending.OldPath);
                Out.Action = EFileAction::Removed;

                if (Callback)
                {
                    Callback(Out);
                }
            }

            PendingMoves.clear();
        }

        ::close(Notify);

        bRunning.store(false, Atomic::MemoryOrderRelaxed);
    }
}

#endif
