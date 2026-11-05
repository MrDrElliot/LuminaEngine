#pragma once

#include "CategorizedPicker.h"
#include "Containers/HashTable.h"
#include "Scripting/DotNet/DotNetHost.h"

namespace Lumina
{
    class CClass;
    class CStruct;
}

namespace Lumina::EntityScriptPicker
{
    // Every class an entity can take as a script, C# and C++ alike, filed by the folder its source sits in. Keyed by class name.
    EDITOR_API void GatherItems(TVector<FPickerItem>& Out);

    // The C# script class a dropped .cs file declares, preferring the one named after the file, and null when it declares none.
    EDITOR_API CClass* FindScriptClassForSource(FStringView VirtualPath);

    // The category C++ scripts file under, which a picker lists after the project's own folders.
    inline constexpr const char* NativeCategory = "C++";

    // A reflected name with its namespace dropped, such as Walker for Ashgrave.Walker.
    inline FStringView ShortTypeName(FStringView FullName)
    {
        const int32 Dot = FullName.FindLastOf('.');
        return Dot == Constants::kIndexNone ? FullName : FullName.Mid(Dot + 1);
    }

    // One pass over the script roots, so filing many C# types by folder does not walk them once per type.
    class EDITOR_API FScriptSourceIndex
    {
    public:

        FScriptSourceIndex();
        LE_NO_COPYMOVE(FScriptSourceIndex);

        // Matched by the type's recorded SourceFile first, then by a file named after it, and null when neither turns up.
        const DotNet::FScriptSourceFolder* Find(const CStruct* Type, FStringView ShortName) const;

    private:

        TVector<DotNet::FScriptSourceFolder>                  Sources;
        THashMap<FString, const DotNet::FScriptSourceFolder*> ByStem;
        THashMap<FString, const DotNet::FScriptSourceFolder*> ByPath;
    };
}
