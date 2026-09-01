#include "Platform/Time/PlatformTime.h"
#include "RuntimePCH.h"
#include "ShaderCompiler.h"
#include "ShaderCache.h"
#include "ShaderLibrary.h"
#include "ShaderPaths.h"
#include "RenderResource.h"
#include "RHI.h"
#include "slang-com-ptr.h"
#include "slang.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Utils/Defer.h"
#include "ErrorHandling/CrashTracker.h"
#include "FileSystem/FileSystem.h"
#include "Memory/MemoryTracking.h"
#include "Memory/Memory.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "TaskSystem/TaskSystem.h"
#include "Log/Log.h"

namespace Lumina
{
    IShaderCompiler* GShaderCompiler = nullptr;
    FShaderLibrary*  GShaderLibrary  = nullptr;
    
    static int GetShaderDebugInfoLevel()
    {
        #if WITH_AFTERMATH
        return SLANG_DEBUG_INFO_LEVEL_MAXIMAL;
        #else
        return SLANG_DEBUG_INFO_LEVEL_MINIMAL;
        #endif
    }
    static int GetShaderOptimizationLevel()
    {
        return SLANG_OPTIMIZATION_LEVEL_MAXIMAL;
    }

#if USING(WITH_EDITOR)
    
    static TConsoleVar<int32> CVarValidateShaders(
        "r.Shaders.Validate",
        0,
        "Run spirv-val over every compiled shader (editor only). Requires VULKAN_SDK. "
        "Also enabled for a whole session with -validateshaders.");

    static bool IsShaderValidationEnabled()
    {
        static const bool bForced = GCommandLine != nullptr && GCommandLine->Has("validateshaders");
        return bForced || CVarValidateShaders.GetValue() != 0;
    }

    static constexpr const char* kSpirvValTargetEnv = "vulkan1.4";

    static const FString& GetSpirvValidatorPath()
    {
        static const FString Path = []() -> FString
        {
            const FString SDK = Platform::GetEnvVariable("VULKAN_SDK");
            if (SDK.empty())
            {
                LOG_WARN("Shader validation was requested but VULKAN_SDK is unset, so spirv-val cannot run.");
                return FString();
            }

            #if defined(_WIN32)
            FString Candidate = SDK + "/Bin/spirv-val.exe";
            #else
            FString Candidate = SDK + "/bin/spirv-val";
            #endif

            Paths::Normalize(Candidate);
            if (!Paths::Exists(Candidate))
            {
                LOG_WARN("Shader validation was requested but spirv-val was not found at '{}'.", Candidate.c_str());
                return FString();
            }

            VFS::CreateDir(FShaderCache::kCacheDirectory);

            LOG_TRACE("Shader validation active: {}", Candidate.c_str());
            return Candidate;
        }();

        return Path;
    }

    static void ValidateSpirv(TSpan<const uint32> Spirv, FStringView DebugName)
    {
        if (!IsShaderValidationEnabled() || Spirv.empty())
        {
            return;
        }

        const FString& Validator = GetSpirvValidatorPath();
        if (Validator.empty())
        {
            return;
        }

        static TAtomic<uint32> Serial{0};

        char NameBuf[32];
        snprintf(NameBuf, sizeof(NameBuf), "/spirv-val-%u.spv", Serial.fetch_add(1, std::memory_order_relaxed));

        const FString VirtualPath = FString(FShaderCache::kCacheDirectory) + NameBuf;

        const TSpan<const uint8> Bytes(reinterpret_cast<const uint8*>(Spirv.data()), Spirv.size() * sizeof(uint32));
        if (!VFS::WriteFile(VirtualPath, Bytes))
        {
            return;
        }
        DEFER { VFS::Remove(VirtualPath); };

        const FPathString RealPath = VFS::ResolvePath(VirtualPath);
        const FString Params = FString("--target-env ") + kSpirvValTargetEnv + " \"" + RealPath.c_str() + "\"";

        FString Diagnostic;
        const int ExitCode = Platform::RunProcessAndWaitCapture(UTF8_TO_TCHAR(Validator.c_str()), UTF8_TO_TCHAR(Params.c_str()), nullptr,
            [&Diagnostic](FStringView Line)
            {
                Diagnostic.append(Line.data(), Line.size());
                Diagnostic += '\n';
            });

        if (ExitCode == -1)
        {
            static TAtomic<bool> bWarned{false};
            if (!bWarned.exchange(true, std::memory_order_relaxed))
            {
                LOG_WARN("Could not run spirv-val ('{}'); shader validation is inactive this session.", Validator.c_str());
            }
            return;
        }

        if (ExitCode != 0)
        {
            const FString Name(DebugName.data(), DebugName.size());
            LOG_ERROR("Shader '{}' compiled to INVALID SPIR-V (spirv-val --target-env {}):\n{}",
                      Name.c_str(), kSpirvValTargetEnv, Diagnostic.c_str());
        }
    }

    static void ValidateMaterialTemplates(IShaderCompiler& Compiler)
    {
        if (!IsShaderValidationEnabled() || GetSpirvValidatorPath().empty())
        {
            return;
        }

        static constexpr const char* kPixelToken  = "$MATERIAL_INPUTS";
        static constexpr const char* kPixelStub   = "\tFMaterialPixelInputs Material = DefaultMaterialInputs();\n";
        static constexpr const char* kVertexToken = "$MATERIAL_VERTEX_INPUTS";
        static constexpr const char* kVertexStub  = "\tMaterial.WorldPositionOffset = float3(0.0);\n";

        uint32 Submitted = 0;
        VFS::DirectoryIterator("/Engine/Resources/Shaders/MaterialShader", [&](const VFS::FFileInfo& Info)
        {
            if (Info.GetExt() != ".slang")
            {
                return;
            }

            FString Source;
            if (!VFS::ReadFile(Source, Info.VirtualPath.c_str()))
            {
                return;
            }

            // First occurrence only, matching the material compiler. DeferredMaterial.slang carries both.
            if (const size_t Pos = Source.find(kVertexToken); Pos != FString::npos)
            {
                Source.replace(Pos, strlen(kVertexToken), kVertexStub);
            }
            if (const size_t Pos = Source.find(kPixelToken); Pos != FString::npos)
            {
                Source.replace(Pos, strlen(kPixelToken), kPixelStub);
            }

            FShaderCompileOptions Options;
            Options.bGenerateReflectionData = false;
            Options.DebugName = FString(VFS::FileName(Info.VirtualPath.c_str(), true)) + " (template)";

            Compiler.CompilerShaderRaw(Move(Source), Options, [](const FShaderHeader&) {});
            ++Submitted;
        });

        if (Submitted > 0)
        {
            LOG_INFO("Validating {} material template(s) against spirv-val.", Submitted);
        }
    }

#endif

    // Slang's interfaces are COM, so lifetime runs through release rather than a base pointer delete.
    #if defined(__GNUC__)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
    #endif

    class FSlangBlob : public ISlangBlob
    {
    public:
        FSlangBlob(TVector<uint8>&& InData)
            : Data(std::move(InData)), RefCount(1) {}
    
        virtual ~FSlangBlob() = default;
        
        void const* getBufferPointer() noexcept override { return (void*)Data.data(); }
        size_t getBufferSize() noexcept override { return Data.size(); }
    
        SlangResult queryInterface(const SlangUUID&, void**) noexcept override { return SLANG_E_NO_INTERFACE; }
    
        uint32_t addRef() noexcept override { return ++RefCount; }
        uint32_t release() noexcept override
        {
            uint32_t rc = --RefCount;
            if (rc == 0)
            {
                delete this;
            }
            return rc;
        }
    
    private:
        TVector<uint8> Data;
        std::atomic<uint32_t> RefCount;
    };
    
    class FShaderFS : public ISlangFileSystem
    {
    public:
        SlangResult loadFile(const char* path, ISlangBlob** outBlob) override
        {
            FString ActualPath{path};
    
            TVector<uint8> Data;
            if (!VFS::ReadFile(Data, ActualPath))
            {
                return SLANG_FAIL;
            }
    
            *outBlob = new FSlangBlob(std::move(Data));
            return SLANG_OK;
        }
    
        SlangResult queryInterface(const SlangUUID&, void**) noexcept override { return SLANG_E_NO_INTERFACE; }
    
        uint32_t addRef() noexcept override { return 1; }
        uint32_t release() noexcept override { return 1; }
    
        void* castAs(const SlangUUID&) noexcept override { return nullptr; }
    };

    #if defined(__GNUC__)
        #pragma GCC diagnostic pop
    #endif

    static FShaderFS FileSystem;

    class FSlangSessionPool
    {
    public:
        Slang::ComPtr<slang::IGlobalSession> Acquire()
        {
            {
                FScopeLock Lock(Mutex);
                if (!Free.empty())
                {
                    Slang::ComPtr<slang::IGlobalSession> Session = Move(Free.back());
                    Free.pop_back();
                    return Session;
                }
            }

            Slang::ComPtr<slang::IGlobalSession> Session;
            {
                FScopeLock Lock(CreateMutex);
                slang::createGlobalSession(Session.writeRef());
            }
            return Session;
        }

        void Release(Slang::ComPtr<slang::IGlobalSession>&& Session)
        {
            if (!Session)
            {
                return;
            }
            FScopeLock Lock(Mutex);
            Free.push_back(Move(Session));
        }

        void TrimIdle(uint32 KeepCount)
        {
            TVector<Slang::ComPtr<slang::IGlobalSession>> Dead;
            {
                FScopeLock Lock(Mutex);
                if (Free.size() <= KeepCount)
                {
                    return;
                }

                Dead.reserve(Free.size() - KeepCount);
                while (Free.size() > KeepCount)
                {
                    Dead.push_back(Move(Free.back()));
                    Free.pop_back();
                }
            }

            FScopeLock Lock(CreateMutex);
            Dead.clear();
        }

    private:
        FMutex Mutex;
        FMutex CreateMutex;
        TVector<Slang::ComPtr<slang::IGlobalSession>> Free;
    };

    static FSlangSessionPool GSlangSessionPool;

    // Invariant for a whole batch, where this used to be rebuilt once per shader.
    static TVector<FString> BuildShaderSearchRoots()
    {
        TVector<FString> Roots;
        Shaders::GetSearchRoots(Roots);
        return Roots;
    }

    // Without the leading own-directory entry a game shader would compile the engine's file.
    static void BuildModuleSearchPaths(FStringView ShaderPath, const TVector<FString>& SharedRoots, TVector<FString>& OutPaths)
    {
        const FStringView OwnDir = VFS::Parent(ShaderPath, true);

        OutPaths.clear();
        OutPaths.reserve(SharedRoots.size() + 1);
        if (!OwnDir.empty())
        {
            OutPaths.emplace_back(OwnDir.data(), OwnDir.size());
        }

        for (const FString& Root : SharedRoots)
        {
            if (OutPaths.empty() || Root != OutPaths[0])
            {
                OutPaths.push_back(Root);
            }
        }
    }

    /** Backing storage for the pointers a SessionDesc holds; must outlive the createSession call. */
    struct FSessionScratch
    {
        TVector<const char*>                    SearchPaths;
        TVector<FString>                        MacroSplits;
        TVector<slang::PreprocessorMacroDesc>   Macros;
    };

    // Macros are baked into the description, so a session is reusable only across a shared macro set.
    static Slang::ComPtr<slang::ISession> CreateCompileSession(slang::IGlobalSession* GlobalSession,
        const TVector<FString>& SearchRoots, const TVector<FString>& MacroDefinitions, FSessionScratch& Scratch)
    {
        slang::SessionDesc SessionDesc = {};
        SessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
        SessionDesc.fileSystem = &FileSystem;

        slang::TargetDesc TargetDesc = {};
        TargetDesc.format  = SLANG_SPIRV;
        TargetDesc.profile = GlobalSession->findProfile("spirv_1_5");
        TargetDesc.flags   = SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY | SLANG_TARGET_FLAG_GENERATE_WHOLE_PROGRAM;

        // Declaring the capability silences warning 41012, and the emitted SPIR-V is identical.
        const char* const WaveCapabilities[] = { "spvGroupNonUniformShuffle", "spvGroupNonUniformVote",
                                                 "spvGroupNonUniformBallot", "spvGroupNonUniformArithmetic" };

        slang::CompilerOptionEntry TargetOptions[2 + std::size(WaveCapabilities)] = {};
        TargetOptions[0].name = slang::CompilerOptionName::DebugInformation;
        TargetOptions[0].value.kind = slang::CompilerOptionValueKind::Int;
        TargetOptions[0].value.intValue0 = GetShaderDebugInfoLevel();
        TargetOptions[1].name = slang::CompilerOptionName::Optimization;
        TargetOptions[1].value.kind = slang::CompilerOptionValueKind::Int;
        TargetOptions[1].value.intValue0 = GetShaderOptimizationLevel();
        uint32 TargetOptionCount = 2;

        for (const char* CapabilityName : WaveCapabilities)
        {
            const SlangCapabilityID Capability = GlobalSession->findCapability(CapabilityName);
            if (Capability != SLANG_CAPABILITY_UNKNOWN)
            {
                TargetOptions[TargetOptionCount].name = slang::CompilerOptionName::Capability;
                TargetOptions[TargetOptionCount].value.kind = slang::CompilerOptionValueKind::Int;
                TargetOptions[TargetOptionCount].value.intValue0 = (int)Capability;
                ++TargetOptionCount;
            }
        }

        TargetDesc.compilerOptionEntries = TargetOptions;
        TargetDesc.compilerOptionEntryCount = TargetOptionCount;

        // Silences the unbounded descriptor array warning, which is intentional for bindless.
        slang::CompilerOptionEntry SessionOptions[1] = {};
        SessionOptions[0].name = slang::CompilerOptionName::DisableWarnings;
        SessionOptions[0].value.kind = slang::CompilerOptionValueKind::String;
        SessionOptions[0].value.stringValue0 = "39001";
        SessionDesc.compilerOptionEntries = SessionOptions;
        SessionDesc.compilerOptionEntryCount = 1;

        SessionDesc.targets     = &TargetDesc;
        SessionDesc.targetCount = 1;

        Scratch.SearchPaths.clear();
        Scratch.SearchPaths.reserve(SearchRoots.size());
        for (const FString& Root : SearchRoots)
        {
            Scratch.SearchPaths.push_back(Root.c_str());
        }
        SessionDesc.searchPaths     = Scratch.SearchPaths.data();
        SessionDesc.searchPathCount = (SlangInt)Scratch.SearchPaths.size();

        // Macros hold pointers into the split storage, so a reallocation would dangle every one pushed.
        Scratch.MacroSplits.clear();
        Scratch.Macros.clear();
        Scratch.MacroSplits.reserve(MacroDefinitions.size() * 2);
        Scratch.Macros.reserve(MacroDefinitions.size());
        for (const FString& Macro : MacroDefinitions)
        {
            const size_t SeparatorPos = Macro.find('=');
            if (SeparatorPos != FString::npos)
            {
                Scratch.MacroSplits.emplace_back(Macro.substr(0, SeparatorPos));
                Scratch.MacroSplits.emplace_back(Macro.substr(SeparatorPos + 1));
                Scratch.Macros.push_back({ Scratch.MacroSplits[Scratch.MacroSplits.size() - 2].c_str(),
                                           Scratch.MacroSplits.back().c_str() });
            }
            else
            {
                Scratch.Macros.push_back({ Macro.c_str(), "1" });
            }
        }
        SessionDesc.preprocessorMacros     = Scratch.Macros.data();
        SessionDesc.preprocessorMacroCount = (SlangInt)Scratch.Macros.size();

        Slang::ComPtr<slang::ISession> Session;
        if (SLANG_FAILED(GlobalSession->createSession(SessionDesc, Session.writeRef())))
        {
            LOG_ERROR("Slang: failed to create session");
            return {};
        }

        return Session;
    }

    static ERHIShaderType ToRHIShaderType(SlangStage Stage)
    {
        switch (Stage)
        {
        case SLANG_STAGE_VERTEX:    return ERHIShaderType::Vertex;
        case SLANG_STAGE_GEOMETRY:  return ERHIShaderType::Geometry;
        case SLANG_STAGE_FRAGMENT:  return ERHIShaderType::Fragment;
        case SLANG_STAGE_COMPUTE:   return ERHIShaderType::Compute;
        case SLANG_STAGE_MESH:      return ERHIShaderType::Mesh;
        case SLANG_STAGE_DISPATCH:  return ERHIShaderType::Task;
        default:                    return ERHIShaderType::Vertex;
        }
    }

    static FString DescribeEntryPoints(slang::IModule* Module, SlangInt32 Count)
    {
        FString Names;
        for (SlangInt32 i = 0; i < Count; ++i)
        {
            Slang::ComPtr<slang::IEntryPoint> EntryPoint;
            Module->getDefinedEntryPoint(i, EntryPoint.writeRef());

            const char* Name = nullptr;
            if (EntryPoint)
            {
                slang::ProgramLayout* Layout = EntryPoint->getLayout(0, nullptr);
                if (Layout != nullptr && Layout->getEntryPointCount() > 0)
                {
                    Name = Layout->getEntryPointByIndex(0)->getName();
                }
            }

            if (!Names.empty())
            {
                Names += ", ";
            }
            Names += Name != nullptr ? Name : "<unnamed>";
        }
        return Names;
    }

    // Shared so the two entry points cannot drift, as they did over the mesh and task stages.
    static bool BuildShaderFromModule(slang::IModule* Module, FStringView DebugName,
        const FShaderCompileOptions& Options, FShaderHeader& OutHeader)
    {
        const SlangInt32 EntryPointCount = Module->getDefinedEntryPointCount();
        if (EntryPointCount == 0)
        {
            LOG_ERROR("Slang: no entry points found in '{}'", DebugName);
            return false;
        }

        // One entry point per module. Concatenating several produced a blob that was valid SPIR-V for none.
        Slang::ComPtr<slang::IEntryPoint> EntryPoint;

        if (!Options.EntryPoint.empty())
        {
            Module->findEntryPointByName(Options.EntryPoint.c_str(), EntryPoint.writeRef());

            if (!EntryPoint)
            {
                LOG_ERROR("Slang: '{}' has no entry point named '{}'. It defines: {}",
                    DebugName, Options.EntryPoint, DescribeEntryPoints(Module, EntryPointCount));
                return false;
            }
        }
        else if (EntryPointCount > 1)
        {
            LOG_ERROR("Slang: '{}' defines {} entry points ({}), so FShaderCompileOptions::EntryPoint must name one",
                DebugName, EntryPointCount, DescribeEntryPoints(Module, EntryPointCount));
            return false;
        }
        else
        {
            Module->getDefinedEntryPoint(0, EntryPoint.writeRef());

            if (!EntryPoint)
            {
                LOG_ERROR("Slang: failed to retrieve the only entry point of '{}'", DebugName);
                return false;
            }
        }

        slang::IComponentType* Components[] = { Module, EntryPoint.get() };

        slang::ISession* Session = Module->getSession();

        Slang::ComPtr<slang::IBlob> Diagnostics;
        Slang::ComPtr<slang::IComponentType> LinkedProgram;
        if (SLANG_FAILED(Session->createCompositeComponentType(Components, 2,
                LinkedProgram.writeRef(), Diagnostics.writeRef())))
        {
            if (Diagnostics)
            {
                LOG_ERROR("Slang link error in '{}': {}", DebugName, (const char*)Diagnostics->getBufferPointer());
            }
            LOG_ERROR("Slang: failed to link '{}'", DebugName);
            return false;
        }

        Slang::ComPtr<slang::IBlob> Code;
        Diagnostics = nullptr;

        if (SLANG_FAILED(LinkedProgram->getEntryPointCode(0, 0, Code.writeRef(), Diagnostics.writeRef())))
        {
            if (Diagnostics)
            {
                LOG_ERROR("Slang compile error in '{}': {}", DebugName, (const char*)Diagnostics->getBufferPointer());
            }
            LOG_ERROR("Slang: failed to get SPIR-V for '{}'", DebugName);
            return false;
        }

        if (Diagnostics)
        {
            LOG_WARN("Slang: {}", (const char*)Diagnostics->getBufferPointer());
        }

        const uint32* SpirvData = static_cast<const uint32*>(Code->getBufferPointer());
        const size_t  SpirvSize = Code->getBufferSize() / sizeof(uint32);

        #if USING(WITH_EDITOR)
        ValidateSpirv(TSpan<const uint32>(SpirvData, SpirvSize), DebugName);
        #endif

        TVector<uint32> Binaries(SpirvData, SpirvData + SpirvSize);

        if (Binaries.empty())
        {
            LOG_ERROR("Slang: '{}' compiled to empty SPIR-V", DebugName);
            return false;
        }

        OutHeader.DebugName = FString(DebugName.data(), DebugName.size());
        OutHeader.Hash      = Hash::GetHash64(Binaries);
        OutHeader.Binaries  = Move(Binaries);
        OutHeader.Defines   = Options.MacroDefinitions;

        slang::ProgramLayout* ProgramLayout = LinkedProgram->getLayout();
        if (ProgramLayout != nullptr && ProgramLayout->getEntryPointCount() > 0)
        {
            OutHeader.Reflection.ShaderType = ToRHIShaderType(ProgramLayout->getEntryPointByIndex(0)->getStage());
        }

        return true;
    }

    bool FSpirVShaderCompiler::HasPendingRequests() const
    {
        return PendingTasks.load(std::memory_order_acquire) > 0;
    }

    void FSpirVShaderCompiler::Flush() const
    {
        uint32 Expected = PendingTasks.load(std::memory_order_acquire);
        while (Expected != 0) 
        {
            std::atomic_wait(&PendingTasks, Expected);
            Expected = PendingTasks.load(std::memory_order_acquire);
        }
    }

    bool FSpirVShaderCompiler::CompileShaderPath(FString ShaderPath, const FShaderCompileOptions& CompileOptions, CompletedFunc OnCompleted)
    {
        TVector ShaderPaths = { Move(ShaderPath) };
        TVector Options = { CompileOptions };

        return CompileShaderPaths(TSpan<FString>(ShaderPaths), TSpan<FShaderCompileOptions>(Options), Move(OnCompleted));
    }

    bool FSpirVShaderCompiler::CompileShaderPaths(TSpan<FString> ShaderPaths, TSpan<FShaderCompileOptions> CompileOptions, CompletedFunc OnCompleted)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Shaders");

        ASSERT(ShaderPaths.size() == CompileOptions.size());

        uint32 NumInputs = (uint32)ShaderPaths.size();
        if (NumInputs == 0)
        {
            return false;
        }

        // The cache pass serves hits inline and queues misses for the Slang task swarm.
        TVector<FString> Paths;
        TVector<FShaderCompileOptions> Options;
        TVector<uint64> SourceHashes;
        Paths.reserve(NumInputs);
        Options.reserve(NumInputs);
        SourceHashes.reserve(NumInputs);

        // Hoisted, since rebuilding per shader costs a plugin walk and an existence check per root.
        const TVector<FString> CacheSearchRoots = BuildShaderSearchRoots();

        uint32 NumHits = 0;
        for (uint32 i = 0; i < NumInputs; ++i)
        {
            const uint64 SrcHash = FShaderCache::ComputeSourceSetHash(ShaderPaths[i], CompileOptions[i].MacroDefinitions, CacheSearchRoots, CompileOptions[i].EntryPoint);
            FShaderHeader Cached;
            if (SrcHash != 0 && FShaderCache::TryLoad(ShaderPaths[i], CompileOptions[i].MacroDefinitions, CompileOptions[i].EntryPoint, SrcHash, Cached))
            {
                RHI::GetCrashTracker().RegisterShader(Cached.Binaries, Cached.DebugName);
                OnCompleted(Move(Cached));
                ++NumHits;
                continue;
            }

            Paths.emplace_back(ShaderPaths[i]);
            Options.emplace_back(CompileOptions[i]);
            SourceHashes.push_back(SrcHash);
        }

        if (NumHits > 0)
        {
            LOG_INFO("Shader cache: {} hit, {} miss", NumHits, (uint32)Paths.size());
        }

        const uint32 NumShaders = (uint32)Paths.size();
        if (NumShaders == 0)
        {
            return true;
        }

        PendingTasks.fetch_add(NumShaders, std::memory_order_relaxed);

        LOG_INFO("Starting Shader Task Swarm - Num: {}", NumShaders);

        const uint32 TargetChunks = Math::Min(NumShaders, Math::Max(1u, Threading::GetNumThreads() / 2));
        const uint32 Grain        = (NumShaders + TargetChunks - 1) / TargetChunks;

        Task::AsyncTask(NumShaders, Grain, [this,
            Paths = Move(Paths),
            Options = Move(Options),
            SourceHashes = Move(SourceHashes),
            Callback = Move(OnCompleted)] (uint32 Start, uint32 End, uint32 Thread) mutable
        {
            LUMINA_MEMORY_SCOPE("Shaders");

            uint32 Num = End - Start;

            DEFER
            {
                const bool bLastChunk = PendingTasks.fetch_sub(Num, std::memory_order_acq_rel) == Num;
                std::atomic_notify_all(&PendingTasks);

                if (bLastChunk)
                {
                    GSlangSessionPool.TrimIdle(1);
                }
            };

            Slang::ComPtr<slang::IGlobalSession> GlobalSession = GSlangSessionPool.Acquire();
            DEFER { GSlangSessionPool.Release(Move(GlobalSession)); };

            const TVector<FString> SearchRoots = BuildShaderSearchRoots();
            FSessionScratch Scratch;
            TVector<FString> ModuleSearchPaths;

            for (uint32 i = Start; i < End; ++i)
            {
                // It used to return, abandoning later shaders while still decrementing the pending count for all.
                const uint64 CompileStart = PlatformTime::Cycles();

                const FString&    Path     = Paths[i];
                const FStringView FileName = VFS::FileName(Path);

                BuildModuleSearchPaths(Path, SearchRoots, ModuleSearchPaths);

                Slang::ComPtr<slang::ISession> Session =
                    CreateCompileSession(GlobalSession, ModuleSearchPaths, Options[i].MacroDefinitions, Scratch);
                if (!Session)
                {
                    continue;
                }

                Slang::ComPtr<slang::IBlob>   Diagnostics;
                Slang::ComPtr<slang::IModule> SlangModule;
                SlangModule = Session->loadModule(FileName.data(), Diagnostics.writeRef());

                if (Diagnostics)
                {
                    LOG_WARN("Slang diagnostics for '{}': {}", Path, (const char*)Diagnostics->getBufferPointer());
                }

                if (!SlangModule)
                {
                    LOG_ERROR("Slang: failed to load shader module '{}'", Path);
                    continue;
                }

                // Two roots may ship the same file name, and the library keys on whatever lands in DebugName.
                FShaderHeader Shader;
                if (!BuildShaderFromModule(SlangModule, Path, Options[i], Shader))
                {
                    continue;
                }

                const double DurationMs = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - CompileStart);

                LOG_TRACE("Compiled {0} in {1:.2f} ms (Thread {2})", Path, DurationMs, Thread);

                RHI::GetCrashTracker().RegisterShader(Shader.Binaries, Shader.DebugName);

                FShaderCache::Save(Paths[i], Options[i].MacroDefinitions, Options[i].EntryPoint, SourceHashes[i], Shader);

                Callback(Move(Shader));
            }

        }, ETaskPriority::High);
    
        return true;
    }

    FSpirVShaderCompiler::FSpirVShaderCompiler()
    {
    }

    void FSpirVShaderCompiler::Initialize()
    {
        // The project and its plugins mount later and are picked up by the second precompile call.
        if (Shaders::PrecompileNewRoots() == 0)
        {
            uint32 Loaded = 0;
            VFS::DirectoryIterator(FShaderCache::kCacheDirectory, [&](const VFS::FFileInfo& Info)
            {
                if (Info.GetExt() != ".lsc")
                {
                    return;
                }
                FShaderHeader Header;
                if (!FShaderCache::TryLoadByCachePath(Info.VirtualPath.c_str(), 0, Header))
                {
                    LOG_WARN("Shader cache: failed to load {}", Info.VirtualPath.c_str());
                    return;
                }
                RHI::GetCrashTracker().RegisterShader(Header.Binaries, Header.DebugName);
                FShaderLibrary::Commit(Header);
                ++Loaded;
            });
            LOG_INFO("Shader cache: loaded {} packaged shaders (no source available).", Loaded);
        }

        #if USING(WITH_EDITOR)
        ValidateMaterialTemplates(*this);
        #endif
    }

    void FSpirVShaderCompiler::Shutdown()
    {
        Flush();

        GSlangSessionPool.TrimIdle(0);
    }

    bool FSpirVShaderCompiler::CompilerShaderRaw(FString ShaderString, const FShaderCompileOptions& CompileOptions, CompletedFunc OnCompleted)
    {
        for (size_t Pos = ShaderString.find("#pragma once"); Pos != FString::npos; Pos = ShaderString.find("#pragma once", Pos))
        {
            size_t LineEnd = ShaderString.find('\n', Pos);
            if (LineEnd == FString::npos)
            {
                LineEnd = ShaderString.size();
            }
            else
            {
                ++LineEnd;
            }
            ShaderString.erase(Pos, LineEnd - Pos);
        }

        PendingTasks.fetch_add(1, std::memory_order_relaxed);
        
        Task::AsyncTask(1, 1, [this,
            ShaderString = Move(ShaderString),
            CompileOptions = Move(CompileOptions),
            Callback = Move(OnCompleted)]
            (uint32, uint32, uint32 Thread)
        {
            // Declared before the cache probe, so a hit still releases the slot after its callback ran.
            DEFER
            {
                const bool bLast = PendingTasks.fetch_sub(1, std::memory_order_acq_rel) == 1;
                std::atomic_notify_all(&PendingTasks);

                if (bLast)
                {
                    GSlangSessionPool.TrimIdle(1);
                }
            };

            // This used to hardcode the engine tree, so a graph could not include a plugin's shader header.
            const TVector<FString> SearchRoots = BuildShaderSearchRoots();

            const uint64 CacheKey = FShaderCache::ComputeRawSourceHash(
                ShaderString, CompileOptions.MacroDefinitions, SearchRoots, CompileOptions.TemplateVirtualPath,
                CompileOptions.EntryPoint);

            if (FShaderHeader Cached; FShaderCache::TryLoadRaw(CacheKey, Cached))
            {
                // A cached binary still has to reach the crash tracker, or it resolves as unknown.
                RHI::GetCrashTracker().RegisterShader(Cached.Binaries, Cached.DebugName);
                Callback(Move(Cached));
                return;
            }

            Slang::ComPtr<slang::IGlobalSession> GlobalSession = GSlangSessionPool.Acquire();
            DEFER { GSlangSessionPool.Release(Move(GlobalSession)); };

            const uint64 CompileStart = PlatformTime::Cycles();

            FSessionScratch Scratch;

            Slang::ComPtr<slang::ISession> Session =
                CreateCompileSession(GlobalSession, SearchRoots, CompileOptions.MacroDefinitions, Scratch);
            if (!Session)
            {
                return;
            }

            const FString& RawName    = CompileOptions.DebugName.empty() ? FString("RawShader") : CompileOptions.DebugName;
            const FString  SourcePath = RawName + ".slang";

            Slang::ComPtr<slang::IBlob>   Diagnostics;
            Slang::ComPtr<slang::IModule> SlangModule;
            SlangModule = Session->loadModuleFromSourceString(
                RawName.c_str(), SourcePath.c_str(), ShaderString.c_str(), Diagnostics.writeRef());

            if (Diagnostics)
            {
                LOG_WARN("Slang diagnostics for '{}': {}", RawName, (const char*)Diagnostics->getBufferPointer());
            }

            if (!SlangModule)
            {
                LOG_ERROR("Slang: failed to load raw shader module '{}'", RawName);
                return;
            }

            FShaderHeader Shader;
            if (!BuildShaderFromModule(SlangModule, RawName, CompileOptions, Shader))
            {
                return;
            }

            const double DurationMs = PlatformTime::ToMilliseconds(PlatformTime::Cycles() - CompileStart);

            LOG_TRACE("Compiled raw shader '{0}' in {1:.2f} ms (Thread {2})", RawName, DurationMs, Thread);

            FShaderCache::SaveRaw(CacheKey, Shader);

            RHI::GetCrashTracker().RegisterShader(Shader.Binaries, Shader.DebugName);

            Callback(Move(Shader));
        });

        return true;
    }
}
