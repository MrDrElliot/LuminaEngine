#pragma once
#include "Core/Object/ObjectMacros.h"
#include "Containers/String.h"
#include "GUID/GUID.h"
#include "UIDocumentRef.generated.h"

namespace Lumina
{
    class FNetArchive;

    // A rename-safe reference to an .rml document, which the details panel picks from the project's UI documents.
    REFLECT()
    struct RUNTIME_API SUIDocumentRef
    {
        GENERATED_BODY()

        // Last-known virtual path such as "/UI/HUD.rml", healed on resolve after a rename.
        PROPERTY(Editable)
        FString Path;

        // Stable identity as GUID text, which the picker maintains.
        PROPERTY(Hidden)
        FString Guid;

        SUIDocumentRef() = default;
        explicit SUIDocumentRef(FStringView InPath) : Path(InPath.GetData(), InPath.Len()) {}

        bool IsNull() const  { return Path.IsEmpty() && Guid.IsEmpty(); }
        bool IsValid() const { return !IsNull(); }

        FStringView GetPath() const { return FStringView(*Path, Path.Len()); }
        FGuid       GetGuid() const;

        // GUID-first lookup that heals Path after a rename, and is safe on any thread and in cooked builds.
        FStringView ResolvePath() const;

        // Resolves, then reads the document text. False on a miss.
        bool ReadText(FString& OutText) const;

        // Editor assignment that derives the GUID, minting a sidecar when the document has none.
        void SetPath(FStringView InPath);
        void Set(FStringView InPath, const FGuid& InGuid);
        void Reset() { Path.Reset(); Guid.Reset(); }

        // A compact net index when the archive binds the document hooks, otherwise the full path and GUID.
        void NetSerialize(FNetArchive& Ar);

        bool operator==(const SUIDocumentRef& Other) const { return Path == Other.Path && Guid == Other.Guid; }
        bool operator!=(const SUIDocumentRef& Other) const { return !(*this == Other); }

    private:

        // Keyed on the text it came from, since loads and replication write Guid directly.
        mutable FString ParsedGuidText;
        mutable FGuid   ParsedGuid;
    };
}
