#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/time.h>

#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Containers/Name.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectBase.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/Package/Package.h"
#include "Core/Versioning/CoreVersion.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "FileSystem/FileSystem.h"
#include "GUID/GUID.h"
#include "Log/Log.h"
#include "Platform/Time/PlatformTime.h"
#include "Scripting/ScriptableTest.h"

using namespace Lumina;

// disabled by default, run with --gtest_also_run_disabled_tests --gtest_filter=PackageLoadBenchmark.*
namespace PackageLoadBench
{
    // a sigprof sampler, since perf_event_paranoid can block unprivileged perf, resolve with addr2line
    namespace Sampler
    {
        constexpr int32 kMaxFrames  = 40;
        constexpr int32 kMaxSamples = 120000;

        void*               GFrames[(size_t)kMaxSamples * kMaxFrames];
        int32               GDepths[kMaxSamples];
        std::atomic<int32>  GCount{0};
        bool                GArmed = false;

        extern "C" void OnProf(int)
        {
            const int32 Slot = GCount.fetch_add(1, std::memory_order_relaxed);
            if (Slot >= kMaxSamples)
            {
                return;
            }
            GDepths[Slot] = backtrace(&GFrames[(size_t)Slot * kMaxFrames], kMaxFrames);
        }

        void Start()
        {
            GCount.store(0, std::memory_order_relaxed);

            // backtrace lazily loads its unwinder, which must not happen inside the handler
            void* Warm[4];
            (void)backtrace(Warm, 4);

            struct sigaction Action = {};
            Action.sa_handler = &OnProf;
            sigemptyset(&Action.sa_mask);
            Action.sa_flags = SA_RESTART;
            sigaction(SIGPROF, &Action, nullptr);

            itimerval Timer = {};
            Timer.it_interval.tv_usec = 500;
            Timer.it_value.tv_usec    = 500;
            setitimer(ITIMER_PROF, &Timer, nullptr);

            GArmed = true;
        }

        void Stop()
        {
            if (!GArmed)
            {
                return;
            }
            itimerval Off = {};
            setitimer(ITIMER_PROF, &Off, nullptr);
            signal(SIGPROF, SIG_IGN);
            GArmed = false;
        }

        void Dump(const char* Label)
        {
            const char* OutPath = std::getenv("LUMINA_BENCH_PROFILE_OUT");
            if (OutPath == nullptr)
            {
                return;
            }

            std::FILE* File = std::fopen(OutPath, "a");
            if (File == nullptr)
            {
                return;
            }

            const int32 Num = Math::Min(GCount.load(std::memory_order_relaxed), kMaxSamples);
            std::fprintf(File, "# label %s samples %d\n", Label, Num);

            for (int32 S = 0; S < Num; ++S)
            {
                for (int32 F = 0; F < GDepths[S]; ++F)
                {
                    Dl_info Info = {};
                    if (dladdr(GFrames[(size_t)S * kMaxFrames + F], &Info) == 0 || Info.dli_fname == nullptr)
                    {
                        continue;
                    }
                    const uintptr_t Offset = (uintptr_t)GFrames[(size_t)S * kMaxFrames + F] - (uintptr_t)Info.dli_fbase;
                    std::fprintf(File, "%s+0x%lx ", Info.dli_fname, (unsigned long)Offset);
                }
                std::fputc('\n', File);
            }

            std::fclose(File);
        }
    }

    constexpr const char* kAlias = "/PkgBench";

    // the harness only starts the object system, so the deferred registration pass has to run here
    void EnsureMount()
    {
        static const bool bReady = []
        {
            ProcessNewlyLoadedCObjects();
            VFS::Mount<VFS::FMemoryFileSystem>(kAlias);
            return true;
        }();
        (void)bReady;
    }

    struct FQuietLog
    {
        FQuietLog() : Saved(Logging::GetLevel()) { Logging::SetLevel(ELogLevel::Warn); }
        ~FQuietLog() { Logging::SetLevel(Saved); }
        ELogLevel Saved;
    };

    void Retire(CPackage* Package)
    {
        TVector<CObject*> Objects;
        Objects.reserve(32);
        GetObjectsWithPackage(Package, Objects);

        for (CObject* Object : Objects)
        {
            if (Object != Package)
            {
                Object->ConditionalBeginDestroy();
            }
        }

        Package->ExportTable.clear();
        Package->ImportTable.clear();
        Package->RemoveFromRoot();
        Package->ConditionalBeginDestroy();
    }

    struct FCorpus
    {
        TVector<FFixedString> Paths;
        int64                 FileBytes   = 0;
        int64                 PlainBytes  = 0;
        int32                 NumExports  = 0;
        double                BuildMs     = 0.0;
        double                InflateMs   = 0.0;
    };

    // pins the container's codec and level for the duration, so codecs measure against each other in one run
    struct FForcedCodec
    {
        FForcedCodec(int32 Codec, int32 Level) { SetForcedPackageCodec(Codec, Level); }
        ~FForcedCodec() { SetForcedPackageCodec(-1, 0); }
    };

    // pins saves to the encoding before PACKAGE_NAME_TABLE, so both formats measure in one run
    struct FLegacyFormat
    {
        explicit FLegacyFormat(bool bEnable)
        {
            if (bEnable)
            {
                SetForcedPackageSaveVersion((int32)ELuminaEngineVersion::PACKAGE_NAME_TABLE - 1);
            }
        }

        ~FLegacyFormat() { SetForcedPackageSaveVersion(0); }
    };

    FCorpus BuildCorpus(const char* Tag, int32 NumPackages, int32 ExportsPerPackage, CClass* ExportClass,
                        const char* SaveProfileLabel = nullptr, bool bLegacyNames = false)
    {
        EnsureMount();

        // property lists flatten lazily, and an unlinked class serializes nothing at all
        ExportClass->Link();
        EXPECT_GT(ExportClass->GetProperties().size(), 0u) << "the benchmark would measure an empty serialize";

        FCorpus Corpus;
        FLegacyFormat Legacy(bLegacyNames);
        if (SaveProfileLabel != nullptr)
        {
            Sampler::Start();
        }
        PlatformTime::FStopwatch Watch;

        for (int32 P = 0; P < NumPackages; ++P)
        {
            char Path[256];
            std::snprintf(Path, sizeof(Path), "%s/%s_%d.lasset", kAlias, Tag, P);

            CPackage* Package = CPackage::CreatePackage(Path);
            if (Package == nullptr)
            {
                continue;
            }

            for (int32 E = 0; E < ExportsPerPackage; ++E)
            {
                char Name[128];
                std::snprintf(Name, sizeof(Name), "%s_%d_%d", Tag, P, E);
                NewObject(ExportClass, Package, FName(Name), FGuid::New());
            }

            if (CPackage::SavePackage(Package, Path))
            {
                Corpus.Paths.emplace_back(Path);
                Corpus.FileBytes += (int64)VFS::Size(Path);
                Corpus.NumExports += ExportsPerPackage;
            }

            Retire(Package);
        }

        Corpus.BuildMs = Watch.ElapsedMilliseconds();

        if (SaveProfileLabel != nullptr)
        {
            Sampler::Stop();
            Sampler::Dump(SaveProfileLabel);
        }

        // what the tagged stream costs before deflate, which is what inflate has to rebuild
        for (const FFixedString& Path : Corpus.Paths)
        {
            TVector<uint8> Binary;
            if (CPackage::ReadPackageFile(Path, Binary, nullptr))
            {
                Corpus.PlainBytes += (int64)Binary.size();
            }
        }

        return Corpus;
    }

    struct FPhases
    {
        double ReadAndInflateMs = 0.0;
        double LoadPackageMs    = 0.0;
        double FullyLoadMs      = 0.0;
    };

    void Report(const char* Name, const FCorpus& Corpus, const FPhases& Phases)
    {
        const double Total    = Phases.LoadPackageMs + Phases.FullyLoadMs;
        const int32  NumPkgs  = (int32)Corpus.Paths.size();
        const double PerPkgUs = NumPkgs ? (Total * 1000.0) / NumPkgs : 0.0;
        const double PerExpUs = Corpus.NumExports ? (Total * 1000.0) / Corpus.NumExports : 0.0;

        std::printf("\n[%s] %d packages, %d exports\n", Name, NumPkgs, Corpus.NumExports);
        std::printf("  bytes       %.1f KiB compressed, %.1f KiB tagged stream (%.1fx), %.0f B/export\n",
            (double)Corpus.FileBytes / 1024.0, (double)Corpus.PlainBytes / 1024.0,
            Corpus.FileBytes ? (double)Corpus.PlainBytes / (double)Corpus.FileBytes : 0.0,
            Corpus.NumExports ? (double)Corpus.PlainBytes / Corpus.NumExports : 0.0);
        if (Phases.ReadAndInflateMs > 0.0)
        {
            std::printf("  inflate     %.0f MiB/s of tagged stream\n",
                ((double)Corpus.PlainBytes / (1024.0 * 1024.0)) / (Phases.ReadAndInflateMs / 1000.0));
        }
        std::printf("  save (untimed setup)    %9.2f ms   %8.1f us/pkg\n",
            Corpus.BuildMs, NumPkgs ? (Corpus.BuildMs * 1000.0) / NumPkgs : 0.0);
        std::printf("  read + inflate only     %9.2f ms   %5.1f%% of load\n",
            Phases.ReadAndInflateMs, Total > 0.0 ? 100.0 * Phases.ReadAndInflateMs / Total : 0.0);
        std::printf("  LoadPackage (container) %9.2f ms   %5.1f%%\n",
            Phases.LoadPackageMs, Total > 0.0 ? 100.0 * Phases.LoadPackageMs / Total : 0.0);
        std::printf("  FullyLoad   (objects)   %9.2f ms   %5.1f%%\n",
            Phases.FullyLoadMs, Total > 0.0 ? 100.0 * Phases.FullyLoadMs / Total : 0.0);
        std::printf("  TOTAL                   %9.2f ms   %8.1f us/pkg   %8.2f us/export\n",
            Total, PerPkgUs, PerExpUs);
        std::fflush(stdout);
    }

    FPhases RunColdLoad(const FCorpus& Corpus, const char* ProfileLabel, int32 Reps = 1)
    {
        FPhases Phases;

        {
            PlatformTime::FStopwatch Watch;
            for (const FFixedString& Path : Corpus.Paths)
            {
                TVector<uint8> Binary;
                (void)CPackage::ReadPackageFile(Path, Binary, nullptr);
            }
            Phases.ReadAndInflateMs = Watch.ElapsedMilliseconds();
        }

        if (ProfileLabel != nullptr)
        {
            Sampler::Start();
        }

        for (int32 Rep = 0; Rep < Reps; ++Rep)
        {
            TVector<CPackage*> Packages;
            Packages.reserve(Corpus.Paths.size());
            {
                PlatformTime::FStopwatch Watch;
                for (const FFixedString& Path : Corpus.Paths)
                {
                    Packages.push_back(CPackage::LoadPackage(Path));
                }
                Phases.LoadPackageMs += Watch.ElapsedMilliseconds();
            }

            {
                PlatformTime::FStopwatch Watch;
                for (CPackage* Package : Packages)
                {
                    if (Package != nullptr)
                    {
                        (void)Package->FullyLoad();
                    }
                }
                Phases.FullyLoadMs += Watch.ElapsedMilliseconds();
            }

            for (CPackage* Package : Packages)
            {
                if (Package != nullptr)
                {
                    Retire(Package);
                }
            }
        }

        if (ProfileLabel != nullptr)
        {
            Sampler::Stop();
            Sampler::Dump(ProfileLabel);
        }

        Phases.LoadPackageMs /= Reps;
        Phases.FullyLoadMs   /= Reps;

        return Phases;
    }
}

using namespace PackageLoadBench;

// every codec and level on one machine, which is the only fair way to pick between them
TEST(PackageLoadBenchmark, DISABLED_Codecs)
{
    FQuietLog Quiet;

    struct FCodecCase
    {
        const char* Name;
        int32       Codec;
        int32       Level;
    };

    // codec 0 is deflate, 1 is zstd
    const FCodecCase Cases[] = {
        { "deflate 6 (was)",  0,  6 },
        { "deflate 1",        0,  1 },
        { "zstd 1",           1,  1 },
        { "zstd 3 (now)",     1,  3 },
        { "zstd 9",           1,  9 },
        { "zstd 12 (cook)",   1, 12 },
        { "zstd 19",          1, 19 },
    };

    std::printf("\n%-16s %9s %9s %10s %9s\n", "codec", "save", "load", "on disk", "ratio");

    for (const FCodecCase& Case : Cases)
    {
        char Tag[64];
        std::snprintf(Tag, sizeof(Tag), "Codec_%d_%d", Case.Codec, Case.Level);

        FCorpus Corpus;
        {
            FForcedCodec Forced(Case.Codec, Case.Level);
            Corpus = BuildCorpus(Tag, 40, 64, CParticleEmitter::StaticClass());
        }

        const FPhases Phases = RunColdLoad(Corpus, nullptr, 8);
        const double  Total  = Phases.LoadPackageMs + Phases.FullyLoadMs;

        std::printf("%-16s %7.2fms %7.2fms %8.0fK %8.2fx\n", Case.Name,
            Corpus.BuildMs, Total, (double)Corpus.FileBytes / 1024.0,
            Corpus.FileBytes ? (double)Corpus.PlainBytes / (double)Corpus.FileBytes : 0.0);
        std::fflush(stdout);
    }
}

// both encodings on one machine, the only way to compare them without trusting run-to-run noise
TEST(PackageLoadBenchmark, DISABLED_NameTableVsInlineNames)
{
    FQuietLog Quiet;

    struct FCase
    {
        const char* Name;
        int32       Packages;
        int32       Exports;
        CClass*     Class;
    };

    const FCase Cases[] = {
        { "1 export/pkg,  44 properties", 400, 1,  CParticleEmitter::StaticClass() },
        { "64 exports/pkg, 44 properties", 40, 64, CParticleEmitter::StaticClass() },
        { "1 export/pkg,   1 property",   400, 1,  CScriptableTest::StaticClass()  },
    };

    for (const FCase& Case : Cases)
    {
        char InlineTag[64];
        char TableTag[64];
        std::snprintf(InlineTag, sizeof(InlineTag), "Cmp_Inline_%d_%d_%s", Case.Packages, Case.Exports, Case.Class->GetName().c_str());
        std::snprintf(TableTag, sizeof(TableTag), "Cmp_Table_%d_%d_%s", Case.Packages, Case.Exports, Case.Class->GetName().c_str());

        FCorpus Old = BuildCorpus(InlineTag, Case.Packages, Case.Exports, Case.Class, nullptr, /*bLegacyNames*/ true);
        FCorpus New = BuildCorpus(TableTag, Case.Packages, Case.Exports, Case.Class, nullptr, /*bLegacyNames*/ false);

        const FPhases OldPhases = RunColdLoad(Old, nullptr, 8);
        const FPhases NewPhases = RunColdLoad(New, nullptr, 8);

        const double OldTotal = OldPhases.LoadPackageMs + OldPhases.FullyLoadMs;
        const double NewTotal = NewPhases.LoadPackageMs + NewPhases.FullyLoadMs;

        std::printf("\n%s\n", Case.Name);
        std::printf("  %-14s %8s %8s %9s %9s %9s\n", "", "load", "save", "stream", "on disk", "us/export");
        std::printf("  %-14s %7.2fms %7.2fms %8.0fK %8.0fK %8.2f\n", "inline names",
            OldTotal, Old.BuildMs, (double)Old.PlainBytes / 1024.0, (double)Old.FileBytes / 1024.0,
            (OldTotal * 1000.0) / Old.NumExports);
        std::printf("  %-14s %7.2fms %7.2fms %8.0fK %8.0fK %8.2f\n", "name table",
            NewTotal, New.BuildMs, (double)New.PlainBytes / 1024.0, (double)New.FileBytes / 1024.0,
            (NewTotal * 1000.0) / New.NumExports);
        std::printf("  %-14s %7.2fx %7.2fx %8.2fx %8.2fx\n", "speedup",
            NewTotal > 0.0 ? OldTotal / NewTotal : 0.0,
            New.BuildMs > 0.0 ? Old.BuildMs / New.BuildMs : 0.0,
            New.PlainBytes > 0 ? (double)Old.PlainBytes / (double)New.PlainBytes : 0.0,
            New.FileBytes > 0 ? (double)Old.FileBytes / (double)New.FileBytes : 0.0);
        std::fflush(stdout);
    }
}

// one asset per file, the shape a cold project open actually produces
TEST(PackageLoadBenchmark, DISABLED_OneExportPerPackage)
{
    FQuietLog Quiet;
    FCorpus Corpus = BuildCorpus("Single", 400, 1, CParticleEmitter::StaticClass(), "save-one-export");
    const FPhases Phases = RunColdLoad(Corpus, "one-export-per-package", 20);
    Report("1 export/package, 28 properties", Corpus, Phases);
}

// a composite asset, such as a particle system's emitters, in one file
TEST(PackageLoadBenchmark, DISABLED_ManyExportsPerPackage)
{
    FQuietLog Quiet;
    FCorpus Corpus = BuildCorpus("Many", 40, 64, CParticleEmitter::StaticClass(), "save-many-exports");
    const FPhases Phases = RunColdLoad(Corpus, "many-exports-per-package", 8);
    Report("64 exports/package, 28 properties", Corpus, Phases);
}

// FullyLoad resolves each export by scanning the export table, so per-export cost grows with it
TEST(PackageLoadBenchmark, DISABLED_ExportTableScaling)
{
    FQuietLog Quiet;
    for (const int32 Exports : { 16, 64, 256, 1024, 4096 })
    {
        FCorpus Corpus = BuildCorpus("Scale", 1, Exports, CParticleEmitter::StaticClass());
        const FPhases Phases = RunColdLoad(Corpus, nullptr, 3);
        std::printf("  %5d exports/package:  FullyLoad %8.2f ms  = %7.2f us/export\n",
            Exports, Phases.FullyLoadMs, (Phases.FullyLoadMs * 1000.0) / Exports);
        std::fflush(stdout);
    }
}

// same export count as OneExportPerPackage but one property each, isolating per-property cost
TEST(PackageLoadBenchmark, DISABLED_ThinExports)
{
    FQuietLog Quiet;
    FCorpus Corpus = BuildCorpus("Thin", 400, 1, CScriptableTest::StaticClass());
    const FPhases Phases = RunColdLoad(Corpus, "thin-exports", 20);
    Report("1 export/package, 1 property", Corpus, Phases);
}

TEST(PackageLoadBenchmark, DISABLED_Primitives)
{
    FQuietLog Quiet;

    constexpr int32 N = 200000;

    {
        PlatformTime::FStopwatch Watch;
        FGuid Sink;
        for (int32 i = 0; i < N; ++i)
        {
            Sink = FGuid::New();
        }
        const double Ns = (Watch.ElapsedMilliseconds() * 1e6) / N;
        std::printf("\nFGuid::New()                       %8.1f ns  (guid %u)\n", Ns, (uint32)Sink.IsValid());
    }

    {
        TVector<FFixedString> Strings;
        Strings.reserve(N);
        for (int32 i = 0; i < N; ++i)
        {
            char Buf[64];
            std::snprintf(Buf, sizeof(Buf), "BenchName_%d", i);
            Strings.emplace_back(Buf);
        }

        PlatformTime::FStopwatch Watch;
        for (const FFixedString& S : Strings)
        {
            FName Name(S.c_str());
            (void)Name;
        }
        std::printf("FName(new string)                  %8.1f ns\n", (Watch.ElapsedMilliseconds() * 1e6) / N);

        Watch.Restart();
        for (const FFixedString& S : Strings)
        {
            FName Name(S.c_str());
            (void)Name;
        }
        std::printf("FName(existing string)             %8.1f ns\n", (Watch.ElapsedMilliseconds() * 1e6) / N);
    }

    {
        EnsureMount();
        CClass* Class = CParticleEmitter::StaticClass();
        Class->Link();
        CPackage* Package = CPackage::GetTransientPackage();

        TVector<CObject*> Objects;
        Objects.reserve(20000);

        Sampler::Start();
        PlatformTime::FStopwatch Watch;
        for (int32 i = 0; i < 20000; ++i)
        {
            Objects.push_back(NewObject(Class, Package, NAME_None, FGuid::New(), OF_Transient));
        }
        const double NewObjectNs = (Watch.ElapsedMilliseconds() * 1e6) / 20000;
        Sampler::Stop();
        Sampler::Dump("new-object");
        std::printf("NewObject(CParticleEmitter)        %8.1f ns  (class is %u bytes)\n",
            NewObjectNs, Class->GetSize());

        Watch.Restart();
        for (CObject* Object : Objects)
        {
            (void)FindObjectImpl(Object->GetGUID());
        }
        std::printf("FindObjectImpl(FGuid)              %8.1f ns\n", (Watch.ElapsedMilliseconds() * 1e6) / 20000);

        // one object's tagged properties, write then read, against a plain memory buffer
        CObject* Subject = Objects[0];
        CClass*  Subject_Class = Subject->GetClass();

        TVector<uint8> Buffer;
        {
            FMemoryWriter Writer(Buffer);
            Subject_Class->SerializeTaggedProperties(Writer, Subject);
        }
        // what the tag costs versus what the property values actually are
        size_t PayloadBytes = 0;
        size_t NameChars    = 0;
        THashSet<FName> DistinctNames;
        for (FProperty* Property : Subject_Class->GetProperties())
        {
            if (!Property->ShouldSerialize())
            {
                continue;
            }
            TVector<uint8> One;
            FMemoryWriter Writer(One);
            Property->Serialize(Writer, Property->GetValuePtr<void>(Subject));
            PayloadBytes += One.size();
            NameChars += Property->GetPropertyName().Length() + Property->GetTypeName().Length();
            DistinctNames.insert(Property->GetPropertyName());
            DistinctNames.insert(Property->GetTypeName());
        }

        const size_t NumProps = Subject_Class->GetProperties().size();
        std::printf("reflected properties               %8zu\n", NumProps);
        std::printf("tagged bytes for those properties  %8zu\n", Buffer.size());
        std::printf("  of which property VALUES         %8zu  (%.1f%%)\n",
            PayloadBytes, 100.0 * (double)PayloadBytes / (double)Buffer.size());
        std::printf("  of which name/type TEXT          %8zu\n", NameChars);
        std::printf("  distinct names in the whole class %7zu  (interned %zu times per object)\n",
            DistinctNames.size(), 2 * NumProps);

        constexpr int32 Reps = 20000;

        Watch.Restart();
        for (int32 i = 0; i < Reps; ++i)
        {
            TVector<uint8> Scratch;
            FMemoryWriter Writer(Scratch);
            Subject_Class->SerializeTaggedProperties(Writer, Subject);
        }
        std::printf("SerializeTaggedProperties write    %8.1f ns  (%.1f ns/prop)\n",
            (Watch.ElapsedMilliseconds() * 1e6) / Reps, (Watch.ElapsedMilliseconds() * 1e6) / Reps / (double)NumProps);

        Sampler::Start();
        Watch.Restart();
        for (int32 i = 0; i < Reps; ++i)
        {
            FMemoryReader Reader(Buffer);
            Subject_Class->SerializeTaggedProperties(Reader, Subject);
        }
        const double ReadNs = (Watch.ElapsedMilliseconds() * 1e6) / Reps;
        Sampler::Stop();
        Sampler::Dump("tagged-property-read");

        std::printf("SerializeTaggedProperties read     %8.1f ns  (%.1f ns/prop)\n", ReadNs, ReadNs / (double)NumProps);

        for (CObject* Object : Objects)
        {
            Object->ConditionalBeginDestroy();
        }
    }

    std::fflush(stdout);
}
