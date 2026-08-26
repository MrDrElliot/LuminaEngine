#include "RuntimePCH.h"
#include "ShaderLibrary.h"
#include "ShaderCompiler.h"
#include "ShaderPaths.h"
#include "FileSystem/FileSystem.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"

namespace Lumina
{
    static uint64 EntryHash(const FName& Path, TSpan<const FString> Defines)
    {
        uint64 Hash = Path.Hash();
        for (const FString& Define : Defines)
        {
            Hash::HashCombine(Hash, Define);
        }
        return Hash;
    }

    // Salted so a content hash cannot land on the small integers named shaders produce.
    static uint64 SpirvContentHash(ERHIShaderType Type, TSpan<const uint32> Spirv)
    {
        uint64 Hash = 0x9E3779B97F4A7C15ull ^ (uint64)Type;
        for (uint32 Word : Spirv)
        {
            Hash ^= (uint64)Word;
            Hash *= 0x100000001B3ull;
        }
        return Hash;
    }

    FShaderLibrary::FShaderLibrary()
    {
        // TSegmentMap::Erase calls DtorFn unconditionally, so leaving it null is a null call on first free.
        Entries.SetDtor([](FShaderEntry* Entry) { Entry->~FShaderEntry(); });
    }

    FShaderLibrary::~FShaderLibrary()
    {
        Entries.Clear();
    }

    FShaderH FShaderLibrary::FindOrCreate(uint64 Hash)
    {
        auto It = HandlesByHash.find(Hash);
        if (It != HandlesByHash.end())
        {
            if (Entries.IsLive(It->second))
            {
                return It->second;
            }
            // The slot was released since this hash was interned, so mint a fresh entry.
            HandlesByHash.erase(It);
        }

        const FShaderH Handle = Entries.Emplace();
        FShaderEntry& Entry   = Entries[Handle];
        Entry.ID       = NextID++;
        Entry.MapHash  = Hash;
        HandlesByHash.emplace(Hash, Handle);
        return Handle;
    }

    const FShaderEntry* FShaderLibrary::Resolve(FShaderH Handle)
    {
        FShaderLibrary* Library = GShaderLibrary;
        if (Library == nullptr)
        {
            return nullptr;
        }
        return Library->Entries.TryGet(Handle);
    }

    void FShaderLibrary::AddRef(FShaderH Handle)
    {
        FShaderLibrary* Library = GShaderLibrary;
        if (Library == nullptr)
        {
            return;
        }

        FScopeLock Lock(Library->Mutex);
        if (FShaderEntry* Entry = Library->Entries.TryGet(Handle))
        {
            ++Entry->RefCount;
        }
    }

    void FShaderLibrary::Release(FShaderH Handle)
    {
        FShaderLibrary* Library = GShaderLibrary;
        if (Library == nullptr)
        {
            return;
        }

        FScopeLock Lock(Library->Mutex);
        FShaderEntry* Entry = Library->Entries.TryGet(Handle);
        if (Entry == nullptr || Entry->RefCount == 0)
        {
            return;
        }

        if (--Entry->RefCount == 0)
        {
            // Queued rather than freed, since handles are dereferenced lock-free off the render thread.
            Library->PendingRelease.push_back(Handle);
        }
    }

    void FShaderLibrary::FlushPendingReleases()
    {
        FShaderLibrary* Library = GShaderLibrary;
        if (Library == nullptr)
        {
            return;
        }

        static thread_local TVector<FShaderH> Ready;
        Ready.clear();
        {
            FScopeLock Lock(Library->Mutex);

            // Swapping a cleared buffer in keeps the pending list's capacity instead of dropping it.
            Ready.swap(Library->PendingRelease);
        }

        for (FShaderH Handle : Ready)
        {
            uint64 MapHash = 0;
            {
                FScopeLock Lock(Library->Mutex);
                FShaderEntry* Entry = Library->Entries.TryGet(Handle);

                // Re-acquired between Release and here by an identical recompile, so it is live again.
                if (Entry == nullptr || Entry->RefCount != 0)
                {
                    continue;
                }
                MapHash = Entry->MapHash;
            }

            {
                FScopeLock Lock(Library->Mutex);
                auto It = Library->HandlesByHash.find(MapHash);
                if (It != Library->HandlesByHash.end() && It->second.Handle == Handle.Handle)
                {
                    Library->HandlesByHash.erase(It);
                }
            }

            // Bumps the slot generation, which is what makes every outstanding weak handle resolve to null.
            Library->Entries.Erase(Handle);
        }
    }

    FName FShaderLibrary::CanonicalPath(const FName& NameOrPath)
    {
        const char* Str = NameOrPath.c_str();
        if (Str == nullptr || Str[0] == '\0' || Str[0] == '/')
        {
            // Already a full virtual path (or nothing to resolve).
            return NameOrPath;
        }

        // A packaged build strips the source tree, so the name index is its only mapping.
        if (FShaderLibrary* Library = GShaderLibrary)
        {
            FScopeLock Lock(Library->Mutex);
            auto It = Library->PathsByName.find(NameOrPath);
            if (It != Library->PathsByName.end())
            {
                return It->second;
            }
        }

        const FString Resolved = Shaders::Resolve(FStringView(Str));

        // Slang still resolves a bare module name, so this degrades to a compile attempt.
        return Resolved.empty() ? NameOrPath : FName(Resolved);
    }

    void FShaderLibrary::IndexShaderName(const FName& Path)
    {
        const FStringView FullPath(Path.c_str());
        const FStringView File = VFS::FileName(FullPath);
        if (File.empty() || File.size() == FullPath.size())
        {
            return;   // not a path, so there is nothing to index it under
        }

        const FName Name(File);
        auto It = PathsByName.find(Name);
        if (It == PathsByName.end())
        {
            PathsByName.emplace(Name, Path);
            return;
        }

        if (It->second != Path)
        {
            LOG_WARN("Two shader roots ship '{}' ('{}' and '{}'). The first wins by name; request the "
                     "other by its full virtual path.", Name, It->second, Path);
        }
    }

    FShaderH FShaderLibrary::Get(const FName& NameOrPath, TSpan<const FString> Defines)
    {
        LUMINA_MEMORY_SCOPE("Shaders");
        FShaderLibrary* Library = GShaderLibrary;

        // Two roots shipping the same file name stay distinct rather than overwriting each other.
        const FName Path  = CanonicalPath(NameOrPath);
        const uint64 Hash = EntryHash(Path, Defines);

        {
            FScopeLock Lock(Library->Mutex);
            const FShaderH Handle = Library->FindOrCreate(Hash);
            FShaderEntry& Entry   = Library->Entries[Handle];
            if (Entry.IsValid())
            {
                // Engine shaders are process-lifetime, which keeps them out of the release queue entirely.
                ++Entry.RefCount;
                return Handle;
            }
            if (Entry.Path.IsNone())
            {
                Entry.Path = Path;
                Entry.Defines.assign(Defines.begin(), Defines.end());
            }
        }

        FShaderCompileOptions Options;
        Options.bGenerateReflectionData = true;
        Options.MacroDefinitions.assign(Defines.begin(), Defines.end());
        GShaderCompiler->CompileShaderPath(FString(Path.c_str()), Options, [](const FShaderHeader& Header)
        {
            Commit(Header);
        });
        GShaderCompiler->Flush();

        FScopeLock Lock(Library->Mutex);
        const FShaderH Handle = Library->FindOrCreate(Hash);
        ++Library->Entries[Handle].RefCount;
        return Handle;
    }

#if USING(WITH_EDITOR)
    static void ScanSpirvLocalArrays(TSpan<const uint32> Spirv, uint32& OutCount, uint32& OutScalars)
    {
        OutCount   = 0;
        OutScalars = 0;

        // Magic + 4 header words. Anything shorter is not a module we can walk.
        constexpr uint32 kSpirvMagic = 0x07230203u;
        if (Spirv.size() < 5 || Spirv[0] != kSpirvMagic)
        {
            return;
        }

        constexpr uint32 kOpTypeInt     = 21;
        constexpr uint32 kOpTypeFloat   = 22;
        constexpr uint32 kOpTypeVector  = 23;
        constexpr uint32 kOpTypeMatrix  = 24;
        constexpr uint32 kOpTypeArray   = 28;
        constexpr uint32 kOpTypePointer = 32;
        constexpr uint32 kOpConstant    = 43;
        constexpr uint32 kOpVariable    = 59;
        constexpr uint32 kStorageFunction = 7;

        // Result-id keyed side tables. Ids are dense and bounded by the header's id bound (word 3).
        const uint32 IdBound = Spirv[3];
        if (IdBound == 0 || IdBound > (1u << 22))
        {
            return;   // implausible bound; refuse rather than allocate wildly
        }

        TVector<uint32> ScalarsOfType(IdBound, 0);   // scalar slots a type occupies (0 = unknown/not counted)
        TVector<uint32> ConstantValue(IdBound, 0);
        TVector<uint8>  IsArrayType(IdBound, 0);
        TVector<uint32> PointeeOfPointer(IdBound, 0);
        TVector<uint8>  PointerIsFunction(IdBound, 0);

        for (size_t Word = 5; Word < Spirv.size(); )
        {
            const uint32 Instruction = Spirv[Word];
            const uint32 WordCount   = Instruction >> 16;
            const uint32 Opcode      = Instruction & 0xFFFFu;

            // A zero word count would loop forever; a run past the end means the module is truncated.
            if (WordCount == 0 || Word + WordCount > Spirv.size())
            {
                break;
            }

            auto Arg = [&](uint32 N) -> uint32 { return (N < WordCount) ? Spirv[Word + N] : 0u; };
            auto InBounds = [&](uint32 Id) { return Id != 0 && Id < IdBound; };

            switch (Opcode)
            {
                case kOpTypeInt:
                case kOpTypeFloat:
                    if (InBounds(Arg(1))) { ScalarsOfType[Arg(1)] = 1; }
                    break;

                case kOpTypeVector:
                    // result, component type, count
                    if (InBounds(Arg(1))) { ScalarsOfType[Arg(1)] = Arg(3); }
                    break;

                case kOpTypeMatrix:
                    // result, column type, column count
                    if (InBounds(Arg(1)) && InBounds(Arg(2))) { ScalarsOfType[Arg(1)] = ScalarsOfType[Arg(2)] * Arg(3); }
                    break;

                case kOpConstant:
                    // result type, result, literal (only single-word literals matter for array lengths)
                    if (InBounds(Arg(2)) && WordCount >= 4) { ConstantValue[Arg(2)] = Arg(3); }
                    break;

                case kOpTypeArray:
                    // result, element type, length (an id referring to a constant)
                    if (InBounds(Arg(1)))
                    {
                        IsArrayType[Arg(1)] = 1;
                        const uint32 ElementScalars = InBounds(Arg(2)) ? ScalarsOfType[Arg(2)] : 0u;
                        const uint32 Length         = InBounds(Arg(3)) ? ConstantValue[Arg(3)]  : 0u;
                        ScalarsOfType[Arg(1)]       = ElementScalars * Length;
                    }
                    break;

                case kOpTypePointer:
                    // result, storage class, pointee type
                    if (InBounds(Arg(1)))
                    {
                        PointerIsFunction[Arg(1)] = (Arg(2) == kStorageFunction) ? 1u : 0u;
                        PointeeOfPointer[Arg(1)]  = Arg(3);
                    }
                    break;

                case kOpVariable:
                {
                    // result type (a pointer), result, storage class
                    const uint32 PointerType = Arg(1);
                    if (Arg(3) == kStorageFunction && InBounds(PointerType) && PointerIsFunction[PointerType])
                    {
                        const uint32 Pointee = PointeeOfPointer[PointerType];
                        if (InBounds(Pointee) && IsArrayType[Pointee])
                        {
                            OutCount   += 1;
                            OutScalars += ScalarsOfType[Pointee];
                        }
                    }
                    break;
                }

                default:
                    break;
            }

            Word += WordCount;
        }
    }

    FShaderEntry::FGPUStats FShaderLibrary::GetGPUStats(FShaderH Handle)
    {
        FScopeLock Lock(GShaderLibrary->Mutex);
        const FShaderEntry* Entry = GShaderLibrary->Entries.TryGet(Handle);
        return Entry != nullptr ? Entry->GPUStats : FShaderEntry::FGPUStats{};
    }

    bool FShaderLibrary::HasPipelineStats(FShaderH Handle)
    {
        FScopeLock Lock(GShaderLibrary->Mutex);
        const FShaderEntry* Entry = GShaderLibrary->Entries.TryGet(Handle);
        // Nothing to publish against; treat as done so callers skip the query.
        return Entry == nullptr || !Entry->GPUStats.Pipeline.empty();
    }

    void FShaderLibrary::PublishPipelineStats(FShaderH Handle, TVector<RHI::FPipelineStat>&& Stats)
    {
        if (Stats.empty())
        {
            return;
        }
        FScopeLock Lock(GShaderLibrary->Mutex);
        if (FShaderEntry* Entry = GShaderLibrary->Entries.TryGet(Handle))
        {
            Entry->GPUStats.Pipeline = Move(Stats);
        }
    }
#endif

    FShaderH FShaderLibrary::Commit(const FName& Key, ERHIShaderType Type, TSpan<const uint32> Spirv)
    {
        LUMINA_MEMORY_SCOPE("Shaders");
        FShaderLibrary* Library = GShaderLibrary;
        FScopeLock Lock(Library->Mutex);

        // Content keying collapses identical geometry stages and stays safe across a recompile.
        const uint64 ContentHash = SpirvContentHash(Type, Spirv);

        uint64 Slot = ContentHash;
        if (auto It = Library->HandlesByHash.find(ContentHash); It != Library->HandlesByHash.end())
        {
            if (const FShaderEntry* Existing = Library->Entries.TryGet(It->second))
            {
                const bool bIdentical = Existing->Type == Type
                                     && Existing->Spirv.size() == Spirv.size()
                                     && std::memcmp(Existing->Spirv.data(), Spirv.data(),
                                                    Spirv.size() * sizeof(uint32)) == 0;
                if (bIdentical)
                {
                    // The bytecode is unchanged, so every pipeline built from this entry stays valid.
                    const FShaderH Handle = It->second;
                    ++Library->Entries[Handle].RefCount;
                    return Handle;
                }

                // A 64-bit collision. Fall back to the caller's name so the two can never alias.
                Slot = EntryHash(Key, {});
            }
        }

        const FShaderH Handle = Library->FindOrCreate(Slot);
        FShaderEntry& Entry   = Library->Entries[Handle];
        Entry.Path = Key;
        Entry.Type = Type;
        Entry.Spirv.assign(Spirv.begin(), Spirv.end());
        Entry.Generation++;
        ++Entry.RefCount;
#if USING(WITH_EDITOR)
        Entry.GPUStats.Pipeline.clear();
        ScanSpirvLocalArrays(Spirv, Entry.GPUStats.LocalArrayCount, Entry.GPUStats.LocalArrayScalars);
#endif
        return Handle;
    }

    void FShaderLibrary::Commit(const FShaderHeader& Header)
    {
        LUMINA_MEMORY_SCOPE("Shaders");
        FShaderLibrary* Library = GShaderLibrary;
        FScopeLock Lock(Library->Mutex);

        const FName Path(Header.DebugName.c_str());
        const FShaderH Handle = Library->FindOrCreate(EntryHash(Path, TSpan<const FString>(Header.Defines.data(), Header.Defines.size())));
        FShaderEntry& Entry   = Library->Entries[Handle];
        Entry.Path    = Path;
        Entry.Defines = Header.Defines;
        Entry.Type    = Header.Reflection.ShaderType;
        Entry.Spirv   = Header.Binaries;
        Entry.Generation++;

        Library->IndexShaderName(Path);
#if USING(WITH_EDITOR)
        Entry.GPUStats.Pipeline.clear();
        ScanSpirvLocalArrays(TSpan<const uint32>(Entry.Spirv.data(), Entry.Spirv.size()),
                             Entry.GPUStats.LocalArrayCount, Entry.GPUStats.LocalArrayScalars);
#endif
    }
}
