#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

namespace Lumina::Platform
{
    struct FPtyLaunchParams
    {
        /** Empty picks the platform default from GetDefaultShell. */
        FString Shell;

        FString WorkingDirectory;

        uint16 Columns = 120;
        uint16 Rows    = 30;
    };

    /** A child attached to a pseudo-terminal, carrying raw VT byte streams rather than lines. */
    class RUNTIME_API IPtySession
    {
    public:

        virtual ~IPtySession() = default;

        NODISCARD virtual bool IsRunning() = 0;

        /** Appends everything readable without blocking, bounded by MaxBytes, and returns the count. */
        virtual int32 Read(TVector<uint8>& OutBytes, int32 MaxBytes) = 0;

        virtual bool Write(const uint8* Bytes, int32 Count) = 0;

        virtual void Resize(uint16 Columns, uint16 Rows) = 0;

        /** Terminates the child and releases the pseudo-console. Idempotent. */
        virtual void Close() = 0;

        /** Meaningful once IsRunning returns false. */
        NODISCARD virtual int32 GetExitCode() const = 0;
    };

    using FPtySessionPtr = TUniquePtr<IPtySession>;

    /** Null when the platform has no pseudo-terminal, or the shell could not be spawned. */
    RUNTIME_API FPtySessionPtr CreatePtySession(const FPtyLaunchParams& Params);

    /** What CreatePtySession substitutes for an empty FPtyLaunchParams::Shell. */
    RUNTIME_API FString GetDefaultShell();

    RUNTIME_API bool IsPtySupported();
}
