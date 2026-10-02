#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "FileLibrary.generated.h"

namespace Lumina
{
    /** Reads through the virtual file system, so a /Game path finds loose files in the editor and the pak in a packaged game. */
    REFLECT()
    class RUNTIME_API CFileLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        /** True when the virtual path names a file, such as /Game/Data/Items.json. */
        FUNCTION()
        static bool FileExists(const FString& Path);

        /** The file's bytes, or nothing when it does not exist. */
        FUNCTION()
        static void ReadFileBytes(const FString& Path, TVector<uint8>& Out);

        /** The file as UTF-8 text, or an empty string when it does not exist. */
        FUNCTION()
        static FString ReadFileText(const FString& Path);
    };
}
