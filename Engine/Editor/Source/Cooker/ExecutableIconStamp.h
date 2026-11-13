#pragma once

#include "Containers/String.h"

namespace Lumina::Windowing
{
    struct FWindowIcon;
}

namespace Lumina::ExecutableIconStamp
{
    // The icon group Lumina.rc gives the launcher, which Explorer shows as the exe's icon.
    inline constexpr uint16 LauncherIconGroupId = 1;

    // Windows stores the largest size as PNG data and the rest as 32-bit bitmaps.
    inline constexpr uint32 IconSizes[] = { 16, 32, 48, 256 };

    // Replaces the exe's launcher icon group and its images with Icon at every IconSizes entry, leaving the file untouched on failure.
    EDITOR_API bool Stamp(FStringView ExePath, const Windowing::FWindowIcon& Icon, FString& OutError);
}
