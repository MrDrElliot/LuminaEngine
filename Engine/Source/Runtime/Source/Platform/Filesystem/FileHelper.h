#pragma once

#include "Containers/Vector.h"
#include "Containers/String.h"

namespace Lumina::FileHelper
{

    RUNTIME_API bool SaveArrayToFile(const TVector<uint8>& Array, FStringView Path);
    RUNTIME_API bool LoadFileToArray(TVector<uint8>& Result, FStringView Path);

    RUNTIME_API bool LoadFileIntoString(FString& OutString, FStringView Path);
    RUNTIME_API bool SaveStringToFile(FStringView String, FStringView Path);

}
