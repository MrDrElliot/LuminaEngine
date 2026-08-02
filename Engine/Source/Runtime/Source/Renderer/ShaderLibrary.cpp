#include "RuntimePCH.h"
#include "ShaderLibrary.h"
#include "ShaderCompiler.h"
#include "Memory/Memory.h"
#include "Paths/Paths.h"

namespace Lumina
{
    static uint64 EntryHash(const FName& Path, TSpan<const FString> Defines)
    {
        uint64 Hash = Path.GetID();
        for (const FString& Define : Defines)
        {
            Hash::HashCombine(Hash, Define);
        }
        return Hash;
    }

    FShaderLibrary::~FShaderLibrary()
    {
        for (auto& [Hash, Entry] : Entries)
        {
            Memory::Delete(Entry);
        }
    }

    FShaderEntry& FShaderLibrary::FindOrCreate(uint64 Hash)
    {
        auto It = Entries.find(Hash);
        if (It == Entries.end())
        {
            FShaderEntry* Entry = Memory::New<FShaderEntry>();
            Entry->ID = NextID++;
            It = Entries.emplace(Hash, Entry).first;
        }
        return *It->second;
    }

    const FShaderEntry* FShaderLibrary::Get(const FName& Path, TSpan<const FString> Defines)
    {
        FShaderLibrary* Library = GShaderLibrary;
        const uint64 Hash = EntryHash(Path, Defines);

        {
            FScopeLock Lock(Library->Mutex);
            FShaderEntry& Entry = Library->FindOrCreate(Hash);
            if (Entry.IsValid())
            {
                return &Entry;
            }
            if (Entry.Path.IsNone())
            {
                Entry.Path = Path;
                Entry.Defines.assign(Defines.begin(), Defines.end());
            }
        }

        // Not delivered yet (startup batch still in flight, or a variant never requested
        // before): compile synchronously so the caller's cached pointer is usable now.
        FShaderCompileOptions Options;
        Options.bGenerateReflectionData = true;
        Options.MacroDefinitions.assign(Defines.begin(), Defines.end());
        GShaderCompiler->CompileShaderPath(Paths::GetEngineShadersDirectory() + "/" + Path.c_str(), Options, [](const FShaderHeader& Header)
        {
            Commit(Header);
        });
        GShaderCompiler->Flush();

        FScopeLock Lock(Library->Mutex);
        return &Library->FindOrCreate(Hash);
    }

    const FShaderEntry* FShaderLibrary::Commit(const FName& Key, ERHIShaderType Type, TSpan<const uint32> Spirv)
    {
        FShaderLibrary* Library = GShaderLibrary;
        FScopeLock Lock(Library->Mutex);

        FShaderEntry& Entry = Library->FindOrCreate(EntryHash(Key, {}));
        Entry.Path = Key;
        Entry.Type = Type;
        Entry.Spirv.assign(Spirv.begin(), Spirv.end());
        Entry.Generation++;
        return &Entry;
    }

    void FShaderLibrary::Commit(const FShaderHeader& Header)
    {
        FShaderLibrary* Library = GShaderLibrary;
        FScopeLock Lock(Library->Mutex);

        const FName Path(Header.DebugName.c_str());
        FShaderEntry& Entry = Library->FindOrCreate(EntryHash(Path, TSpan<const FString>(Header.Defines.data(), Header.Defines.size())));
        Entry.Path    = Path;
        Entry.Defines = Header.Defines;
        Entry.Type    = Header.Reflection.ShaderType;
        Entry.Spirv   = Header.Binaries;
        Entry.Generation++;
    }
}
