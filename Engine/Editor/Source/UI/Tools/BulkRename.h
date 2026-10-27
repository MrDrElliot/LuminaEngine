#pragma once
#include "Containers/Function.h"
#include "Containers/String.h"
#include "Containers/Vector.h"

namespace Lumina
{
    class IEditorToolContext;

    struct FBulkRenameRule
    {
        FString Find;
        FString Replace;
        FString Prefix;
        FString Suffix;
        bool    bMatchCase = false;
        bool    bNumber = false;
        int32   NumberStart = 1;
        int32   NumberDigits = 2;
    };

    enum class EBulkRenameStatus : uint8
    {
        Ready,
        Unchanged,
        Empty,
        InvalidCharacters,
        Duplicate,
        Exists,
    };

    struct FBulkRenameEntry
    {
        FFixedString      SourcePath;
        FFixedString      TargetPath;
        FString           NewName;
        bool              bDirectory = false;
        EBulkRenameStatus Status = EBulkRenameStatus::Unchanged;
    };

    // Find and replace first, then the prefix, the suffix and the Index-th number of the batch.
    EDITOR_API FString ApplyBulkRenameRule(const FBulkRenameRule& Rule, FStringView Name, int32 Index);

    // Names and checks every entry in order, where Exists answers whether a path is already taken on disk.
    EDITOR_API void PlanBulkRename(const FBulkRenameRule& Rule, TVector<FBulkRenameEntry>& Entries, const TFunction<bool(FStringView)>& Exists);

    // Previews the batch under a rule the user edits, then moves every ready entry once its files are writable.
    EDITOR_API void OpenBulkRenameModal(IEditorToolContext* Context, TVector<FFixedString> Paths, TFunction<void()> OnRenamed);
}
