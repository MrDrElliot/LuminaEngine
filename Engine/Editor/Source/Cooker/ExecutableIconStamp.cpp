#include "EditorPCH.h"
#include "ExecutableIconStamp.h"

#include "Containers/StringFormat.h"
#include "Core/Windows/Window.h"

#if defined(LE_PLATFORM_WINDOWS)
#include <Windows.h>
#include "Tools/Image/ImageWrite.h"
#include "Tools/Import/ImportHelpers.h"
#endif

namespace Lumina::ExecutableIconStamp
{
#if defined(LE_PLATFORM_WINDOWS)
    namespace
    {
        constexpr uint32 NumSizes = (uint32)std::size(IconSizes);

        // RuntimePCH defines NORESOURCE, which hides the RT_ICON and RT_GROUP_ICON names winuser.h would give.
        const LPCWSTR IconResource = MAKEINTRESOURCEW(3);
        const LPCWSTR IconGroupResource = MAKEINTRESOURCEW(14);

        #pragma pack(push, 2)
        struct FGroupIconHeader
        {
            uint16 Reserved;
            uint16 Type;
            uint16 Count;
        };

        struct FGroupIconEntry
        {
            uint8  Width;
            uint8  Height;
            uint8  ColorCount;
            uint8  Reserved;
            uint16 Planes;
            uint16 BitCount;
            uint32 BytesInRes;
            uint16 Id;
        };
        #pragma pack(pop)

        template<typename T>
        void AppendBytes(TVector<uint8>& Out, const T& Value)
        {
            Out.Append(reinterpret_cast<const uint8*>(&Value), sizeof(T));
        }

        // Bottom-up BGRA rows followed by the transparency mask, which stays clear because alpha carries the shape.
        TVector<uint8> EncodeBitmap(const uint8* Rgba, uint32 Size)
        {
            const uint32 MaskBytes = (Size + 31) / 32 * 4 * Size;

            BITMAPINFOHEADER Header = {};
            Header.biSize        = sizeof(BITMAPINFOHEADER);
            Header.biWidth       = (LONG)Size;
            Header.biHeight      = (LONG)Size * 2;
            Header.biPlanes      = 1;
            Header.biBitCount    = 32;
            Header.biCompression = BI_RGB;
            Header.biSizeImage   = Size * Size * 4 + MaskBytes;

            TVector<uint8> Out;
            AppendBytes(Out, Header);
            for (uint32 Row = Size; Row-- > 0;)
            {
                const uint8* Pixel = Rgba + Row * Size * 4;
                for (uint32 X = 0; X < Size; ++X, Pixel += 4)
                {
                    Out.Append({ Pixel[2], Pixel[1], Pixel[0], Pixel[3] });
                }
            }
            Out.AddZeroed(MaskBytes);
            return Out;
        }

        // What the exe already holds, so the update replaces the group in its own language instead of adding a second copy.
        struct FExistingIcons
        {
            WORD            Language = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
            TVector<uint16> GroupIconIds;
            uint16          MaxIconId = 0;
        };

        bool ReadExistingIcons(const wchar_t* Path, FExistingIcons& Out)
        {
            HMODULE Module = LoadLibraryExW(Path, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            if (Module == nullptr)
            {
                return false;
            }

            EnumResourceNamesW(Module, IconResource, [](HMODULE, LPCWSTR, LPWSTR Name, LONG_PTR Param) -> BOOL
            {
                uint16& MaxId = *reinterpret_cast<uint16*>(Param);
                const uint16 Id = IS_INTRESOURCE(Name) ? (uint16)(ULONG_PTR)Name : 0;
                MaxId = Id > MaxId ? Id : MaxId;
                return TRUE;
            }, (LONG_PTR)&Out.MaxIconId);

            EnumResourceLanguagesW(Module, IconGroupResource, MAKEINTRESOURCEW(LauncherIconGroupId), [](HMODULE, LPCWSTR, LPCWSTR, WORD Language, LONG_PTR Param) -> BOOL
            {
                *reinterpret_cast<WORD*>(Param) = Language;
                return FALSE;
            }, (LONG_PTR)&Out.Language);

            if (HRSRC Resource = FindResourceExW(Module, IconGroupResource, MAKEINTRESOURCEW(LauncherIconGroupId), Out.Language))
            {
                const uint8* Data = static_cast<const uint8*>(LockResource(LoadResource(Module, Resource)));
                const size_t Size = Data != nullptr ? SizeofResource(Module, Resource) : 0;
                const uint16 Count = Size >= sizeof(FGroupIconHeader) ? reinterpret_cast<const FGroupIconHeader*>(Data)->Count : 0;
                for (uint16 i = 0; i < Count && sizeof(FGroupIconHeader) + (i + 1) * sizeof(FGroupIconEntry) <= Size; ++i)
                {
                    Out.GroupIconIds.Add(reinterpret_cast<const FGroupIconEntry*>(Data + sizeof(FGroupIconHeader))[i].Id);
                }
            }

            FreeLibrary(Module);
            return true;
        }
    }

    bool Stamp(FStringView ExePath, const Windowing::FWindowIcon& Icon, FString& OutError)
    {
        if (Icon.Width <= 0 || Icon.Height <= 0 || Icon.Pixels.Num() != Icon.Width * Icon.Height * 4)
        {
            OutError = "the icon has no pixels";
            return false;
        }

        const FWString WidePath = StringUtils::ToWideString(ExePath);
        FExistingIcons Existing;
        if (!ReadExistingIcons(*WidePath, Existing))
        {
            OutError = Format("{} could not be opened as an executable", ExePath);
            return false;
        }

        TVector<uint8> Images[NumSizes];
        uint16 Ids[NumSizes];
        uint16 NextId = Existing.MaxIconId;

        TVector<uint8> Group;
        AppendBytes(Group, FGroupIconHeader{ 0, 1, (uint16)NumSizes });
        for (uint32 i = 0; i < NumSizes; ++i)
        {
            const uint32 Size = IconSizes[i];
            Import::Textures::FTextureImportResult Resized{ TVector64<uint8>(Icon.Pixels.GetData(), Icon.Pixels.Num()), FUIntVector2(Icon.Width, Icon.Height), EFormat::RGBA8_UNORM };
            Import::Textures::ResizeImportResult(Resized, FUIntVector2(Size, Size));

            if (i + 1 < NumSizes)
            {
                Images[i] = EncodeBitmap(Resized.Pixels.GetData(), Size);
            }
            else if (!ImageWrite::EncodePng(Images[i], Size, Size, 4, Resized.Pixels.GetData(), Size * 4))
            {
                OutError = Format("the {} px icon could not be encoded as PNG", Size);
                return false;
            }

            Ids[i] = i < (uint32)Existing.GroupIconIds.Num() ? Existing.GroupIconIds[i] : ++NextId;
            const uint8 Dimension = (uint8)(Size % 256);
            AppendBytes(Group, FGroupIconEntry{ Dimension, Dimension, 0, 0, 1, 32, (uint32)Images[i].Num(), Ids[i] });
        }

        HANDLE Update = BeginUpdateResourceW(*WidePath, FALSE);
        if (Update == nullptr)
        {
            OutError = Format("{} could not be opened for a resource update (error {})", ExePath, GetLastError());
            return false;
        }

        bool bUpdated = true;
        for (int32 i = NumSizes; i < Existing.GroupIconIds.Num(); ++i)
        {
            bUpdated &= UpdateResourceW(Update, IconResource, MAKEINTRESOURCEW(Existing.GroupIconIds[i]), Existing.Language, nullptr, 0) != FALSE;
        }
        for (uint32 i = 0; i < NumSizes; ++i)
        {
            bUpdated &= UpdateResourceW(Update, IconResource, MAKEINTRESOURCEW(Ids[i]), Existing.Language, Images[i].GetData(), (DWORD)Images[i].Num()) != FALSE;
        }
        bUpdated &= UpdateResourceW(Update, IconGroupResource, MAKEINTRESOURCEW(LauncherIconGroupId), Existing.Language, Group.GetData(), (DWORD)Group.Num()) != FALSE;

        const DWORD UpdateError = bUpdated ? 0 : GetLastError();
        if (!EndUpdateResourceW(Update, !bUpdated) || !bUpdated)
        {
            OutError = Format("the icon resources of {} could not be written (error {})", ExePath, bUpdated ? GetLastError() : UpdateError);
            return false;
        }
        return true;
    }
#else
    bool Stamp(FStringView, const Windowing::FWindowIcon&, FString& OutError)
    {
        OutError = "only Windows executables carry icon resources";
        return false;
    }
#endif
}
