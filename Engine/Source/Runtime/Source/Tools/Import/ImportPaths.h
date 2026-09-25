#pragma once

#include "Containers/String.h"
#include "Platform/GenericPlatform.h"

namespace Lumina::Import::PathReservations
{
    // The one owner of import destination names, so two importers cannot hand out the same path.

    /** True when no loaded package, nothing on disk and no in-flight import already owns Path. */
    RUNTIME_API bool IsFree(FStringView Path);

    /** Reserves Path, or Path_1, Path_2 and so on when it is taken. Empty when nothing was free. */
    RUNTIME_API FFixedString Reserve(FStringView Path);

    /** Hands a reservation back, whether the import committed or failed. */
    RUNTIME_API void Release(FStringView Path);

    /** Drops every reservation, for a teardown that cannot pair its releases. */
    RUNTIME_API void ReleaseAll();

    // Releases what it reserved unless Commit is called, so an early return cannot leak a name.
    class RUNTIME_API FScopedReservation
    {
    public:

        explicit FScopedReservation(FStringView Path);
        ~FScopedReservation();

        FScopedReservation(const FScopedReservation&) = delete;
        FScopedReservation& operator=(const FScopedReservation&) = delete;

        const FFixedString& Get() const { return Reserved; }
        bool IsValid() const { return !Reserved.empty(); }

        /** Keeps the reservation past this scope, for a name the caller now owns. */
        void Commit() { bCommitted = true; }

    private:

        FFixedString Reserved;
        bool         bCommitted = false;
    };
}
