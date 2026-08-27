#include "RuntimePCH.h"
#ifdef LE_PLATFORM_LINUX

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Platform/Process/PlatformProcess.h"
#include "Platform/Process/PlatformPty.h"

namespace Lumina::Platform
{
    namespace
    {
        class FLinuxPtySession final : public IPtySession
        {
        public:

            ~FLinuxPtySession() override { Close(); }

            bool Start(const FPtyLaunchParams& Params);

            bool IsRunning() override;
            int32 Read(TVector<uint8>& OutBytes, int32 MaxBytes) override;
            bool Write(const uint8* Bytes, int32 Count) override;
            void Resize(uint16 Columns, uint16 Rows) override;
            void Close() override;

            int32 GetExitCode() const override { return ExitCode; }

        private:

            int   Master   = -1;
            pid_t ChildPid = -1;
            int32 ExitCode = 0;
            bool  bExited  = false;
        };

        // Runs between fork and exec, so it may call only async-signal-safe functions.
        [[noreturn]] void RunChild(const char* SlaveName, const char* Shell, const char* WorkingDirectory)
        {
            setsid();

            const int Slave = open(SlaveName, O_RDWR);
            if (Slave < 0)
            {
                _exit(127);
            }

            ioctl(Slave, TIOCSCTTY, 0);

            dup2(Slave, STDIN_FILENO);
            dup2(Slave, STDOUT_FILENO);
            dup2(Slave, STDERR_FILENO);

            if (Slave > STDERR_FILENO)
            {
                close(Slave);
            }

            if (WorkingDirectory != nullptr && WorkingDirectory[0] != '\0')
            {
                if (chdir(WorkingDirectory) != 0)
                {
                    // A missing directory is not worth failing the shell over.
                }
            }

            // Without this, programs fall back to a dumb terminal and emit no color.
            setenv("TERM", "xterm-256color", 1);

            execlp(Shell, Shell, static_cast<char*>(nullptr));
            _exit(127);
        }

        bool FLinuxPtySession::Start(const FPtyLaunchParams& Params)
        {
            Master = posix_openpt(O_RDWR | O_NOCTTY);
            if (Master < 0)
            {
                LOG_ERROR("[Pty] posix_openpt failed ({}).", errno);
                return false;
            }

            if (grantpt(Master) != 0 || unlockpt(Master) != 0)
            {
                LOG_ERROR("[Pty] Failed to unlock the pseudo-terminal ({}).", errno);
                Close();
                return false;
            }

            const char* SlaveName = ptsname(Master);
            if (SlaveName == nullptr)
            {
                LOG_ERROR("[Pty] ptsname returned nothing.");
                Close();
                return false;
            }

            const FString Shell = Params.Shell.empty() ? GetDefaultShell() : Params.Shell;

            // Copied before the fork, because the child cannot safely touch engine containers.
            FString SlaveNameCopy(SlaveName);

            Resize(Params.Columns, Params.Rows);

            ChildPid = fork();
            if (ChildPid < 0)
            {
                LOG_ERROR("[Pty] fork failed ({}).", errno);
                Close();
                return false;
            }

            if (ChildPid == 0)
            {
                close(Master);
                RunChild(SlaveNameCopy.c_str(), Shell.c_str(), Params.WorkingDirectory.c_str());
            }

            if (fcntl(Master, F_SETFL, fcntl(Master, F_GETFL, 0) | O_NONBLOCK) != 0)
            {
                LOG_WARN("[Pty] Could not make the master non-blocking ({}).", errno);
            }

            Resize(Params.Columns, Params.Rows);

            return true;
        }

        bool FLinuxPtySession::IsRunning()
        {
            if (bExited || ChildPid <= 0)
            {
                return false;
            }

            int Status = 0;
            const pid_t Result = waitpid(ChildPid, &Status, WNOHANG);
            if (Result == ChildPid)
            {
                ExitCode = WIFEXITED(Status) ? WEXITSTATUS(Status) : -1;
                bExited  = true;
                return false;
            }

            return Result == 0;
        }

        int32 FLinuxPtySession::Read(TVector<uint8>& OutBytes, int32 MaxBytes)
        {
            if (Master < 0)
            {
                return 0;
            }

            int32 Total = 0;
            while (Total < MaxBytes)
            {
                const size_t Wanted = static_cast<size_t>(MaxBytes - Total) < 4096u
                    ? static_cast<size_t>(MaxBytes - Total)
                    : 4096u;

                const size_t Offset = OutBytes.size();
                OutBytes.resize(Offset + Wanted);

                const ssize_t Got = read(Master, OutBytes.data() + Offset, Wanted);
                if (Got <= 0)
                {
                    OutBytes.resize(Offset);
                    break;
                }

                OutBytes.resize(Offset + static_cast<size_t>(Got));
                Total += static_cast<int32>(Got);
            }

            return Total;
        }

        bool FLinuxPtySession::Write(const uint8* Bytes, int32 Count)
        {
            if (Master < 0 || Bytes == nullptr || Count <= 0)
            {
                return false;
            }

            int32 Written = 0;
            while (Written < Count)
            {
                const ssize_t Chunk = write(Master, Bytes + Written, static_cast<size_t>(Count - Written));
                if (Chunk < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    return errno == EAGAIN;
                }

                Written += static_cast<int32>(Chunk);
            }

            return true;
        }

        void FLinuxPtySession::Resize(uint16 Columns, uint16 Rows)
        {
            if (Master < 0)
            {
                return;
            }

            winsize Size = {};
            Size.ws_col = Columns;
            Size.ws_row = Rows;

            ioctl(Master, TIOCSWINSZ, &Size);
        }

        void FLinuxPtySession::Close()
        {
            if (ChildPid > 0 && !bExited)
            {
                kill(ChildPid, SIGHUP);

                int Status = 0;
                if (waitpid(ChildPid, &Status, 0) == ChildPid)
                {
                    ExitCode = WIFEXITED(Status) ? WEXITSTATUS(Status) : -1;
                }
            }

            if (Master >= 0)
            {
                close(Master);
                Master = -1;
            }

            ChildPid = -1;
            bExited  = true;
        }
    }

    bool IsPtySupported()
    {
        return true;
    }

    FString GetDefaultShell()
    {
        const FString Preferred = GetEnvVariable("SHELL");
        return Preferred.empty() ? FString("/bin/bash") : Preferred;
    }

    FPtySessionPtr CreatePtySession(const FPtyLaunchParams& Params)
    {
        FLinuxPtySession* Session = Memory::New<FLinuxPtySession>();
        if (!Session->Start(Params))
        {
            Memory::Delete(Session);
            return FPtySessionPtr();
        }

        return FPtySessionPtr(Session);
    }
}

#endif
