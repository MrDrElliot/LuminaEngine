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

    FShaderEntry::FGPUStats FShaderLibrary::GetGPUStats(const FShaderEntry* Entry)
    {
        if (Entry == nullptr)
        {
            return {};
        }
        FScopeLock Lock(GShaderLibrary->Mutex);
        return Entry->GPUStats;
    }

    bool FShaderLibrary::HasPipelineStats(const FShaderEntry* Entry)
    {
        if (Entry == nullptr)
        {
            return true;   // nothing to publish against; treat as done so callers skip the query
        }
        FScopeLock Lock(GShaderLibrary->Mutex);
        return !Entry->GPUStats.Pipeline.empty();
    }

    void FShaderLibrary::PublishPipelineStats(const FShaderEntry* Entry, TVector<RHI::FPipelineStat>&& Stats)
    {
        if (Entry == nullptr || Stats.empty())
        {
            return;
        }
        FScopeLock Lock(GShaderLibrary->Mutex);
        const_cast<FShaderEntry*>(Entry)->GPUStats.Pipeline = Move(Stats);
    }
#endif

    const FShaderEntry* FShaderLibrary::Commit(const FName& Key, ERHIShaderType Type, TSpan<const uint32> Spirv)
    {
        FShaderLibrary* Library = GShaderLibrary;
        FScopeLock Lock(Library->Mutex);

        FShaderEntry& Entry = Library->FindOrCreate(EntryHash(Key, {}));
        Entry.Path = Key;
        Entry.Type = Type;
        Entry.Spirv.assign(Spirv.begin(), Spirv.end());
        Entry.Generation++;
#if USING(WITH_EDITOR)
        Entry.GPUStats.Pipeline.clear();
        ScanSpirvLocalArrays(Spirv, Entry.GPUStats.LocalArrayCount, Entry.GPUStats.LocalArrayScalars);
#endif
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
#if USING(WITH_EDITOR)
        Entry.GPUStats.Pipeline.clear();
        ScanSpirvLocalArrays(TSpan<const uint32>(Entry.Spirv.data(), Entry.Spirv.size()),
                             Entry.GPUStats.LocalArrayCount, Entry.GPUStats.LocalArrayScalars);
#endif
    }
}
