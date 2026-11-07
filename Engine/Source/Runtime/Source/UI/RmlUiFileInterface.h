#pragma once

// Bridges Rml::FileInterface to VFS; whole file is read into memory. Use only for small UI/font assets.

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Threading/Sync.h"

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

        // True when a stylesheet read since the last call has a different write time on disk now, which leaves RmlUi's sheet cache stale.
        bool ConsumeChangedStyleSheets();

    private:
        struct FReadStyleSheet
        {
            FString Path;
            int64   WriteTime = 0;
        };

        void NoteStyleSheetRead(FStringView VirtualPath);

        // RmlUi keeps every parsed stylesheet by path for the whole session, so the files behind them are tracked here.
        FMutex                   StyleSheetMutex;
        TVector<FReadStyleSheet> ReadStyleSheets;
    };
}
