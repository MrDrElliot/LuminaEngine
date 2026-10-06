#pragma once

#include "Containers/Vector.h"
#include "ModuleAPI.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class FProperty;

    // Copies of some properties' values, kept so a later frame can ask which of them changed.
    class RUNTIME_API FPropertySnapshot
    {
    public:

        FPropertySnapshot() = default;
        ~FPropertySnapshot();

        FPropertySnapshot(const FPropertySnapshot&) = delete;
        FPropertySnapshot& operator=(const FPropertySnapshot&) = delete;

        // Takes the fields and their current values in Container, dropping whatever was held before.
        void Capture(const TVector<FProperty*>& InFields, const void* Container);

        void Reset();

        bool IsCaptured() const { return bCaptured; }
        size_t Num() const { return Fields.size(); }
        FProperty* Field(size_t Index) const { return Fields[Index]; }

        // The value held for field Index, which is the old one until Store replaces it.
        void* Slot(size_t Index);

        // False for a field that does not report its size, which then always matches.
        bool IsTracked(size_t Index) const;

        // True when field Index in Container still equals the held value.
        bool Matches(size_t Index, const void* Container);

        void Store(size_t Index, const void* Container);

        // The index of Field, or -1 when it is not held.
        int32 IndexOf(const FProperty* InField) const;

    private:

        TVector<FProperty*>       Fields;
        TVector<size_t>           Offsets;
        TVector<std::max_align_t> Storage;
        bool                      bCaptured = false;
    };
}
