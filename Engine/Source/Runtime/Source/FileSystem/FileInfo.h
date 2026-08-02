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
    
    struct FFileInfo
    {
        FString         Name;
        
        FFixedString    VirtualPath;
        FFixedString    PathSource;
        
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
