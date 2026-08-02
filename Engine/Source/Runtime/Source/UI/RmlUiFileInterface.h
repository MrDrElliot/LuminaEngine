#pragma once

// Bridges Rml::FileInterface to VFS; whole file is read into memory. Use only for small UI/font assets.

#include <RmlUi/Core/FileInterface.h>

namespace Lumina
{
    class FRmlUiFileInterface final : public Rml::FileInterface
    {
    public:
        Rml::FileHandle Open(const Rml::String& Path) override;
        void            Close(Rml::FileHandle File) override;
        size_t          Read(void* Buffer, size_t Size, Rml::FileHandle File) override;
        bool            Seek(Rml::FileHandle File, long Offset, int Origin) override;
        size_t          Tell(Rml::FileHandle File) override;
        size_t          Length(Rml::FileHandle File) override;
        bool            LoadFile(const Rml::String& Path, Rml::String& OutData) override;
    };
}
