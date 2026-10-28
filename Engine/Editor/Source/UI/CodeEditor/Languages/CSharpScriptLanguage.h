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
        bool GetDocumentSymbols(const FCodeDocument& Document, TVector<FCodeSymbolInfo>& Out) const override;
        void GetCodeActions(const FCodeDocument& Document, FCodePosition Position, TVector<FCodeAction>& Out) const override;

        enum class EReply : uint8
        {
            Answered,
            // The unit is still being built, so the caller asks again later.
            Busy,
            // Scripting is off, so the word based answers stand in.
            Unavailable,
        };

        struct FReference
        {
            FString    Path;
            FCodeRange Range;
            FString    Preview;
            bool       bDeclaration = false;
        };

        struct FProjectDiagnostic
        {
            FString         Path;
            FCodeDiagnostic Diagnostic;
        };

        // Hands unsaved text to the compiler, so a search across the unit sees what the editor shows rather than the file on disk.
        void Sync(const FCodeDocument& Document) const;
        EReply FindReferences(const FCodeDocument& Document, FCodePosition Position, TVector<FReference>& Out) const;
        EReply FindWorkspaceSymbols(FStringView Search, TVector<FCodeSymbolInfo>& Out) const;
        EReply GetProjectDiagnostics(TVector<FProjectDiagnostic>& Out) const;

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
            References = 6,
            DocumentSymbols = 7,
            WorkspaceSymbols = 8,
            ProjectDiagnostics = 9,
            CodeActions = 10,
            Sync = 11,
        };

        using FRecord = TVector<FString>;
        EReply Query(EQuery Kind, const FCodeDocument& Document, FCodePosition Position, TVector<FRecord>& OutRecords) const;
        EReply QueryText(EQuery Kind, FStringView Text, int32 Offset, TVector<FRecord>& OutRecords) const;

        FString FilePath;
        DotNet::TManagedExport<int32 (*)(int32, const char*, int32, const char*, int32, int32, void*, void*)> QueryExport{ "QueryScriptLanguage" };
    };

    namespace CSharpScript
    {
        // The starting source of a new entity script, its class named after the file.
        EDITOR_API FString NewScriptSource(FStringView FileStem);
    }
}
