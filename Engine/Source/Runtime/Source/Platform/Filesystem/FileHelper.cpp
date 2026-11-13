#include "RuntimePCH.h"
#include "FileHelper.h"

#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Log/Log.h"
#include "PlatformFilesystem.h"

namespace Lumina::FileHelper
{
    bool SaveArrayToFile(const TVector<uint8>& Array, FStringView Path)
    {
        const TSpan<const uint8> Data(Array.GetData(), Array.Num());

        if (!Filesystem::WriteFile(Path, Data))
        {
            LOG_ERROR("Failed to write data to file: {0}", Path);
            return false;
        }

        return true;
    }

    bool LoadFileToArray(TVector<uint8>& Result, FStringView Path)
    {
        if (!Filesystem::ReadFile(Result, Path))
        {
            LOG_ERROR("Failed to read data from file: {0}", Path);
            return false;
        }

        return true;
    }

    bool LoadFileIntoString(FString& OutString, FStringView Path)
    {
        if (!Filesystem::ReadFile(OutString, Path))
        {
            LOG_ERROR("Failed to open file: {0}", Path);
            return false;
        }

        return true;
    }

    bool SaveStringToFile(FStringView String, FStringView Path)
    {
        const TSpan<const uint8> Data(reinterpret_cast<const uint8*>(String.GetData()), String.Len());

        if (!Filesystem::WriteFile(Path, Data))
        {
            LOG_ERROR("Failed to open file for writing: {0}", Path);
            return false;
        }

        return true;
    }
}
