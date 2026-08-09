#pragma once

#include "Containers/String.h"
#include "Core/LuminaMacros.h"
#include "Platform/Platform.h"

namespace Lumina::VFS
{
    
    enum class EFileFlags : uint8
    {
        None        = 0,

        Directory   = BIT(0),
        File        = BIT(1),
        Symlink     = BIT(2),

        Hidden      = BIT(3),

        ReadOnly    = BIT(4),

        LAssetFile  = BIT(6),
    };
    
    ENUM_CLASS_FLAGS(EFileFlags);
    
    // Paths are heap strings, not fixed strings. A pair of 255-char inline buffers made this ~620 bytes,
    // and consumers keep them in bulk -- one per content-browser tile, one per file the cooker walks --
    // so the inline capacity cost far more than the allocation it was avoiding.
    struct FFileInfo
    {
        FString         Name;

        FString         VirtualPath;
        FString         PathSource;

        int64           LastModifyTime;
        EFileFlags      Flags;

        
        NODISCARD FString GetExt() const
        {
            size_t DotPos = Name.find_last_of('.');
            if (DotPos == FString::npos)
            {
                return {};
            }
            
            return Name.substr(DotPos);
        }
        
        NODISCARD bool IsDirectory() const  { return EnumHasAllFlags(Flags, EFileFlags::Directory); }
        NODISCARD bool IsHidden() const     { return EnumHasAllFlags(Flags, EFileFlags::Hidden); }
        NODISCARD bool IsReadOnly() const   { return EnumHasAllFlags(Flags, EFileFlags::ReadOnly); }
        NODISCARD bool IsLAsset() const     { return EnumHasAllFlags(Flags, EFileFlags::LAssetFile); }

    };
}
