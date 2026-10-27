#pragma once

#include "CSharpLanguage.h"
#include "Scripting/DotNet/DotNetHost.h"

namespace Lumina
{
    // C# answered by the script compiler's own Roslyn, so completion, hover, signatures and errors know the project's real types.
    class EDITOR_API FCSharpScriptLanguage : public FCSharpLanguage
    {
    public:

        void SetFilePath(FStringView InDiskPath) { FilePath = InDiskPath; }
        const FString& GetFilePath() const { return FilePath; }

        // Starts building the script's unit in the background, so the first suggestion does not wait on Roslyn.
        void Warm(const FCodeDocument& Document) const;

        bool IsCompletionTrigger(char Typed, FStringView LinePrefix, uint32 CaretState) const override;
        void GetCompletions(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out) const override;
        bool GetHover(const FCodeDocument& Document, FCodePosition Position, FCodeHover& Out) const override;
        bool GetDiagnostics(const FCodeDocument& Document, TVector<FCodeDiagnostic>& Out) const override;
        bool GetSignature(const FCodeDocument& Document, FCodePosition Caret, FCodeSignature& Out) const override;
        bool FindDefinition(const FCodeDocument& Document, FCodePosition Position, FCodeLocation& Out) const override;

    private:

        // Matches EScriptQuery in ScriptLanguageService.cs.
        enum class EQuery : int32
        {
            Completion = 0,
            Hover = 1,
            Signature = 2,
            Diagnostics = 3,
            Warm = 4,
            Definition = 5,
        };

        enum class EReply : uint8
        {
            Answered,
            // The unit is still being built, so the caller asks again later.
            Busy,
            // Scripting is off, so the word based answers stand in.
            Unavailable,
        };

        using FRecord = TVector<FString>;
        EReply Query(EQuery Kind, const FCodeDocument& Document, FCodePosition Position, TVector<FRecord>& OutRecords) const;

        FString FilePath;
        DotNet::TManagedExport<int32 (*)(int32, const char*, int32, const char*, int32, int32, void*, void*)> QueryExport{ "QueryScriptLanguage" };
    };
}
