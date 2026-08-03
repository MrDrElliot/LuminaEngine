#include "RuntimePCH.h"
#include "ShaderCompiler.h"
#include "ShaderCache.h"
#include "ShaderLibrary.h"
#include "RenderResource.h"
#include "RHI.h"
#include "slang-com-ptr.h"
#include "slang.h"
#include "Core/Plugin/Plugin.h"
#include "Core/Plugin/PluginManager.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Utils/Defer.h"
#include "ErrorHandling/CrashTracker.h"
#include "FileSystem/FileSystem.h"
#include "Memory/Memory.h"
#include "Paths/Paths.h"
#include "TaskSystem/TaskSystem.h"
#include "Log/Log.h"

namespace Lumina
{
    IShaderCompiler* GShaderCompiler = nullptr;
    FShaderLibrary*  GShaderLibrary  = nullptr;
    
    static int GetShaderDebugInfoLevel()
    {
        return SLANG_DEBUG_INFO_LEVEL_MINIMAL;
    }
    static int GetShaderOptimizationLevel()
    {
        return SLANG_OPTIMIZATION_LEVEL_MAXIMAL;
    }

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
    
    static FShaderFS FileSystem;

    // Slang's global session is NOT thread-safe: objects created from one may only be touched by a
    // single thread at a time (see slang.h IGlobalSession docs). To compile in parallel we keep a
    // pool of global sessions and lend one to each compile task for its lifetime; the lock is only
    // held for the brief borrow/return, never across the (hundreds-of-ms) compile itself.
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

            // Grow on demand. createGlobalSession loads the Slang core module and touches
            // process-global state, so serialize creation behind its own lock.
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

        void Prewarm(uint32 Count)
        {
            TVector<Slang::ComPtr<slang::IGlobalSession>> Sessions;
            Sessions.reserve(Count);
            for (uint32 i = 0; i < Count; ++i)
            {
                Sessions.push_back(Acquire());
            }
            for (auto& Session : Sessions)
            {
                Release(Move(Session));
            }
        }

    private:
        FMutex Mutex;
        FMutex CreateMutex;
        TVector<Slang::ComPtr<slang::IGlobalSession>> Free;
    };

    static FSlangSessionPool GSlangSessionPool;
    
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

        return CompileShaderPaths(TSpan(ShaderPaths), TSpan(Options), Move(OnCompleted));
    }

    bool FSpirVShaderCompiler::CompileShaderPaths(TSpan<FString> ShaderPaths, TSpan<FShaderCompileOptions> CompileOptions, CompletedFunc OnCompleted)
    {
        LUMINA_PROFILE_SCOPE();

        ASSERT(ShaderPaths.size() == CompileOptions.size());

        uint32 NumInputs = (uint32)ShaderPaths.size();
        if (NumInputs == 0)
        {
            return false;
        }

        // Cache pass: serve hits inline, queue misses for the Slang task swarm.
        TVector<FString> Paths;
        TVector<FShaderCompileOptions> Options;
        TVector<uint64> SourceHashes;
        Paths.reserve(NumInputs);
        Options.reserve(NumInputs);
        SourceHashes.reserve(NumInputs);

        uint32 NumHits = 0;
        for (uint32 i = 0; i < NumInputs; ++i)
        {
            const uint64 SrcHash = FShaderCache::ComputeSourceSetHash(ShaderPaths[i], CompileOptions[i].MacroDefinitions);
            FShaderHeader Cached;
            if (SrcHash != 0 && FShaderCache::TryLoad(ShaderPaths[i], CompileOptions[i].MacroDefinitions, SrcHash, Cached))
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

        // Spread the work across workers instead of one serial chunk. Bound concurrency so we don't
        // spin up a global session (each loads the Slang core module) per shader; a handful of
        // sessions, each compiling a few shaders, is the sweet spot.
        const uint32 TargetChunks = std::min(NumShaders, std::max(1u, Threading::GetNumThreads() / 2));
        const uint32 Grain        = (NumShaders + TargetChunks - 1) / TargetChunks;

        Task::AsyncTask(NumShaders, Grain, [this,
            Paths = Move(Paths),
            Options = Move(Options),
            SourceHashes = Move(SourceHashes),
            Callback = Move(OnCompleted)] (uint32 Start, uint32 End, uint32 Thread) mutable
        {

            uint32 Num = End - Start;

            DEFER
            {
                PendingTasks.fetch_sub(Num, std::memory_order_relaxed);
                std::atomic_notify_all(&PendingTasks);
            };

            // Borrow one global session for this whole chunk (reused across its shaders) and return
            // it when done. Slang explicitly supports re-using one global session for many sessions.
            Slang::ComPtr<slang::IGlobalSession> GlobalSession = GSlangSessionPool.Acquire();
            DEFER { GSlangSessionPool.Release(Move(GlobalSession)); };

            auto CompileStart = std::chrono::high_resolution_clock::now();

            for (uint32 i = Start; i < End; ++i)
            {
                slang::SessionDesc SessionDesc = {};
                SessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
                SessionDesc.fileSystem = &FileSystem;
        
                slang::TargetDesc TargetDesc = {};
                TargetDesc.format  = SLANG_SPIRV;
                TargetDesc.profile = GlobalSession->findProfile("spirv_1_5");
                TargetDesc.flags   = SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY | SLANG_TARGET_FLAG_GENERATE_WHOLE_PROGRAM;

                // Debug-info level is GPU/build-gated (see GetShaderDebugInfoLevel): STANDARD for Nsight
                // source debugging on non-AMD non-Shipping, MINIMAL elsewhere (AMDVLK STANDARD crash).
                // Optimization is forced off DEFAULT (-O1), which mis-compiles BDA pointers (see
                // GetShaderOptimizationLevel).
                slang::CompilerOptionEntry TargetOptions[2] = {};
                TargetOptions[0].name = slang::CompilerOptionName::DebugInformation;
                TargetOptions[0].value.kind = slang::CompilerOptionValueKind::Int;
                TargetOptions[0].value.intValue0 = GetShaderDebugInfoLevel();
                TargetOptions[1].name = slang::CompilerOptionName::Optimization;
                TargetOptions[1].value.kind = slang::CompilerOptionValueKind::Int;
                TargetOptions[1].value.intValue0 = GetShaderOptimizationLevel();
                TargetDesc.compilerOptionEntries = TargetOptions;
                TargetDesc.compilerOptionEntryCount = 2;

                // 39001: unbounded descriptor array (intentional, bindless)
                slang::CompilerOptionEntry SessionOptions[1] = {};
                SessionOptions[0].name = slang::CompilerOptionName::DisableWarnings;
                SessionOptions[0].value.kind = slang::CompilerOptionValueKind::String;
                SessionOptions[0].value.stringValue0 = "39001";
                SessionDesc.compilerOptionEntries = SessionOptions;
                SessionDesc.compilerOptionEntryCount = 1;

                SessionDesc.targets     = &TargetDesc;
                SessionDesc.targetCount = 1;

                // Engine first, then every enabled plugin's Shaders/ root. SlangSearchRoots keeps the
                // FString backing live across the Slang call; SearchPaths holds pointers into it.
                TVector<FString>     SlangSearchRoots;
                TVector<const char*> SearchPaths;
                SlangSearchRoots.reserve(8);
                SlangSearchRoots.emplace_back("/Engine/Resources/Shaders");
                for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
                {
                    if (!Plugin->IsEnabled())        continue;
                    if (!Plugin->IsContentMounted()) continue;
                    FString Root = Plugin->GetMountAlias();
                    Root += "/Shaders";
                    if (VFS::Exists(Root))
                    {
                        SlangSearchRoots.emplace_back(Move(Root));
                    }
                }
                SearchPaths.reserve(SlangSearchRoots.size());
                for (const FString& R : SlangSearchRoots) SearchPaths.push_back(R.c_str());
                SessionDesc.searchPaths     = SearchPaths.data();
                SessionDesc.searchPathCount = (SlangInt)SearchPaths.size();
                
                TVector<slang::PreprocessorMacroDesc> Macros;
                Macros.reserve(Options[i].MacroDefinitions.size());
                for (const FString& Macro : Options[i].MacroDefinitions)
                {
                    auto SeparatorPos = Macro.find('=');
                    if (SeparatorPos != FString::npos)
                    {
                        Macros.push_back({ Macro.substr(0, SeparatorPos).c_str(),
                                           Macro.substr(SeparatorPos + 1).c_str() });
                    }
                    else
                    {
                        Macros.push_back({ Macro.c_str(), "1" });
                    }
                }
                SessionDesc.preprocessorMacros     = Macros.data();
                SessionDesc.preprocessorMacroCount = (SlangInt)Macros.size();
        
                Slang::ComPtr<slang::ISession> Session;
                {
                    if (SLANG_FAILED(GlobalSession->createSession(SessionDesc, Session.writeRef())))
                    {
                        LOG_ERROR("Slang: Failed to create session");
                        return;
                    }
                }
        
                
                const FString& Path = Paths[i];
                FStringView FileName = VFS::FileName(Path);
                Slang::ComPtr<slang::IModule> SlangModule;
                Slang::ComPtr<slang::IBlob> Diagnostics;
                SlangModule = Session->loadModule(FileName.data(), Diagnostics.writeRef());
        
                if (Diagnostics)
                {
                    LOG_WARN("Slang diagnostics: {}", (const char*)Diagnostics->getBufferPointer());
                    Diagnostics = nullptr;
                }
        
                if (!SlangModule)
                {
                    LOG_ERROR("Slang: Failed to load shader module");
                    return;
                }
            
                TVector<Slang::ComPtr<slang::IEntryPoint>> EntryPoints;
                SlangInt32 EntryPointCount = SlangModule->getDefinedEntryPointCount();
                if (EntryPointCount == 0)
                {
                    LOG_ERROR("Slang: No entry points found in shader source");
                    return;
                }
        
                for (SlangInt32 EntryPointIdx = 0; EntryPointIdx < EntryPointCount; ++EntryPointIdx)
                {
                    Slang::ComPtr<slang::IEntryPoint> EP;
                    SlangModule->getDefinedEntryPoint(EntryPointIdx, EP.writeRef());
                    EntryPoints.push_back(Move(EP));
                }
        
                TVector<slang::IComponentType*> Components;
                Components.push_back(SlangModule);
                for (auto& EP : EntryPoints)
                {
                    Components.push_back(EP.get());
                }
        
                Slang::ComPtr<slang::IComponentType> LinkedProgram;
                if (SLANG_FAILED(Session->createCompositeComponentType(
                        Components.data(), (SlangInt)Components.size(),
                        LinkedProgram.writeRef(), Diagnostics.writeRef())))
                {
                    if (Diagnostics)
                    {
                        LOG_ERROR("Slang link error: {}", (const char*)Diagnostics->getBufferPointer());
                    }
                    LOG_ERROR("Slang: Failed to link shader program");
                    return;
                }
        
                TVector<uint32> Binaries;
                for (SlangInt EntryPointIdx = 0; EntryPointIdx < (SlangInt)EntryPoints.size(); ++EntryPointIdx)
                {
                    Slang::ComPtr<slang::IBlob> Code;
                    Diagnostics = nullptr;
        
                    if (SLANG_FAILED(LinkedProgram->getEntryPointCode(
                            EntryPointIdx, 0, Code.writeRef(), Diagnostics.writeRef())))
                    {
                        if (Diagnostics)
                        {
                            LOG_ERROR("Slang compile error: {}", (const char*)Diagnostics->getBufferPointer());
                        }
                    
                        LOG_ERROR("Slang: Failed to get SPIR-V for entry point {}", EntryPointIdx);
                        return;
                    }
        
                    if (Diagnostics)
                    {
                        LOG_WARN("Slang: {}", (const char*)Diagnostics->getBufferPointer());
                    }
        
                    const uint32* SpirvData = static_cast<const uint32*>(Code->getBufferPointer());
                    size_t SpirvSize        = Code->getBufferSize() / sizeof(uint32);
                    Binaries.insert(Binaries.end(), SpirvData, SpirvData + SpirvSize);
                }
        
                if (Binaries.empty())
                {
                    LOG_ERROR("Slang: Shader compiled to empty SPIR-V");
                    return;
                }
        
                FShaderHeader Shader;
                Shader.DebugName = FileName.data();
                Shader.Hash      = Hash::GetHash64(Binaries);
                Shader.Binaries  = Move(Binaries);
                Shader.Defines   = Options[i].MacroDefinitions;
            
                slang::ProgramLayout* ProgramLayout = LinkedProgram->getLayout();
                for (SlangUInt EntryPointIdx = 0; EntryPointIdx < EntryPointCount; ++EntryPointIdx)
                {
                    slang::EntryPointReflection* EPReflection = ProgramLayout->getEntryPointByIndex(EntryPointIdx);
                    switch (EPReflection->getStage())
                    {
                    case SLANG_STAGE_VERTEX:
                        Shader.Reflection.ShaderType = ERHIShaderType::Vertex;
                        break;
                    case SLANG_STAGE_GEOMETRY:
                        Shader.Reflection.ShaderType = ERHIShaderType::Geometry;
                        break;
                    case SLANG_STAGE_FRAGMENT:
                        Shader.Reflection.ShaderType = ERHIShaderType::Fragment;
                        break;
                    case SLANG_STAGE_COMPUTE:
                        Shader.Reflection.ShaderType = ERHIShaderType::Compute;
                        break;
                    case SLANG_STAGE_MESH:
                        Shader.Reflection.ShaderType = ERHIShaderType::Mesh;
                        break;
                    case SLANG_STAGE_DISPATCH:
                        Shader.Reflection.ShaderType = ERHIShaderType::Task;
                        break;
                    }
                }
            
                auto CompileEnd = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::milli> DurationMs = CompileEnd - CompileStart;
        
                LOG_TRACE("Compiled {0} in {1:.2f} ms (Thread {2})", FileName, DurationMs.count(), Thread);
        
                RHI::GetCrashTracker().RegisterShader(Shader.Binaries, Shader.DebugName);

                FShaderCache::Save(Paths[i], Options[i].MacroDefinitions, SourceHashes[i], Shader);

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
        // Pre-warm the global-session pool so the first compile wave doesn't serialize on core-module
        // loads. Sized to the batch target concurrency; the pool still grows on demand beyond this.
        GSlangSessionPool.Prewarm(std::max(1u, Threading::GetNumThreads() / 2));

        TVector<FString> Shaders;
        auto EnumerateShadersUnder = [&](FStringView Root)
        {
            VFS::DirectoryIterator(Root, [&](const VFS::FFileInfo& Info)
            {
                if (Info.GetExt() == ".slang")
                {
                    Shaders.emplace_back(Info.VirtualPath.c_str());
                }
            });
        };

        EnumerateShadersUnder("/Engine/Resources/Shaders");
        for (const FPlugin* Plugin : FPluginManager::Get().GetAllPlugins())
        {
            if (!Plugin->IsEnabled())        continue;
            if (!Plugin->IsContentMounted()) continue;
            FString Root = Plugin->GetMountAlias();
            Root += "/Shaders";
            if (VFS::Exists(Root))
            {
                EnumerateShadersUnder(FStringView(Root.c_str(), Root.size()));
            }
        }

        if (Shaders.empty())
        {
            // Packaged builds ship the .lsc cache instead of .slang sources;
            // load every entry under /Engine/Intermediate/ShaderCache directly.
            uint32 Loaded = 0;
            VFS::DirectoryIterator(FShaderCache::CACHE_DIR, [&](const VFS::FFileInfo& Info)
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
        else
        {
            TVector<FShaderCompileOptions> Options(Shaders.size());
            for (int i = 0; i < Shaders.size(); ++i)
            {
                Options[i].bGenerateReflectionData = false;
            }

            CompileShaderPaths(Shaders, Options, [&] (const FShaderHeader& Header)
            {
                FShaderLibrary::Commit(Header);
            });
        }
    }

    void FSpirVShaderCompiler::Shutdown()
    {
        Flush();
    }

    bool FSpirVShaderCompiler::CompilerShaderRaw(FString ShaderString, const FShaderCompileOptions& CompileOptions, CompletedFunc OnCompleted)
    {
        // Slang warns on top-level `#pragma once` (only valid inside an include).
        // Source assembled from .slang headers carries it through, strip it here.
        for (size_t Pos = ShaderString.find("#pragma once"); Pos != FString::npos; Pos = ShaderString.find("#pragma once", Pos))
        {
            size_t LineEnd = ShaderString.find('\n', Pos);
            if (LineEnd == FString::npos) LineEnd = ShaderString.size();
            else ++LineEnd;
            ShaderString.erase(Pos, LineEnd - Pos);
        }

        PendingTasks.fetch_add(1, std::memory_order_relaxed);
        
        Task::AsyncTask(1, 1, [this,
            ShaderString = Move(ShaderString),
            CompileOptions = Move(CompileOptions),
            Callback = Move(OnCompleted)]
            (uint32, uint32, uint32 Thread)
        {
            DEFER
            {
                PendingTasks.fetch_sub(1, std::memory_order_relaxed);
                std::atomic_notify_all(&PendingTasks);
            };

            Slang::ComPtr<slang::IGlobalSession> GlobalSession = GSlangSessionPool.Acquire();
            DEFER { GSlangSessionPool.Release(Move(GlobalSession)); };

            auto CompileStart = std::chrono::high_resolution_clock::now();
        
            slang::SessionDesc SessionDesc = {};
            SessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
            SessionDesc.fileSystem = &FileSystem;

            slang::TargetDesc TargetDesc = {};
            TargetDesc.format  = SLANG_SPIRV;
            TargetDesc.profile = GlobalSession->findProfile("spirv_1_5");
            TargetDesc.flags   = SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY | SLANG_TARGET_FLAG_GENERATE_WHOLE_PROGRAM;

            slang::CompilerOptionEntry TargetOptions[2] = {};
            TargetOptions[0].name = slang::CompilerOptionName::DebugInformation;
            TargetOptions[0].value.kind = slang::CompilerOptionValueKind::Int;
            TargetOptions[0].value.intValue0 = GetShaderDebugInfoLevel();
            TargetOptions[1].name = slang::CompilerOptionName::Optimization;
            TargetOptions[1].value.kind = slang::CompilerOptionValueKind::Int;
            TargetOptions[1].value.intValue0 = GetShaderOptimizationLevel();
            TargetDesc.compilerOptionEntries = TargetOptions;
            TargetDesc.compilerOptionEntryCount = 2;

            // 39001: unbounded descriptor array (intentional, bindless)
            slang::CompilerOptionEntry SessionOptions[1] = {};
            SessionOptions[0].name = slang::CompilerOptionName::DisableWarnings;
            SessionOptions[0].value.kind = slang::CompilerOptionValueKind::String;
            SessionOptions[0].value.stringValue0 = "39001";
            SessionDesc.compilerOptionEntries = SessionOptions;
            SessionDesc.compilerOptionEntryCount = 1;

            SessionDesc.targets     = &TargetDesc;
            SessionDesc.targetCount = 1;
        
            const char* SearchPaths[] = { "/Engine/Resources/Shaders" };
            SessionDesc.searchPaths     = SearchPaths;
            SessionDesc.searchPathCount = 1;
        
            TVector<slang::PreprocessorMacroDesc> Macros;
            Macros.reserve(CompileOptions.MacroDefinitions.size());
            for (const FString& Macro : CompileOptions.MacroDefinitions)
            {
                auto SeparatorPos = Macro.find('=');
                if (SeparatorPos != FString::npos)
                {
                    Macros.push_back({ Macro.substr(0, SeparatorPos).c_str(),
                                       Macro.substr(SeparatorPos + 1).c_str() });
                }
                else
                {
                    Macros.push_back({ Macro.c_str(), "1" });
                }
            }
            SessionDesc.preprocessorMacros     = Macros.data();
            SessionDesc.preprocessorMacroCount = (SlangInt)Macros.size();
        
            Slang::ComPtr<slang::ISession> Session;
            if (SLANG_FAILED(GlobalSession->createSession(SessionDesc, Session.writeRef())))
            {
                LOG_ERROR("Slang: Failed to create session");
                return;
            }
        
            Slang::ComPtr<slang::IBlob> Diagnostics;
        
            // Name the in-memory module after the caller's DebugName so crash dumps / Aftermath map source
            // lines to "<DebugName>.slang" instead of the generic "RawShader".
            const FString& RawName    = CompileOptions.DebugName.empty() ? FString("RawShader") : CompileOptions.DebugName;
            const FString  SourcePath = RawName + ".slang";

            Slang::ComPtr<slang::IModule> SlangModule;
            SlangModule = Session->loadModuleFromSourceString(RawName.c_str(), SourcePath.c_str(), ShaderString.c_str(), Diagnostics.writeRef());

            if (Diagnostics)
            {
                LOG_WARN("Slang diagnostics: {}", (const char*)Diagnostics->getBufferPointer());
                Diagnostics = nullptr;
            }

            if (!SlangModule)
            {
                LOG_ERROR("Slang: Failed to load shader module");
                return;
            }
            
            TVector<Slang::ComPtr<slang::IEntryPoint>> EntryPoints;
            SlangInt32 EntryPointCount = SlangModule->getDefinedEntryPointCount();
            if (EntryPointCount == 0)
            {
                LOG_ERROR("Slang: No entry points found in shader source");
                return;
            }
        
            for (SlangInt32 i = 0; i < EntryPointCount; ++i)
            {
                Slang::ComPtr<slang::IEntryPoint> EP;
                SlangModule->getDefinedEntryPoint(i, EP.writeRef());
                EntryPoints.push_back(Move(EP));
            }
        
            TVector<slang::IComponentType*> Components;
            Components.push_back(SlangModule);
            for (auto& EP : EntryPoints)
            {
                Components.push_back(EP.get());
            }
        
            Slang::ComPtr<slang::IComponentType> LinkedProgram;
            if (SLANG_FAILED(Session->createCompositeComponentType(
                    Components.data(), (SlangInt)Components.size(),
                    LinkedProgram.writeRef(), Diagnostics.writeRef())))
            {
                if (Diagnostics)
                {
                    LOG_ERROR("Slang link error: {}", (const char*)Diagnostics->getBufferPointer());
                }
                LOG_ERROR("Slang: Failed to link shader program");
                return;
            }
        
            TVector<uint32> Binaries;
            for (SlangInt i = 0; i < (SlangInt)EntryPoints.size(); ++i)
            {
                Slang::ComPtr<slang::IBlob> Code;
                Diagnostics = nullptr;
        
                if (SLANG_FAILED(LinkedProgram->getEntryPointCode(
                        i, 0, Code.writeRef(), Diagnostics.writeRef())))
                {
                    if (Diagnostics)
                    {
                        LOG_ERROR("Slang compile error: {}", (const char*)Diagnostics->getBufferPointer());
                    }
                    
                    LOG_ERROR("Slang: Failed to get SPIR-V for entry point {}", i);
                    return;
                }
        
                if (Diagnostics)
                {
                    LOG_WARN("Slang: {}", (const char*)Diagnostics->getBufferPointer());
                }
        
                const uint32* SpirvData = static_cast<const uint32*>(Code->getBufferPointer());
                size_t SpirvSize        = Code->getBufferSize() / sizeof(uint32);
                Binaries.insert(Binaries.end(), SpirvData, SpirvData + SpirvSize);
            }
        
            if (Binaries.empty())
            {
                LOG_ERROR("Slang: Shader compiled to empty SPIR-V");
                return;
            }
        
            FShaderHeader Shader;
            Shader.DebugName = RawName;
            Shader.Hash      = Hash::GetHash64(Binaries);
            Shader.Binaries  = Move(Binaries);
            Shader.Defines   = CompileOptions.MacroDefinitions;
            
            slang::ProgramLayout* ProgramLayout = LinkedProgram->getLayout();
            for (SlangInt32 i = 0; i < EntryPointCount; ++i)
            {
                slang::EntryPointReflection* EPReflection = ProgramLayout->getEntryPointByIndex(i);
                switch (EPReflection->getStage())
                {
                case SLANG_STAGE_VERTEX:
                    Shader.Reflection.ShaderType = ERHIShaderType::Vertex;
                    break;
                case SLANG_STAGE_GEOMETRY:
                    Shader.Reflection.ShaderType = ERHIShaderType::Geometry;
                    break;
                case SLANG_STAGE_FRAGMENT:
                    Shader.Reflection.ShaderType = ERHIShaderType::Fragment;
                    break;
                case SLANG_STAGE_COMPUTE:
                    Shader.Reflection.ShaderType = ERHIShaderType::Compute;
                    break;
                }
            }
            
            auto CompileEnd = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> DurationMs = CompileEnd - CompileStart;
        
            LOG_TRACE("Compiled raw shader in {0:.2f} ms (Thread {1})", DurationMs.count(), Thread);
        
            RHI::GetCrashTracker().RegisterShader(Shader.Binaries, Shader.DebugName);
            
            Callback(Move(Shader));
        });

        return true;
    }
}
