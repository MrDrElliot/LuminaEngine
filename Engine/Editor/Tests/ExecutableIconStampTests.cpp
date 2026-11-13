#include <gtest/gtest.h>

#include "Containers/Algorithm.h"
#include "Containers/StringFormat.h"
#include "Cooker/AssetCooker.h"
#include "Cooker/ExecutableIconStamp.h"
#include "Core/Windows/Window.h"
#include "FileSystem/PakFileSystem.h"
#include "Pak/PakWriter.h"
#include "Paths/Paths.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "Tools/Image/ImageWrite.h"

#if defined(LE_PLATFORM_WINDOWS)
#include <Windows.h>
#pragma comment(lib, "user32.lib")
#endif

using namespace Lumina;

namespace
{
    struct FIconTestFolder
    {
        FString Root = FString(*Paths::Combine(Filesystem::GetTempDirectory(), "LuminaExecutableIconStampTests"));

        FIconTestFolder()
        {
            Filesystem::RemoveTree(Root);
            Filesystem::MakeDirectoryTree(Root);
        }

        ~FIconTestFolder() { Filesystem::RemoveTree(Root); }

        FString File(FStringView Name) const { return FString(*Paths::Combine(Root, Name)); }
    };

    Windowing::FWindowIcon SolidIcon(int32 Size, uint8 R, uint8 G, uint8 B)
    {
        Windowing::FWindowIcon Icon;
        Icon.Width = Size;
        Icon.Height = Size;
        for (int32 i = 0; i < Size * Size; ++i)
        {
            Icon.Pixels.Append({ R, G, B, 255 });
        }
        return Icon;
    }

    int32 CountWarnings(const TVector<FString>& Lines)
    {
        int32 Count = 0;
        for (const FString& Line : Lines)
        {
            Count += Line.Find("[warn]") != Constants::kIndexNone ? 1 : 0;
        }
        return Count;
    }

    bool EncodeLogged(FStringView Path, TVector<uint8>& Png, TVector<FString>& Lines)
    {
        return EncodeProjectIcon(Path, Png, [&Lines](FStringView Line) { Lines.Emplace(Line.GetData(), Line.Len()); });
    }
}

TEST(ExecutableIconStamp, UnsetIconIsSilentAndBadIconsWarnOnce)
{
    FIconTestFolder Folder;
    TVector<uint8> Png;
    TVector<FString> Lines;
    EXPECT_FALSE(EncodeLogged("", Png, Lines));
    EXPECT_EQ(CountWarnings(Lines), 0);

    const FString Empty = Folder.File("Empty.png");
    const FString Garbage = Folder.File("Garbage.png");
    ASSERT_TRUE(Filesystem::WriteFile(Empty, TSpan<const uint8>()));
    const uint8 NotAnImage[] = { 'n', 'o', 't', ' ', 'a', ' ', 'p', 'n', 'g' };
    ASSERT_TRUE(Filesystem::WriteFile(Garbage, TSpan<const uint8>(NotAnImage, sizeof(NotAnImage))));

    for (const FString& Path : { Folder.File("Missing.png"), Empty, Garbage })
    {
        Lines.Reset();
        EXPECT_FALSE(EncodeLogged(Path, Png, Lines)) << *Path;
        EXPECT_EQ(CountWarnings(Lines), 1) << *Path;
    }
}

TEST(ExecutableIconStamp, CookedProjectIconResolvesThroughThePak)
{
    FIconTestFolder Folder;
    const Windowing::FWindowIcon Source = SolidIcon(20, 10, 200, 30);
    const FString Path = Folder.File("Icon.png");
    ASSERT_TRUE(ImageWrite::WritePngFile(*Path, 20, 20, 4, Source.Pixels.GetData(), 20 * 4));

    TVector<uint8> Png;
    TVector<FString> Lines;
    ASSERT_TRUE(EncodeLogged(Path, Png, Lines));
    EXPECT_EQ(CountWarnings(Lines), 0);

    // The cooked game mounts each top-level alias of its pak, and the cooked settings name this path.
    const FString PakPath = Folder.File("Game.pak");
    FPakWriter Writer;
    ASSERT_TRUE(Writer.AddEntry(CookedProjectIconPath, TSpan<const uint8>(Png.GetData(), Png.Num())));
    ASSERT_TRUE(Writer.Finalize(PakPath));
    const TSharedPtr<FPakArchive> Archive = FPakArchive::Open(PakPath);
    ASSERT_TRUE(Archive != nullptr);
    const TVector<FString> Aliases = Archive->GetTopLevelAliases();
    ASSERT_EQ(Aliases.Num(), 1);
    EXPECT_TRUE(CookedProjectIconPath.StartsWith(FStringView(*Aliases[0], Aliases[0].Len())));

    VFS::FPakFileSystem Mounted(FFixedString(*Aliases[0], Aliases[0].Len()), Archive);
    TVector<uint8> Shipped;
    ASSERT_TRUE(Mounted.ReadFile(Shipped, CookedProjectIconPath));
    const FString ShippedPath = Folder.File("Shipped.png");
    ASSERT_TRUE(Filesystem::WriteFile(ShippedPath, TSpan<const uint8>(Shipped.GetData(), Shipped.Num())));

    Windowing::FWindowIcon Decoded;
    ASSERT_TRUE(Windowing::LoadWindowIcon(ShippedPath, Decoded));
    EXPECT_EQ(Decoded.Width, 20);
    EXPECT_TRUE(Decoded.Pixels == Source.Pixels);
}

#if defined(LE_PLATFORM_WINDOWS)
namespace
{
    constexpr uint8 PngSignature[] = { 0x89, 'P', 'N', 'G' };

    #pragma pack(push, 2)
    struct FGroupIconEntry
    {
        BYTE  Width;
        BYTE  Height;
        BYTE  ColorCount;
        BYTE  Reserved;
        WORD  Planes;
        WORD  BitCount;
        DWORD BytesInRes;
        WORD  Id;
    };
    #pragma pack(pop)

    // A copy of this test binary, which carries no icon of its own, stands in for the packaged exe.
    FString CopyOfTestExecutable(const FIconTestFolder& Folder)
    {
        wchar_t Self[MAX_PATH];
        GetModuleFileNameW(nullptr, Self, MAX_PATH);
        const FString Copy = Folder.File("Stamped.exe");
        EXPECT_TRUE(Filesystem::Copy(FString(StringUtils::FromWideString(Self)), Copy, true));
        return Copy;
    }

    TSpan<const uint8> FindResourceBytes(HMODULE Module, LPCWSTR Type, WORD Id)
    {
        HRSRC Resource = FindResourceW(Module, MAKEINTRESOURCEW(Id), Type);
        if (Resource == nullptr)
        {
            return {};
        }
        return TSpan<const uint8>(static_cast<const uint8*>(LockResource(LoadResource(Module, Resource))), SizeofResource(Module, Resource));
    }

    int32 CountIconResources(HMODULE Module)
    {
        int32 Count = 0;
        EnumResourceNamesW(Module, RT_ICON, [](HMODULE, LPCWSTR, LPWSTR, LONG_PTR Param) -> BOOL
        {
            ++*reinterpret_cast<int32*>(Param);
            return TRUE;
        }, (LONG_PTR)&Count);
        return Count;
    }
}

TEST(ExecutableIconStamp, ReplacesTheLauncherIconGroupAtEverySize)
{
    FIconTestFolder Folder;
    const FString Exe = CopyOfTestExecutable(Folder);

    FString Error;
    ASSERT_TRUE(ExecutableIconStamp::Stamp(Exe, SolidIcon(64, 255, 0, 0), Error)) << *Error;
    ASSERT_TRUE(ExecutableIconStamp::Stamp(Exe, SolidIcon(300, 0, 0, 255), Error)) << *Error;

    HMODULE Module = LoadLibraryExW(*StringUtils::ToWideString(Exe), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    ASSERT_NE(Module, nullptr);

    const TSpan<const uint8> Group = FindResourceBytes(Module, RT_GROUP_ICON, ExecutableIconStamp::LauncherIconGroupId);
    ASSERT_GE(Group.Num(), 6);
    const uint16 Count = reinterpret_cast<const WORD*>(Group.GetData())[2];
    ASSERT_EQ(Count, std::size(ExecutableIconStamp::IconSizes));
    ASSERT_EQ((size_t)Group.Num(), 6 + Count * sizeof(FGroupIconEntry));
    EXPECT_EQ(CountIconResources(Module), Count);

    const FGroupIconEntry* Entries = reinterpret_cast<const FGroupIconEntry*>(Group.GetData() + 6);
    for (uint16 i = 0; i < Count; ++i)
    {
        const uint32 Size = ExecutableIconStamp::IconSizes[i];
        EXPECT_EQ(Entries[i].Width, (BYTE)(Size % 256));
        EXPECT_EQ(Entries[i].BitCount, 32);

        const TSpan<const uint8> Image = FindResourceBytes(Module, RT_ICON, Entries[i].Id);
        ASSERT_EQ((DWORD)Image.Num(), Entries[i].BytesInRes) << Size;
        if (Size == 256)
        {
            EXPECT_TRUE(Algo::Equal(Image.Left((int32)sizeof(PngSignature)), TSpan<const uint8>(PngSignature)));
            continue;
        }

        const BITMAPINFOHEADER* Header = reinterpret_cast<const BITMAPINFOHEADER*>(Image.GetData());
        EXPECT_EQ(Header->biWidth, (LONG)Size);
        EXPECT_EQ(Header->biHeight, (LONG)Size * 2);
        const uint8* FirstPixel = Image.GetData() + sizeof(BITMAPINFOHEADER);
        EXPECT_EQ(FirstPixel[0], 255) << "blue of the second stamp at " << Size;
        EXPECT_EQ(FirstPixel[2], 0) << "red of the first stamp at " << Size;
    }

    // Windows' own pick for a 32 px icon lands on the stamped image.
    const int PickedId = LookupIconIdFromDirectoryEx(const_cast<PBYTE>(Group.GetData()), TRUE, 32, 32, LR_DEFAULTCOLOR);
    EXPECT_EQ(PickedId, Entries[1].Id);
    FreeLibrary(Module);
}

TEST(ExecutableIconStamp, UnusableIconLeavesTheExecutableUnchanged)
{
    FIconTestFolder Folder;
    const FString Exe = CopyOfTestExecutable(Folder);
    TVector<uint8> Before;
    ASSERT_TRUE(Filesystem::ReadFile(Before, Exe));

    FString Error;
    EXPECT_FALSE(ExecutableIconStamp::Stamp(Exe, Windowing::FWindowIcon(), Error));
    EXPECT_FALSE(Error.IsEmpty());

    TVector<uint8> After;
    ASSERT_TRUE(Filesystem::ReadFile(After, Exe));
    EXPECT_TRUE(Before == After);
}
#endif
