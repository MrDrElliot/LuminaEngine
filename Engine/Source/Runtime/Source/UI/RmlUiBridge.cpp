#include "Platform/Time/PlatformTime.h"
#include "RuntimePCH.h"
#include "Core/Engine/Engine.h"
#include "World/ECS/Registry.h"
#include "RmlUiBridge.h"
#include "Core/Delegates/ScriptDelegate.h"

#include "RmlUiFileInterface.h"
#include "RmlUiRenderer.h"
#include "WorldUIContext.h"
#include "UIScript.h"
#include "Core/Object/Object.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/PropertySnapshot.h"
#include "Core/Reflection/PropertyText.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/EnumProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Scripting/DotNet/DotNetUI.h"
#include "Core/Math/Hash/Hash.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Plugin.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/DataVariable.h>
#include <RmlUi/Core/Variant.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <cmath>
#include <limits>
#include <RmlUi/Debugger.h>

#include "FileSystem/FileSystem.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Delegates/CoreDelegates.h"
#include "Log/Log.h"
#include "Memory/Memory.h"
#include "Memory/MemoryTracking.h"
#include "Memory/SmartPtr.h"
#include "Core/Threading/Thread.h"
#include "Renderer/Format.h"
#include "Renderer/RHI.h"
#include "Renderer/RHITexture.h"
#include "Platform/Filesystem/PlatformFilesystem.h"
#include "World/World.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "World/Entity/Components/WidgetComponent.h"

#if USING(WITH_EDITOR)
#include "Tools/UI/ImGui/ImGuiX.h"
#endif

extern "C" RUNTIME_API void* LuminaRmlFreeTypeAlloc(size_t Size)
{
    LUMINA_MEMORY_SCOPE("RmlUi");
    return ::Lumina::Memory::Malloc(Size);
}

extern "C" RUNTIME_API void* LuminaRmlFreeTypeRealloc(void* Block, size_t NewSize)
{
    LUMINA_MEMORY_SCOPE("RmlUi");
    return ::Lumina::Memory::Realloc(Block, NewSize);
}

extern "C" RUNTIME_API void LuminaRmlFreeTypeFree(void* Block)
{
    ::Lumina::Memory::Free(Block);
}

namespace Lumina::RmlUi
{
    static TConsoleVar<int32> CVarWidgetMaxRendersPerFrame("UI.Widget.MaxRendersPerFrame", 8,
        "Max world-space widget RT rasterizations per frame; the rest reuse last frame's RT.");

    static TConsoleVar<int32> CVarWidgetDormancyFrames("UI.Widget.DormancyFrames", 4,
        "Frames of unchanged output before a world-space widget stops ticking; 0 = always tick.");

    // RmlUi has no implicit default font, so the bridge registers the engine font under this family.
    static constexpr const char* GDefaultUIFontFamily = "Lumina";

    namespace
    {
        // Thread-local because RmlUi logs from whichever thread drives it.
        thread_local TVector<FRmlDiagnostic>* GDiagnosticSink = nullptr;

        struct FScopedDiagnosticSink
        {
            explicit FScopedDiagnosticSink(TVector<FRmlDiagnostic>* Sink) : Previous(GDiagnosticSink) { GDiagnosticSink = Sink; }
            ~FScopedDiagnosticSink() { GDiagnosticSink = Previous; }

            TVector<FRmlDiagnostic>* Previous;
        };

#if USING(WITH_EDITOR)
        // Each distinct message is shown once and then muted, since RmlUi repeats them every frame.
        void NotifyDiagnostic(Rml::Log::Type Type, const Rml::String& Message)
        {
            constexpr double MuteSeconds = 10.0;
            constexpr size_t MaxTracked  = 64;

            static FMutex Mutex;
            static THashMap<FString, double> LastShown;

            const double Now = PlatformTime::Seconds();

            const FString Key(Message.c_str(), Message.size());
            {
                FScopeLock Lock(Mutex);

                // Bounded rather than evicted one by one, since messages carrying line numbers are unique.
                if (LastShown.size() >= MaxTracked)
                {
                    LastShown.clear();
                }

                auto It = LastShown.find(Key);
                if (It != LastShown.end() && (Now - It->second) < MuteSeconds)
                {
                    return;
                }
                LastShown[Key] = Now;
            }

            // NotifyInternal, not the format wrappers, because RCSS text is full of braces.
            const ImGuiX::Notifications::EType Severity = (Type == Rml::Log::LT_WARNING)
                ? ImGuiX::Notifications::EType::Warning
                : ImGuiX::Notifications::EType::Error;

            ImGuiX::Notifications::NotifyInternal(Severity, FStringView(Key.c_str(), Key.size()));
        }
#endif

        class FLuminaSystemInterface final : public Rml::SystemInterface
        {
        public:
            FLuminaSystemInterface() : StartTime(PlatformTime::Seconds()) {}

            double GetElapsedTime() override
            {
                return PlatformTime::Seconds() - StartTime;
            }

            bool LogMessage(Rml::Log::Type Type, const Rml::String& Message) override
            {
                // Only the two levels that mean the author has something to fix.
                const bool bDiagnostic =
                    (Type == Rml::Log::LT_ERROR || Type == Rml::Log::LT_ASSERT || Type == Rml::Log::LT_WARNING);

                if (bDiagnostic && GDiagnosticSink != nullptr)
                {
                    FRmlDiagnostic& Diagnostic = GDiagnosticSink->emplace_back();
                    Diagnostic.bError  = (Type != Rml::Log::LT_WARNING);
                    Diagnostic.Message = FString(Message.c_str(), Message.size());
                }
#if USING(WITH_EDITOR)
                // Only when nobody is collecting, since a capturing caller reports its own summary.
                else if (bDiagnostic)
                {
                    NotifyDiagnostic(Type, Message);
                }
#endif

                switch (Type)
                {
                case Rml::Log::LT_ERROR:
                case Rml::Log::LT_ASSERT:
                    LOG_ERROR("[RmlUi] {}", Message.c_str());
                    break;
                case Rml::Log::LT_WARNING:
                    LOG_WARN("[RmlUi] {}", Message.c_str());
                    break;
                case Rml::Log::LT_INFO:
                    LOG_INFO("[RmlUi] {}", Message.c_str());
                    break;
                case Rml::Log::LT_DEBUG:
                case Rml::Log::LT_ALWAYS:
                case Rml::Log::LT_MAX:
                default:
                    LOG_TRACE("[RmlUi] {}", Message.c_str());
                    break;
                }
                return true;
            }

            // Absolute virtual paths must pass through verbatim; base impl strips leading '/'.
            void JoinPath(Rml::String& OutPath, const Rml::String& DocumentPath, const Rml::String& Path) override
            {
                // Absolute virtual path -> use as-is.
                if (!Path.empty() && Path[0] == '/')
                {
                    OutPath = Path;
                    return;
                }

                // Scheme-prefixed source, so custom URI schemes survive path joining.
                const size_t Colon = Path.find(':');
                const size_t Slash = Path.find('/');
                if (Colon != Rml::String::npos && (Slash == Rml::String::npos || Colon < Slash))
                {
                    OutPath = Path;
                    return;
                }

                // Relative path resolved against the document's directory, preserving its leading slash.
                OutPath = DocumentPath;
                const size_t LastSlash = OutPath.rfind('/');
                if (LastSlash != Rml::String::npos)
                {
                    OutPath.resize(LastSlash + 1);
                }
                else
                {
                    OutPath.clear();
                }
                OutPath += Path;
            }

        private:
            double StartTime = 0.0;
        };

        // Editor preview context not bound to any world; tool owns the target image.
        struct FEditorEntry
        {
            Rml::Context*         Context = nullptr;
            RHI::FTextureH        Target = {};
            FUIntVector2            Size{0, 0};
            Rml::ElementDocument* Document = nullptr;
            float                 DpiScale = 1.0f;
            FVector4             ClearColor{0.10f, 0.10f, 0.12f, 1.0f};

            // Owned by State.DataModels like every other model, listed here so the preview can replace or reap them.
            TVector<void*>        DesignModels;
        };

        // Game code running inside an RmlUi event, where removing a data model would free the controller still on the stack.
        int32 GEventDispatchDepth = 0;

        struct FEventDispatchScope
        {
            FEventDispatchScope()  { ++GEventDispatchDepth; }
            ~FEventDispatchScope() { --GEventDispatchDepth; }
        };

        // RmlUi never deletes listeners on element destruction, so bAttached guards a freed element.
        class FManagedUIListener final : public Rml::EventListener
        {
        public:
            FManagedUIListener(CWorld* InWorld, Rml::String InType)
                : World(InWorld), Type(Move(InType)) {}

            void ProcessEvent(Rml::Event& Event) override
            {
                if (!Event_.IsBound())
                {
                    return;
                }
                FUIEventData Data{};
                Data.Id             = (int32)Event.GetId();
                Data.Phase          = (int32)Event.GetPhase();
                Data.CurrentElement = Event.GetCurrentElement();
                Data.TargetElement  = Event.GetTargetElement();
                Data.MouseX         = Event.GetParameter<float>("mouse_x", 0.0f);
                Data.MouseY         = Event.GetParameter<float>("mouse_y", 0.0f);
                Data.MouseButton    = Event.GetParameter<int>("button", -1);
                Data.KeyIdentifier  = Event.GetParameter<int>("key_identifier", 0);
                int32 Mods = 0;
                if (Event.GetParameter<bool>("ctrl_key",  false)) Mods |= 0x1;
                if (Event.GetParameter<bool>("shift_key", false)) Mods |= 0x2;
                if (Event.GetParameter<bool>("alt_key",   false)) Mods |= 0x4;
                if (Event.GetParameter<bool>("meta_key",  false)) Mods |= 0x8;
                Data.Modifiers = Mods;

                FEventDispatchScope Dispatch;
                Event_.Broadcast(Data);
            }

            void OnAttach(Rml::Element* InElement) override { Element = InElement; bAttached = true; }
            void OnDetach(Rml::Element*) override           { bAttached = false; }

            // Destroying this listener destroys the delegate, which is what releases every script binding.
            TScriptDelegate<FUIEventData> Event_;

            CWorld*              World     = nullptr;
            Rml::String          Type;
            Rml::Element*        Element   = nullptr;
            bool                 bAttached = false;
        };

        // Element and member identity packs into the handle, row in the high 32 bits, column in the low.
        struct FListField
        {
            Rml::String                             Name;
            TVector<Rml::String>                    MemberNames;   // column names ({{ item.Name }})
            TVector<TVector<Rml::Variant>>          Rows;          // Rows[row][col], string variants
            TVector<TVector<Rml::Variant>>          PreviousRows;  // the rows before the last resize
            Rml::UniquePtr<Rml::VariableDefinition> ArrayDef;
            Rml::UniquePtr<Rml::VariableDefinition> StructDef;
            Rml::UniquePtr<Rml::VariableDefinition> MemberDef;
        };

        // Leaf that reads one cell; the handle packs (row << 32 | col).
        class FListMemberDef final : public Rml::VariableDefinition
        {
        public:
            explicit FListMemberDef(FListField* InList) : Rml::VariableDefinition(Rml::DataVariableType::Scalar), List(InList) {}
            bool Get(void* Ptr, Rml::Variant& Out) override
            {
                const uintptr_t V = reinterpret_cast<uintptr_t>(Ptr);
                const int Row = (int)(V >> 32);
                const int Col = (int)(V & 0xFFFFFFFFu);
                if (Row >= 0 && Row < (int)List->Rows.size() && Col >= 0 && Col < (int)List->Rows[Row].size())
                {
                    Out = List->Rows[Row][Col];
                    return true;
                }

                // A row the list just shrank past stays bound for one update while its element is removed.
                if (Row >= (int)List->Rows.size() && Row < (int)List->PreviousRows.size() && Col >= 0 && Col < (int)List->PreviousRows[Row].size())
                {
                    Out = List->PreviousRows[Row][Col];
                    return true;
                }
                return false;
            }
        private:
            FListField* List;
        };

        // One row, resolving {{ item.<member> }} to a member-cell handle from the row index.
        class FListStructDef final : public Rml::VariableDefinition
        {
        public:
            explicit FListStructDef(FListField* InList) : Rml::VariableDefinition(Rml::DataVariableType::Struct), List(InList) {}
            Rml::DataVariable Child(void* Ptr, const Rml::DataAddressEntry& Address) override
            {
                const uintptr_t Row = reinterpret_cast<uintptr_t>(Ptr) & 0xFFFFFFFFu;
                for (size_t Col = 0; Col < List->MemberNames.size(); ++Col)
                {
                    if (List->MemberNames[Col] == Address.name)
                    {
                        const uintptr_t Encoded = (Row << 32) | (uintptr_t)Col;
                        return Rml::DataVariable(List->MemberDef.get(), reinterpret_cast<void*>(Encoded));
                    }
                }
                return Rml::DataVariable();
            }
        private:
            FListField* List;
        };

        // The array, mapping Size and index to a row handle; '.size' takes RmlUi's literal int path.
        class FListArrayDef final : public Rml::VariableDefinition
        {
        public:
            explicit FListArrayDef(FListField* InList) : Rml::VariableDefinition(Rml::DataVariableType::Array), List(InList) {}
            int Size(void* /*Ptr*/) override { return (int)List->Rows.size(); }
            Rml::DataVariable Child(void* /*Ptr*/, const Rml::DataAddressEntry& Address) override
            {
                const int Count = (int)List->Rows.size();
                const int Index = Address.index;
                if (Address.name == "size")
                {
                    return Rml::MakeLiteralIntVariable(Count);
                }

                if (Index < 0)
                {
                    return Rml::DataVariable();
                }
                // A list of plain values, such as names, reads each row's one cell as the item itself.
                if (List->MemberNames.empty())
                {
                    return Rml::DataVariable(List->MemberDef.get(), reinterpret_cast<void*>((uintptr_t)Index << 32));
                }
                return Rml::DataVariable(List->StructDef.get(), reinterpret_cast<void*>((uintptr_t)Index));
            }
        private:
            FListField* List;
        };

        // Variant::Set is private, so values are assigned through the public templated operator=.
        struct FManagedDataModel
        {
            CWorld*                   World = nullptr;
            Rml::String               Name;
            Rml::DataModelConstructor Constructor;        // held for the model's life so binding can continue
            Rml::DataModelHandle      Handle;
            TVector<Rml::Variant>     Values;             // per-field value cache (index == field id)
            TVector<int32>            Types;              // per-field EUIVarType
            TVector<Rml::String>      VarNames;           // per-field name, for DirtyVariable
            TVector<TUniquePtr<FListField>> Lists;        // list fields (own id space, index == list field id)
            void*                     Context = nullptr;  // managed GCHandle handed back to the thunks
            FManagedDataSetThunk      SetThunk = nullptr;
            FManagedDataEventThunk    EventThunk = nullptr;

            // Set for an editor preview's design model, which belongs to that context rather than to a world.
            Rml::Context*             OwnerContext = nullptr;

            // Commands an editor preview fired, as "Name(args)", since it has no script to call.
            TVector<FString>          FiredCommands;
        };

        // Coerce a double into a Variant typed per EUIVarType so views format/compare against the right type.
        void StoreNumber(Rml::Variant& V, int32 Type, double Value)
        {
            switch ((EUIVarType)Type)
            {
            case EUIVarType::Bool:   V = (Value != 0.0);    break;
            case EUIVarType::Int:    V = (int)Value;        break;
            case EUIVarType::Float:  V = (float)Value;      break;
            case EUIVarType::Double: V = Value;             break;
            default:                 V = Value;             break;
            }
        }

        struct FState
        {
            TUniquePtr<FLuminaSystemInterface>  System;
            TUniquePtr<FRmlUiFileInterface>     Files;
            TUniquePtr<FRmlUiRenderer>          Renderer;
            TVector<TUniquePtr<FEditorEntry>>   EditorContexts;

            // Editor hot-reload, raised by OnContentFileModified on any .rml or .rcss save.
            TAtomic<bool>                       bUIReloadPending{false};

            // Cycle count of the latest such save, since a watcher reports a write as it starts and the file is still partial then.
            TAtomic<uint64>                     LastUIFileChangeCycles{0};

            // .rml files saved since the last reload pass, handed to the scripts showing them. Written by the watcher thread.
            FMutex                              ChangedDocumentsMutex;
            TVector<FString>                    ChangedDocuments;

            CWorld*                             ActiveWorld = nullptr;

            // Live world UI contexts; membership is what separates a world a script cached from a freed one.
            THashSet<CWorld*>                   Worlds;

            Rml::Context*                       DebuggerHost = nullptr;
            bool                                bDebuggerVisible = false;
            bool                                bInitialized = false;

            uint32                              WidgetRenderCursor = 0;

            // Small N per screen, so a linear scan on add and remove is fine.
            TVector<FManagedUIListener*>        UIListeners;

            // Script-registered data models (World.UI.AddModel). Owned here; reaped per-world like UIListeners.
            TVector<FManagedDataModel*>         DataModels;

            // Recursive because Update may fire callbacks that re-enter the bridge on the same thread.
            FRecursiveMutex                     StateMutex;

            // Bumped by anything that can change what a context draws, so a context that saw the current value can sit idle.
            uint64                              ChangeGeneration = 1;

            // Elements whose children came from one inner RML string, so the same string set again can be skipped.
            THashMap<const Rml::Element*, uint64> InnerRmlHashes;

            // Checked once after their first layout, for mistakes RmlUi accepts silently.
            TVector<Rml::ElementDocument*>      DocumentsToLint;
        };

        FState& S()
        {
            static FState State;
            return State;
        }

        void NoteUIChanged()
        {
            ++S().ChangeGeneration;
        }

        // A change under an element whose children were set from markup means that markup no longer describes them.
        void ForgetInnerRml(const Rml::Element* Element)
        {
            FState& State = S();
            if (State.InnerRmlHashes.empty())
            {
                return;
            }
            for (const Rml::Element* It = Element; It != nullptr; It = It->GetParentNode())
            {
                State.InnerRmlHashes.erase(It);
            }
        }

        void NoteElementChanged(const Rml::Element* Element)
        {
            NoteUIChanged();
            ForgetInnerRml(Element);
        }

        // Null for a world the bridge has no context for, INCLUDING a freed one whose pointer a script cached.
        FWorldUIContext* WorldUI(const CWorld* World)
        {
            FState& State = S();
            if (World == nullptr || !State.Worlds.contains(const_cast<CWorld*>(World)))
            {
                return nullptr;
            }
            return World->GetUIContext();
        }

        FWorldUIContext* ActiveUI()
        {
            CWorld* W = S().ActiveWorld;
            return W ? W->GetUIContext() : nullptr;
        }

        Rml::Context* ActiveContext()
        {
            FWorldUIContext* UI = ActiveUI();
            return UI ? UI->Context : nullptr;
        }

        void SyncDebuggerToActiveContext()
        {
            FState& State = S();
            Rml::Context* Active = ActiveContext();
            
            Rml::Context* DesiredHost = State.bDebuggerVisible ? Active : nullptr;

            if (State.DebuggerHost != DesiredHost)
            {
                if (State.DebuggerHost != nullptr)
                {
                    Rml::Debugger::Shutdown();
                    State.DebuggerHost = nullptr;
                }
                if (DesiredHost != nullptr)
                {
                    if (Rml::Debugger::Initialise(DesiredHost))
                    {
                        State.DebuggerHost = DesiredHost;
                    }
                    else
                    {
                        LOG_WARN("[RmlUi] Debugger failed to attach to context '{}'.", DesiredHost->GetName());
                    }
                }
            }

            Rml::Debugger::SetVisible(State.bDebuggerVisible);
        }

        struct FWorldTarget { RHI::FTextureH Image = {}; FUIntVector2 Size{0, 0}; };
        FWorldTarget GetWorldTarget(const CWorld* World)
        {
            if (World == nullptr)
            {
                return {};
            }
            IRenderScene* Scene = World->GetRenderer();
            if (Scene == nullptr)
            {
                return {};
            }
            const RHI::FTextureH Img = Scene->GetDisplayTexture();
            if (!RHI::IsValid(Img))
            {
                return {};
            }
            const RHI::FTextureDesc Desc = RHI::GetTextureDesc(Img);
            return { Img, FUIntVector2(Desc.Dimension.x, Desc.Dimension.y) };
        }

        int64 GetDocumentWriteTime(const FString& VirtualPath)
        {
            if (VirtualPath.empty())
            {
                return 0;
            }
            const FPathString Physical = VFS::ResolvePath(VirtualPath);
            if (Physical.empty())
            {
                return 0;
            }
            return Filesystem::LastWriteTime(Physical);
        }

        // Every element in the context inherits this family unless it sets its own font-family.
        void ApplyDefaultFontFamily(Rml::Context* Ctx)
        {
            if (Ctx == nullptr)
            {
                return;
            }
            Rml::Element* Root = Ctx->GetRootElement();
            if (Root == nullptr)
            {
                return;
            }
            Root->SetProperty("font-family", GDefaultUIFontFamily);

            // Update once here so the root carries a computed family before any document is shown.
            Ctx->Update();
        }

        void DestroyWidgetRuntime(FWidgetRuntime& E)
        {
            if (E.Context != nullptr)
            {
                if (S().bInitialized)
                {
                    Rml::RemoveContext(E.Context->GetName());
                }
                E.Context  = nullptr;
                E.Document = nullptr;
            }
            // Drop the renderer's cached batch for this RT before the RT itself goes away.
            if (E.Target.IsValid() && S().Renderer != nullptr)
            {
                S().Renderer->ReleaseTargetBatch(E.Target.Texture);
            }
            if (E.Target.IsValid())
            {
                RHI::Textures::Release(E.Target);
            }
            E.ResourceID = -1;
            E.BuiltSize  = FUIntVector2(0, 0);
            E.LoadedPath.clear();
        }

        void EnsureWidgetResources(FWidgetRuntime& E, CWorld* World, ECS::FEntity Entity, uint32 Width, uint32 Height)
        {
            DestroyWidgetRuntime(E);

            char NameBuf[80];
            std::snprintf(NameBuf, sizeof(NameBuf), "widget_%p_%u",
                static_cast<void*>(World), (Entity).Value);

            E.Context = Rml::CreateContext(NameBuf, Rml::Vector2i(int(Width), int(Height)));
            if (E.Context == nullptr)
            {
                LOG_ERROR("[RmlUi] CreateContext failed for widget {}.", NameBuf);
                return;
            }
            ApplyDefaultFontFamily(E.Context);

            char TargetName[96];
            std::snprintf(TargetName, sizeof(TargetName), "RmlUi.WidgetRT.%s", NameBuf);

            E.Target = RHI::Textures::Create(RHI::FTexture2DDesc
            {
                .Width  = Width,
                .Height = Height,
                .Format = EFormat::RGBA8_UNORM,
                .bRenderTarget = true,
                .DebugName = TargetName,
            });
            E.ResourceID = E.Target.IsValid() ? (int32)E.Target.SampledSlot : -1;
            E.BuiltSize  = FUIntVector2(Width, Height);
        }

        // Long enough for a tool writing a large generated stylesheet to finish, so the restyle never caches half a file.
        constexpr double kUIReloadQuietSeconds = 0.5;

        void ProcessPendingUIReload()
        {
            FState& State = S();
            if (!State.bUIReloadPending.load(Atomic::MemoryOrderAcquire))
            {
                return;
            }
            const uint64 Since = PlatformTime::Cycles() - State.LastUIFileChangeCycles.load(Atomic::MemoryOrderAcquire);
            if (PlatformTime::ToSeconds(Since) < kUIReloadQuietSeconds)
            {
                return;
            }
            State.bUIReloadPending.store(false, Atomic::MemoryOrderRelease);
            NoteUIChanged();

            Rml::Factory::ClearStyleSheetCache();
            Rml::Factory::ClearTemplateCache();

            const int NumContexts = Rml::GetNumContexts();
            for (int i = 0; i < NumContexts; ++i)
            {
                Rml::Context* Ctx = Rml::GetContext(i);
                if (Ctx == nullptr)
                {
                    continue;
                }
                const int NumDocs = Ctx->GetNumDocuments();
                for (int d = 0; d < NumDocs; ++d)
                {
                    // A document parsed from memory has no file to read its styles back from.
                    Rml::ElementDocument* Doc = Ctx->GetDocument(d);
                    if (Doc != nullptr && !Doc->GetSourceURL().empty())
                    {
                        Doc->ReloadStyleSheet();
                    }
                }
            }
            LOG_INFO("[RmlUi] UI hot-reload: restyled all documents across {} context(s).", NumContexts);

            // Restyling cannot change markup, so a script showing a saved .rml loads it again.
            TVector<FString> Changed;
            {
                FScopeLock ChangedLock(State.ChangedDocumentsMutex);
                Changed.swap(State.ChangedDocuments);
            }
            for (const FString& Path : Changed)
            {
                UIScripts::DocumentChanged(FStringView(Path.c_str(), Path.size()));
            }
        }

        // Scans the live contexts without dereferencing Document, so a handle kept past its unload is refused instead of followed.
        bool IsLiveDocument(void* Document)
        {
            if (Document == nullptr)
            {
                return false;
            }
            const int NumContexts = Rml::GetNumContexts();
            for (int i = 0; i < NumContexts; ++i)
            {
                Rml::Context* Ctx = Rml::GetContext(i);
                if (Ctx == nullptr)
                {
                    continue;
                }
                const int NumDocs = Ctx->GetNumDocuments();
                for (int d = 0; d < NumDocs; ++d)
                {
                    if (Ctx->GetDocument(d) == Document)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        // Safe whether or not the elements still exist, since a destroyed one already fired OnDetach.
        void ReapWorldUIListeners(FState& State, CWorld* World)
        {
            for (size_t i = 0; i < State.UIListeners.size(); )
            {
                FManagedUIListener* L = State.UIListeners[i];
                if (L->World == World)
                {
                    if (L->bAttached && L->Element != nullptr)
                    {
                        L->Element->RemoveEventListener(L->Type, L, false);
                    }
                    State.UIListeners[i] = State.UIListeners.back();
                    State.UIListeners.pop_back();
                    delete L;
                }
                else
                {
                    ++i;
                }
            }
        }

        // RemoveContext already tore down the underlying DataModel, so only the wrapper is freed.
        void ReapWorldDataModels(FState& State, CWorld* World)
        {
            for (size_t i = 0; i < State.DataModels.size(); )
            {
                FManagedDataModel* M = State.DataModels[i];
                if (M->World == World)
                {
                    State.DataModels[i] = State.DataModels.back();
                    State.DataModels.pop_back();
                    delete M;
                }
                else
                {
                    ++i;
                }
            }
        }

        void OnContentFileModified(FStringView VirtualPath)
        {
            auto EndsWith = [&](FStringView Suffix)
            {
                return VirtualPath.size() >= Suffix.size()
                    && VirtualPath.substr(VirtualPath.size() - Suffix.size()) == Suffix;
            };
            if (EndsWith(FStringView(".rml")) || EndsWith(FStringView(".rcss")))
            {
                if (EndsWith(FStringView(".rml")))
                {
                    FState& State = S();
                    FScopeLock Lock(State.ChangedDocumentsMutex);
                    const FString Path(VirtualPath.data(), VirtualPath.size());
                    if (std::find(State.ChangedDocuments.begin(), State.ChangedDocuments.end(), Path) == State.ChangedDocuments.end())
                    {
                        State.ChangedDocuments.push_back(Path);
                    }
                }
                S().LastUIFileChangeCycles.store(PlatformTime::Cycles(), Atomic::MemoryOrderRelease);
                S().bUIReloadPending.store(true, Atomic::MemoryOrderRelease);
            }
        }

        // A stylesheet edited on disk outlives its cached parse unless the cache is dropped before the next document load.
        void DropStaleStyleSheets()
        {
            if (FRmlUiFileInterface::ConsumeChangedStyleSheets())
            {
                Rml::Factory::ClearStyleSheetCache();
                Rml::Factory::ClearTemplateCache();
                NoteUIChanged();
            }
        }
    }

    namespace
    {
        // RmlUi ships no user agent stylesheet, so without this every div in every document lays out inline.
        class FDefaultStyleSheetPlugin final : public Rml::Plugin
        {
        public:

            int GetEventClasses() override { return EVT_DOCUMENT; }

            void OnDocumentLoad(Rml::ElementDocument* Document) override
            {
                if (!bLoaded)
                {
                    bLoaded = true;
                    FString Source;
                    if (VFS::ReadFile(Source, "/Engine/Resources/UI/Default.rcss"))
                    {
                        DefaultRcss = Rml::String(Source.c_str());
                    }
                }

                if (Document == nullptr || DefaultRcss.empty())
                {
                    return;
                }

                // Parsed per document, because combining mutates the sheet it starts from.
                Rml::SharedPtr<Rml::StyleSheetContainer> Base = Rml::Factory::InstanceStyleSheetString(DefaultRcss);
                if (Base == nullptr)
                {
                    return;
                }

                const Rml::StyleSheetContainer* Own = Document->GetStyleSheetContainer();
                Document->SetStyleSheetContainer(Own != nullptr ? Base->CombineStyleSheetContainer(*Own) : Base);
            }

        private:

            Rml::String DefaultRcss;
            bool bLoaded = false;
        };

        class FElementLifetimePlugin final : public Rml::Plugin
        {
        public:

            int GetEventClasses() override { return EVT_ELEMENT | EVT_DOCUMENT; }

            void OnDocumentLoad(Rml::ElementDocument* Document) override
            {
                S().DocumentsToLint.push_back(Document);
            }

            void OnDocumentUnload(Rml::ElementDocument* Document) override
            {
                TVector<Rml::ElementDocument*>& Pending = S().DocumentsToLint;
                Pending.erase(std::remove(Pending.begin(), Pending.end(), Document), Pending.end());
            }

            void OnElementDestroy(Rml::Element* Element) override
            {
                FState& State = S();
                if (!State.InnerRmlHashes.empty())
                {
                    State.InnerRmlHashes.erase(Element);
                }
            }
        };

        // Text taller than its line spills out of its box and gets cut by whatever clips around it, which reads as a layout bug.
        constexpr float kLineHeightLintRatio = 0.75f;
        constexpr int32 kLintWarningsPerDocument = 8;

        void LintTextLineHeights(Rml::Element* Element, const Rml::String& Url, int32& Warnings)
        {
            if (Warnings >= kLintWarningsPerDocument)
            {
                return;
            }
            if (const Rml::ElementText* Text = rmlui_dynamic_cast<const Rml::ElementText*>(Element))
            {
                const Rml::FontFaceHandle Face = Element->GetFontFaceHandle();
                if (Face == 0 || Text->GetText().empty())
                {
                    return;
                }
                const Rml::FontMetrics& Metrics = Rml::GetFontEngineInterface()->GetFontMetrics(Face);
                const float FontHeight = Metrics.ascent + Metrics.descent;
                const float LineHeight = Element->GetLineHeight();
                if (FontHeight > 0.0f && LineHeight < FontHeight * kLineHeightLintRatio)
                {
                    Rml::Element* Owner = Element->GetParentNode() != nullptr ? Element->GetParentNode() : Element;
                    LOG_WARN("[RmlUi] {}: '{}' has a {:.0f}px line-height but its font is {:.0f}px tall, so the text spills out of its box. "
                             "Raise line-height or lower font-size.", Url.c_str(), Owner->GetAddress().c_str(), LineHeight, FontHeight);
                    ++Warnings;
                }
                return;
            }
            for (int i = 0; i < Element->GetNumChildren(); ++i)
            {
                LintTextLineHeights(Element->GetChild(i), Url, Warnings);
            }
        }

        // Runs after an update, since line heights and font faces exist only once styles are computed.
        void LintLoadedDocuments(Rml::Context* Context)
        {
            TVector<Rml::ElementDocument*>& Pending = S().DocumentsToLint;
            for (size_t i = 0; i < Pending.size();)
            {
                Rml::ElementDocument* Document = Pending[i];
                if (Document->GetContext() != Context)
                {
                    ++i;
                    continue;
                }
                int32 Warnings = 0;
                LintTextLineHeights(Document, Document->GetSourceURL(), Warnings);
                Pending.erase(Pending.begin() + i);
            }
        }

        void RegisterDefaultStyleSheet()
        {
            static FDefaultStyleSheetPlugin Plugin;
            static FElementLifetimePlugin LifetimePlugin;
            static bool bRegistered = false;
            if (!bRegistered)
            {
                bRegistered = true;
                Rml::RegisterPlugin(&Plugin);
                Rml::RegisterPlugin(&LifetimePlugin);
            }
        }

        void CreateWorldContext(CWorld* World, FWorldUIContext& UI)
        {
            FState& State = S();

            // TickWorldUI resizes from the real RT each frame; initial size is a placeholder.
            const FWorldTarget Tgt = GetWorldTarget(World);
            const Rml::Vector2i InitialSize = (Tgt.Size.x > 0 && Tgt.Size.y > 0)
                ? Rml::Vector2i(int(Tgt.Size.x), int(Tgt.Size.y))
                : Rml::Vector2i(1280, 720);

            char NameBuf[64];
            std::snprintf(NameBuf, sizeof(NameBuf), "world_%p", static_cast<void*>(World));

            Rml::Context* Ctx = Rml::CreateContext(NameBuf, InitialSize);
            if (Ctx == nullptr)
            {
                LOG_ERROR("[RmlUi] CreateContext failed for world {}.", static_cast<void*>(World));
                return;
            }

            UI.Context = Ctx;
            ApplyDefaultFontFamily(Ctx);

            // Newest world becomes the active UI target.
            State.ActiveWorld = World;
            SyncDebuggerToActiveContext();

            LOG_INFO("[RmlUi] Created context '{}' for world {} (initial {}x{}).", NameBuf, static_cast<void*>(World), InitialSize.x, InitialSize.y);
        }
    }

    bool Initialize()
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized) return true;

        State.System   = MakeUnique<FLuminaSystemInterface>();
        State.Files    = MakeUnique<FRmlUiFileInterface>();
        State.Renderer = MakeUnique<FRmlUiRenderer>();

        Rml::SetSystemInterface(State.System.get());
        Rml::SetFileInterface(State.Files.get());
        Rml::SetRenderInterface(State.Renderer.get());

        auto ResetOnFailure = [&]()
        {
            State.System.reset();
            State.Files.reset();
            State.Renderer.reset();
            State.EditorContexts.clear();
            State.ActiveWorld = nullptr;
            State.DebuggerHost = nullptr;
            State.bDebuggerVisible = false;
            State.bInitialized = false;
        };

        if (!Rml::Initialise())
        {
            LOG_ERROR("[RmlUi] Rml::Initialize failed.");
            Rml::SetRenderInterface(nullptr);
            Rml::SetFileInterface(nullptr);
            Rml::SetSystemInterface(nullptr);
            ResetOnFailure();
            return false;
        }

        RegisterDefaultStyleSheet();

        if (!State.Renderer->Initialize())
        {
            LOG_ERROR("[RmlUi] FRmlUiRenderer initialization failed; tearing down.");
            Rml::Shutdown();
            Rml::SetRenderInterface(nullptr);
            Rml::SetFileInterface(nullptr);
            Rml::SetSystemInterface(nullptr);
            ResetOnFailure();
            return false;
        }

        // Registered under our own family name rather than whatever the file's metadata calls it.
        constexpr const char* DefaultFontCandidates[] =
        {
            "/Engine/Resources/UI/Fonts/LatoLatin-Regular.ttf",
            "/Engine/Resources/Fonts/Lexend/Lexend-Regular.ttf",
            "/Engine/Resources/Fonts/JetbrainsMono/JetBrainsMono-Regular.ttf",
        };

        bool bDefaultFontRegistered = false;
        for (const char* Candidate : DefaultFontCandidates)
        {
            if (Rml::LoadFontFace(Candidate, GDefaultUIFontFamily, Rml::Style::FontStyle::Normal,
                    Rml::Style::FontWeight::Auto, true /*fallback_face*/))
            {
                bDefaultFontRegistered = true;
                break;
            }

            LOG_WARN("[RmlUi] UI font '{}' failed to register as '{}'; trying the next candidate.",
                Candidate, GDefaultUIFontFamily);
        }

        if (bDefaultFontRegistered)
        {
            // Probe once at startup, lowercased, since the provider keys on lowercase and asserts in Debug.
            const Rml::String Probe = Rml::StringUtilities::ToLower(Rml::String(GDefaultUIFontFamily));

            if (Rml::GetFontEngineInterface()->GetFontFaceHandle(Probe, Rml::Style::FontStyle::Normal,
                    Rml::Style::FontWeight::Normal, 16) == 0)
            {
                LOG_ERROR("[RmlUi] A face registered under '{}' but the family does not resolve for lookup. "
                          "Every document inheriting the engine default will render no text.",
                    GDefaultUIFontFamily);
            }
            else
            {
                LOG_INFO("[RmlUi] Default UI font family '{}' registered and resolving.", GDefaultUIFontFamily);
            }
        }
        else
        {
            // Nothing else in the UI stack fails visibly here, so say it once and plainly.
            LOG_ERROR("[RmlUi] No UI font could be registered as '{}'. Every document that does not author its "
                      "own 'font-family' will render no text. Check that the engine content is mounted.",
                GDefaultUIFontFamily);
        }

        // Optional monospace face for digit-heavy HUDs; falls back to LatoLatin when missing.
        Rml::LoadFontFace("/Engine/Resources/Fonts/JetbrainsMono/JetBrainsMono-ExtraBold.ttf", false);
        Rml::LoadFontFace("/Engine/Resources/Fonts/JetbrainsMono/JetBrainsMono-Bold.ttf", false);

        State.bInitialized = true;

        // A packaged game loads its startup map inside LoadProject, before this runs.
        for (CWorld* World : State.Worlds)
        {
            if (FWorldUIContext* UI = World->GetUIContext(); UI != nullptr && UI->Context == nullptr)
            {
                CreateWorldContext(World, *UI);
            }
        }

        (void)FCoreDelegates::OnContentFileModified.AddStatic(&OnContentFileModified);

        LOG_INFO("[RmlUi] Initialized.");
        return true;
    }

    bool LoadFontFace(FStringView Path, FStringView Family, bool bBold, bool bItalic)
    {
        return Rml::LoadFontFace(Rml::String(Path.data(), Path.size()), Rml::String(Family.data(), Family.size()),
            bItalic ? Rml::Style::FontStyle::Italic : Rml::Style::FontStyle::Normal,
            bBold ? Rml::Style::FontWeight::Bold : Rml::Style::FontWeight::Normal);
    }

    void Shutdown()
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized && !State.System)
        {
            return;
        }

        if (State.DebuggerHost != nullptr)
        {
            Rml::Debugger::Shutdown();
            State.DebuggerHost = nullptr;
        }
        
        for (auto& E : State.EditorContexts)
        {
            if (E->Context != nullptr)
            {
                Rml::RemoveContext(E->Context->GetName());
                E->Context = nullptr;
            }
        }

        // Widget contexts are torn down with their worlds; Shutdown drops any that outlive theirs.
        Rml::Shutdown();

        // All contexts are gone; free any script UI listeners that outlived their world.
        for (FManagedUIListener* L : State.UIListeners)
        {
            delete L;
        }
        State.UIListeners.clear();

        // Same for data models (their contexts were destroyed by Rml::Shutdown above).
        for (FManagedDataModel* M : State.DataModels)
        {
            delete M;
        }
        State.DataModels.clear();

        State.EditorContexts.clear();
        State.Worlds.clear();
        State.ActiveWorld = nullptr;
        State.bInitialized = false;

        if (State.Renderer) State.Renderer->Shutdown();

        Rml::SetRenderInterface(nullptr);
        Rml::SetFileInterface(nullptr);
        Rml::SetSystemInterface(nullptr);

        // Clear fields individually because StateMutex is non-movable.
        State.System.reset();
        State.Files.reset();
        State.Renderer.reset();
        State.bDebuggerVisible = false;
    }

    TUniquePtr<FWorldUIContext> CreateWorldUI(CWorld* World)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);

        TUniquePtr<FWorldUIContext> UI = MakeUnique<FWorldUIContext>();
        if (World == nullptr)
        {
            return UI;
        }
        State.Worlds.insert(World);

        // A world loaded before Initialize gets its context from the backfill there.
        if (State.bInitialized)
        {
            CreateWorldContext(World, *UI);
        }
        return UI;
    }

    namespace
    {
        // Defined with the object models further down.
        void ReapContextObjectModels(Rml::Context* Context);
    }

    void DestroyWorldUI(CWorld* World)
    {
        if (World == nullptr)
        {
            return;
        }
        FWorldUIContext* UI = World->GetUIContext();
        if (UI == nullptr)
        {
            return;
        }

        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);

        State.Worlds.erase(World);

        // Rml::Shutdown already destroyed every context, so drop the dangling pointer untouched.
        if (!State.bInitialized)
        {
            ReapWorldUIListeners(State, World);
            ReapWorldDataModels(State, World);
            ReapContextObjectModels(UI->Context);
            UIScripts::ForgetWorld(World);
            UI->Context = nullptr;
            UI->Documents.clear();
            if (State.ActiveWorld == World)
            {
                State.ActiveWorld = nullptr;
            }
            return;
        }

        if (UI->Context != nullptr)
        {
            // Detach debugger before removing host; otherwise it'd walk a freed element tree.
            if (State.DebuggerHost == UI->Context)
            {
                Rml::Debugger::Shutdown();
                State.DebuggerHost = nullptr;
                State.bDebuggerVisible = false;
            }
            // The scripts showing documents here let go of them before the context and its models go.
            UIScripts::ForgetWorld(World);
            Rml::Context* Dying = UI->Context;

            // RmlUi tears down first (calls listener OnDetach), then we drop wrappers.
            Rml::RemoveContext(UI->Context->GetName());
            UI->Context = nullptr;
            ReapContextObjectModels(Dying);
        }
        // Elements are gone now (RemoveContext fired OnDetach); free the script listener objects.
        ReapWorldUIListeners(State, World);
        // The context (and its data models) is destroyed; free the data-model wrappers for this world.
        ReapWorldDataModels(State, World);
        UI->Documents.clear();

        if (State.ActiveWorld == World)
        {
            State.ActiveWorld = nullptr;
            SyncDebuggerToActiveContext();
        }
    }

    void SetActiveWorld(CWorld* World)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || World == nullptr || WorldUI(World) == nullptr)
        {
            return;
        }
        State.ActiveWorld = World;
        SyncDebuggerToActiveContext();
    }

    void TickWorldUI(CWorld* World)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || World == nullptr)
        {
            return;
        }

        // Once per frame, restyling all docs when a UI file changed on disk; the flag self-clears.
        ProcessPendingUIReload();

        // RmlUi reloads released textures on demand, which picks up the new slot and size of a reimported asset.
        if (State.Renderer && State.Renderer->HasStaleAssetTextures())
        {
            Rml::ReleaseTextures();
            NoteUIChanged();
        }

        FWorldUIContext* UI = WorldUI(World);
        if (UI == nullptr || UI->Context == nullptr)
        {
            return;
        }

        UIScripts::Tick(World);
        DotNetUI::PollModels(World);
        PollObjectModels(UI->Context);

        const FWorldTarget Tgt = GetWorldTarget(World);
        FUIntVector2 LayoutSize = UI->LastLayoutSize;
        float DpRatio = UI->LastDpRatio;
        if (RHI::IsValid(Tgt.Image))
        {
            // The editor's DisplaySize override lays the UI out at the panel's aspect, not the RT's.
            LayoutSize = (UI->DisplaySize.x > 0 && UI->DisplaySize.y > 0) ? UI->DisplaySize : Tgt.Size;

            constexpr float NominalHeight = 1080.0f;
            DpRatio = Math::Max(1.0f, float(LayoutSize.y) / NominalHeight);
        }

        // RmlUi reports how long it can wait before an animation, transition or caret blink needs it, and every edit and input bumps the generation.
        const double Now = PlatformTime::Seconds();
        const bool bChanged = UI->SeenChangeGeneration != State.ChangeGeneration || LayoutSize != UI->LastLayoutSize
            || DpRatio != UI->LastDpRatio || Now >= UI->NextUpdateSeconds;
        UI->bIdleThisFrame = !bChanged;
        if (!bChanged)
        {
            return;
        }

        UI->Context->SetDimensions(Rml::Vector2i(int(LayoutSize.x), int(LayoutSize.y)));
        UI->Context->SetDensityIndependentPixelRatio(DpRatio);
        UI->LastLayoutSize = LayoutSize;
        UI->LastDpRatio = DpRatio;

        // Taken before the update, so an edit made by a callback inside it brings the next frame back for another pass.
        UI->SeenChangeGeneration = State.ChangeGeneration;
        {
            LUMINA_PROFILE_SECTION("RmlUi Context Update");
            UI->Context->Update();
        }
        if (!State.DocumentsToLint.empty())
        {
            LintLoadedDocuments(UI->Context);
        }
        const double Delay = UI->Context->GetNextUpdateDelay();
        UI->NextUpdateSeconds = Delay >= std::numeric_limits<double>::max() ? std::numeric_limits<double>::infinity() : Now + Delay;
    }

    void RenderWorldUI(const CWorld* World, RHI::FCmdListH CmdList)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || State.Renderer == nullptr || World == nullptr)
        {
            return;
        }
        FWorldUIContext* UI = WorldUI(World);
        if (UI == nullptr || UI->Context == nullptr)
        {
            return;
        }

        const FWorldTarget Tgt = GetWorldTarget(World);
        if (!RHI::IsValid(Tgt.Image))
        {
            return;
        }

        const FUIntVector2 LayoutSize = (UI->DisplaySize.x > 0 && UI->DisplaySize.y > 0) ? UI->DisplaySize : Tgt.Size;
        if (UI->bIdleThisFrame && State.Renderer->ReplayCachedFrame(CmdList, Tgt.Image, Tgt.Size, LayoutSize))
        {
            return;
        }
        LUMINA_PROFILE_SECTION("RmlUi Context Render");
        State.Renderer->BeginFrame(CmdList, Tgt.Image, Tgt.Size, LayoutSize);
        UI->Context->Render();
        State.Renderer->EndFrame();
    }

    void TickWorldWidgets(CWorld* World)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || World == nullptr)
        {
            return;
        }
        FWorldUIContext* UI = WorldUI(World);
        if (UI == nullptr)
        {
            return;
        }

        UI->WidgetJobs.clear();

        World->View<SWidgetComponent>().ForEach([&](ECS::FEntity Entity, SWidgetComponent& Comp)
        {
            FWidgetRuntime& R = Comp.Runtime;

            if (!R.bVisible && R.Context != nullptr)
            {
                return;
            }

            const uint32 Width  = (uint32)Math::Max(1, Comp.DrawWidth);
            const uint32 Height = (uint32)Math::Max(1, Comp.DrawHeight);

            if (R.Context == nullptr || R.BuiltSize != FUIntVector2(Width, Height))
            {
                EnsureWidgetResources(R, World, Entity, Width, Height);
            }
            if (R.Context == nullptr)
            {
                return;
            }

            // Resolve the rename-safe ref to its current virtual path (GUID-first).
            const FStringView DocView = Comp.DocumentPath.ResolvePath();
            const FString     DocPath(DocView.data(), DocView.size());

            const int64 CurrentWriteTime = GetDocumentWriteTime(DocPath);
            const bool  bPathChanged     = (R.LoadedPath != DocPath);
            const bool  bFileChanged     = (R.Document != nullptr && CurrentWriteTime != 0 && CurrentWriteTime != R.DocWriteTime);

            if (bPathChanged || bFileChanged)
            {
                // Drop cached stylesheets so a re-parse picks up .rcss changes, not just the .rml body.
                if (bFileChanged)
                {
                    Rml::Factory::ClearStyleSheetCache();
                    Rml::Factory::ClearTemplateCache();
                }

                R.Context->UnloadAllDocuments();
                R.Document   = nullptr;
                R.LoadedPath = DocPath;
                if (!DocPath.empty())
                {
                    R.Document = R.Context->LoadDocument(Rml::String(DocPath.c_str()));
                    if (R.Document != nullptr)
                    {
                        R.Document->SetProperty("width", "100%");
                        R.Document->SetProperty("height", "100%");
                        R.Document->Show();
                    }
                    else
                    {
                        LOG_WARN("[RmlUi] Widget failed to load document '{}'.", DocPath.c_str());
                    }
                }
                R.DocWriteTime = CurrentWriteTime;
            }
            
            const int32 DormancyFrames = CVarWidgetDormancyFrames.GetValue();
            if (DormancyFrames > 0 && R.bRmlIdle && !bPathChanged && !bFileChanged && R.Target.IsValid() && State.Renderer != nullptr)
            {
                if (State.Renderer->GetTargetStableFrames(R.Target.Texture) >= (uint32)DormancyFrames)
                {
                    return;   // settled, so no Update and no job; keep last RT
                }
            }

            R.Context->SetDimensions(Rml::Vector2i(int(Width), int(Height)));
            R.Context->SetDensityIndependentPixelRatio(Math::Max(0.1f, float(Height) / 1080.0f));
            R.Context->Update();

            // RmlUi lowers this from infinity when an animation is pending, so huge means idle.
            R.bRmlIdle = (R.Context->GetNextUpdateDelay() > 1.0e6);

            // Queue for the render phase (R.ResourceID is read by the scene gather directly).
            if (R.Document != nullptr && R.Target.IsValid())
            {
                UI->WidgetJobs.push_back(FWidgetRenderJob{ R.Context, R.Target.Texture, R.BuiltSize });
            }
        });
    }

    void RenderWorldWidgets(const CWorld* World, RHI::FCmdListH CmdList)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || State.Renderer == nullptr || World == nullptr)
        {
            return;
        }
        const FWorldUIContext* UI = WorldUI(World);
        if (UI == nullptr)
        {
            return;
        }

        const size_t JobCount = UI->WidgetJobs.size();
        if (JobCount == 0)
        {
            return;
        }

        const int32 Budget = Math::Max(0, CVarWidgetMaxRendersPerFrame.GetValue());
        int32 Rendered = 0;

        // Rotate the start each frame so the budget doesn't always favor the first widgets.
        for (size_t k = 0; k < JobCount; ++k)
        {
            const FWidgetRenderJob& Job = UI->WidgetJobs[(State.WidgetRenderCursor + k) % JobCount];
            if (Job.Context == nullptr || !RHI::IsValid(Job.Target))
            {
                continue;
            }

            // Transparent clear rides the UI pass load op instead of a barrier pair per widget.
            const FVector4 Transparent(0.0f, 0.0f, 0.0f, 0.0f);
            State.Renderer->BeginFrame(CmdList, Job.Target, Job.Size, FUIntVector2(0), &Transparent);
            Job.Context->Render();
            const uint64 Hash = State.Renderer->PeekFrameHash();

            if (State.Renderer->IsTargetUpToDate(Job.Target, Hash))
            {
                State.Renderer->AbortFrame();
                State.Renderer->NoteTargetStable(Job.Target, true);   // unchanged -> closer to dormant
                continue;
            }

            if (Budget > 0 && Rendered >= Budget)
            {
                State.Renderer->AbortFrame();
                State.Renderer->NoteTargetStable(Job.Target, false);  // pending change -> keep awake
                continue;
            }

            if (Rendered == 0)
            {
                // Order this frame's widget RT writes after the previous frame's sampling of them.
                RHI::CmdBarrier(CmdList,
                    RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderWrite,
                    RHI::EStageFlags::RasterColorOut,
                    RHI::EAccessFlags::ColorRead | RHI::EAccessFlags::ColorWrite);
            }

            State.Renderer->EndFrame();
            State.Renderer->NoteTargetStable(Job.Target, false);      // just changed -> reset
            ++Rendered;
        }

        if (Rendered > 0)
        {
            // Widget RT writes visible to the scene's widget pass sampling them later this frame.
            RHI::CmdBarrier(CmdList,
                RHI::EStageFlags::RasterColorOut, RHI::EAccessFlags::ColorWrite,
                RHI::EStageFlags::PixelShader,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }

        State.WidgetRenderCursor = (uint32)((State.WidgetRenderCursor + 1) % JobCount);
    }

    void ReleaseWidget(CWorld* World, SWidgetComponent& Component)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        
        if (World != nullptr)
        {
            if (FWorldUIContext* UI = WorldUI(World))
            {
                Rml::Context* DyingContext = Component.Runtime.Context;
                for (auto It = UI->WidgetJobs.begin(); It != UI->WidgetJobs.end(); )
                {
                    It = (It->Context == DyingContext) ? UI->WidgetJobs.erase(It) : It + 1;
                }
            }
        }

        DestroyWidgetRuntime(Component.Runtime);
    }

    void TickEditorContexts()
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized)
        {
            return;
        }

        for (auto& E : State.EditorContexts)
        {
            if (E->Context == nullptr)
            {
                continue;
            }
            if (E->Size.x > 0 && E->Size.y > 0)
            {
                E->Context->SetDimensions(Rml::Vector2i(int(E->Size.x), int(E->Size.y)));
                // Editor contexts use caller-supplied DPI; the world heuristic is too small for previews <1080px.
                E->Context->SetDensityIndependentPixelRatio(Math::Max(0.1f, E->DpiScale));
            }
            PollObjectModels(E->Context);
            E->Context->Update();
        }
    }

    void RenderEditorContexts(RHI::FCmdListH CmdList)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || State.Renderer == nullptr)
        {
            return;
        }

        bool bAnyRendered = false;
        for (auto& E : State.EditorContexts)
        {
            if (E->Context == nullptr || !RHI::IsValid(E->Target))
            {
                continue;
            }
            if (E->Size.x == 0 || E->Size.y == 0)
            {
                continue;
            }
            // Renderer uses LoadOp=Load; clear here so editor can composite its own background under a transparent canvas.
            const float Clear[4] = { E->ClearColor.x, E->ClearColor.y, E->ClearColor.z, E->ClearColor.w };
            RHI::CmdBarrier(CmdList,
                RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorWrite,
                RHI::EStageFlags::Transfer,
                RHI::EAccessFlags::TransferRead | RHI::EAccessFlags::TransferWrite);
            RHI::CmdClearTexture(CmdList, E->Target, Clear);
            RHI::CmdBarrier(CmdList,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::PixelShader,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite | RHI::EAccessFlags::ColorRead | RHI::EAccessFlags::ColorWrite);

            // A resized preview target starts undefined, so loading it showed a frame of garbage.
            const FVector4 PreviewClear(0.0f, 0.0f, 0.0f, 0.0f);
            State.Renderer->BeginFrame(CmdList, E->Target, E->Size, FUIntVector2(0), &PreviewClear);
            E->Context->Render();
            State.Renderer->EndFrame();
            bAnyRendered = true;
        }

        if (bAnyRendered)
        {
            // Preview RT writes visible to ImGui sampling them this frame.
            RHI::CmdBarrier(CmdList,
                RHI::EStageFlags::RasterColorOut, RHI::EAccessFlags::ColorWrite,
                RHI::EStageFlags::PixelShader,
                RHI::EAccessFlags::ShaderRead | RHI::EAccessFlags::ShaderWrite);
        }
    }

    // Renderer pointer is set once in Initialize() and cleared only in Shutdown(), so reads go unlocked.
    FRmlUiRenderer* GetRenderer()      { return S().Renderer.get(); }

    Rml::Context* GetContextForWorld(CWorld* World)
    {
        if (World == nullptr)
        {
            return nullptr;
        }
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        FWorldUIContext* UI = WorldUI(World);
        NoteUIChanged();
        return UI ? UI->Context : nullptr;
    }

    bool WorldUIWantsMouse(const CWorld* World)
    {
        if (World == nullptr)
        {
            return false;
        }
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized)
        {
            return false;
        }
        const FWorldUIContext* UI = WorldUI(World);
        return UI != nullptr && UI->Context != nullptr && UI->Context->IsMouseInteracting();
    }

    FLockedWorldContext::FLockedWorldContext(CWorld* World, bool bMayModify)
    {
        if (World == nullptr)
        {
            return;
        }
        FState& State = S();
        State.StateMutex.lock();
        bLocked = true;
        // Resolved inside the locked scope so the Context* cannot be torn down before use.
        FWorldUIContext* UI = WorldUI(World);
        Context = UI ? UI->Context : nullptr;
        // Whatever the holder does to the context, it did not go through an edit the bridge could see.
        if (bMayModify)
        {
            NoteUIChanged();
        }
    }

    FLockedWorldContext::~FLockedWorldContext()
    {
        if (bLocked)
        {
            S().StateMutex.unlock();
        }
    }

    FUIntVector2 GetWorldLayoutSize(CWorld* World)
    {
        FLockedWorldContext Context(World, false);
        if (!Context)
        {
            return FUIntVector2(0u, 0u);
        }

        const Rml::Vector2i Size = Context->GetDimensions();
        return FUIntVector2((uint32)Math::Max(Size.x, 0), (uint32)Math::Max(Size.y, 0));
    }

    bool IsCursorOverWorldUI(CWorld* World)
    {
        FLockedWorldContext Context(World, false);
        if (!Context)
        {
            return false;
        }

        Rml::Element* Hovered = Context->GetHoverElement();

        // The context reports the document itself when the cursor is over nothing the page drew.
        while (Hovered != nullptr)
        {
            if (Hovered->GetOwnerDocument() == Hovered)
            {
                return false;
            }
            if (Hovered->GetComputedValues().pointer_events() != Rml::Style::PointerEvents::None)
            {
                return true;
            }
            Hovered = Hovered->GetParentNode();
        }
        return false;
    }

    void SetWorldDisplaySize(CWorld* World, const FUIntVector2& Size)
    {
        if (World == nullptr)
        {
            return;
        }
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (FWorldUIContext* UI = WorldUI(World))
        {
            UI->DisplaySize = Size;
        }
    }

    bool SetWorldInlineDocument(CWorld* World, FStringView Body, FStringView SourceUrl)
    {
        NoteUIChanged();
        if (World == nullptr)
        {
            return false;
        }
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized)
        {
            return false;
        }
        FWorldUIContext* UI = WorldUI(World);
        if (UI == nullptr || UI->Context == nullptr)
        {
            return false;
        }

        // Drop whatever was loaded; the preview owns the whole context.
        for (auto& KV : UI->Documents)
        {
            UI->Context->UnloadDocument(KV.second);
        }
        UI->Documents.clear();

        if (Body.empty())
        {
            return false;
        }

        const Rml::String BodyStr(Body.data(), Body.size());
        const Rml::String UrlStr(SourceUrl.data(), SourceUrl.size());
        Rml::ElementDocument* Doc = UI->Context->LoadDocumentFromMemory(BodyStr, UrlStr);
        if (Doc == nullptr)
        {
            LOG_ERROR("[RmlUi] SetWorldInlineDocument: failed to parse inline body.");
            return false;
        }
        Doc->Show();
        UI->Documents.emplace(FString("__inline__"), Doc);
        return true;
    }

    Rml::Context* CreateEditorContext(const char* Name, const FUIntVector2& InitialSize)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || Name == nullptr)
        {
            return nullptr;
        }

        const Rml::Vector2i Size(
            int(InitialSize.x > 0 ? InitialSize.x : 1u),
            int(InitialSize.y > 0 ? InitialSize.y : 1u));

        Rml::Context* Ctx = Rml::CreateContext(Name, Size);
        if (Ctx == nullptr)
        {
            LOG_ERROR("[RmlUi] CreateEditorContext failed for name '{}'.", Name);
            return nullptr;
        }

        ApplyDefaultFontFamily(Ctx);

        TUniquePtr<FEditorEntry> Entry = MakeUnique<FEditorEntry>();
        Entry->Context = Ctx;
        Entry->Size    = InitialSize;
        State.EditorContexts.push_back(Move(Entry));
        return Ctx;
    }

    void DestroyEditorContext(Rml::Context* Context)
    {
        if (Context == nullptr)
        {
            return;
        }
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        for (size_t i = 0; i < State.EditorContexts.size(); ++i)
        {
            FEditorEntry* E = State.EditorContexts[i].get();
            if (E->Context != Context)
            {
                continue;
            }

            if (State.DebuggerHost == E->Context)
            {
                Rml::Debugger::Shutdown();
                State.DebuggerHost = nullptr;
                State.bDebuggerVisible = false;
            }

            for (void* Model : E->DesignModels)
            {
                DestroyDataModel(Model);
            }
            E->DesignModels.clear();

            Rml::Context* Dying = E->Context;
            Rml::RemoveContext(E->Context->GetName());
            ReapContextObjectModels(Dying);
            E->Context = nullptr;

            const size_t Last = State.EditorContexts.size() - 1;
            if (i != Last)
            {
                std::swap(State.EditorContexts[i], State.EditorContexts[Last]);
            }
            State.EditorContexts.pop_back();
            return;
        }
    }

    void SetEditorContextTarget(Rml::Context* Context, RHI::FTextureH Target, const FUIntVector2& Size)
    {
        if (Context == nullptr)
        {
            return;
        }
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        for (auto& E : State.EditorContexts)
        {
            if (E->Context == Context)
            {
                E->Target = Target;
                E->Size   = Size;
                return;
            }
        }
    }

    void SetEditorContextDpiScale(Rml::Context* Context, float Scale)
    {
        if (Context == nullptr)
        {
            return;
        }
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        for (auto& E : State.EditorContexts)
        {
            if (E->Context == Context)
            {
                E->DpiScale = Scale;
                return;
            }
        }
    }

    void ForwardEditorContextMouse(Rml::Context* Context, const FVector2& Position, float WheelDelta,
                                   bool bLeftDown, bool bRightDown, bool bLeftWasDown, bool bRightWasDown)
    {
        if (Context == nullptr)
        {
            return;
        }

        FRecursiveScopeLock Lock(S().StateMutex);

        Context->ProcessMouseMove(int(Position.x), int(Position.y), 0);

        if (bLeftDown != bLeftWasDown)
        {
            bLeftDown ? Context->ProcessMouseButtonDown(0, 0) : Context->ProcessMouseButtonUp(0, 0);
        }
        if (bRightDown != bRightWasDown)
        {
            bRightDown ? Context->ProcessMouseButtonDown(1, 0) : Context->ProcessMouseButtonUp(1, 0);
        }

        if (WheelDelta != 0.0f)
        {
            // RmlUi counts wheel down as positive, the opposite of ImGui.
            Context->ProcessMouseWheel(-WheelDelta, 0);
        }
    }

    void ForwardEditorContextMouseLeave(Rml::Context* Context)
    {
        if (Context == nullptr)
        {
            return;
        }

        FRecursiveScopeLock Lock(S().StateMutex);
        Context->ProcessMouseLeave();
    }

    void SetEditorContextClearColor(Rml::Context* Context, const FVector4& Color)
    {
        if (Context == nullptr)
        {
            return;
        }
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        for (auto& E : State.EditorContexts)
        {
            if (E->Context == Context)
            {
                E->ClearColor = Color;
                return;
            }
        }
    }

    namespace
    {
        FEditorEntry* FindEditorEntry(Rml::Context* Context)
        {
            FState& State = S();
            for (auto& E : State.EditorContexts)
            {
                if (E->Context == Context)
                {
                    return E.get();
                }
            }
            return nullptr;
        }
    }

    bool ReplaceEditorContextDocument(Rml::Context* Context, FStringView Body, FStringView SourceUrl,
        TVector<FRmlDiagnostic>* OutDiagnostics)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        FEditorEntry* Entry = FindEditorEntry(Context);
        if (Entry == nullptr || Entry->Context == nullptr)
        {
            return false;
        }

        if (Entry->Document != nullptr)
        {
            Entry->Context->UnloadDocument(Entry->Document);
            Entry->Document = nullptr;
        }

        if (Body.empty())
        {
            return false;
        }

        const Rml::String BodyStr(Body.data(), Body.size());
        const Rml::String UrlStr(SourceUrl.data(), SourceUrl.size());
        
        Rml::Factory::ClearStyleSheetCache();
        Rml::Factory::ClearTemplateCache();

        // Scoped over Show() too, since property and decorator errors surface at first styling.
        {
            FScopedDiagnosticSink Capture(OutDiagnostics);

            Entry->Document = Entry->Context->LoadDocumentFromMemory(BodyStr, UrlStr);
            if (Entry->Document == nullptr)
            {
                return false;
            }
            Entry->Document->Show();

            Entry->Context->Update();
        }

        return true;
    }

    void ClearEditorContextDocument(Rml::Context* Context)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        FEditorEntry* Entry = FindEditorEntry(Context);
        if (Entry == nullptr || Entry->Context == nullptr)
        {
            return;
        }
        if (Entry->Document != nullptr)
        {
            Entry->Context->UnloadDocument(Entry->Document);
            Entry->Document = nullptr;
        }
    }

    void EnumerateEditorSlots(Rml::Context* Context, TVector<FRmlEditorSlot>& OutSlots)
    {
        OutSlots.clear();

        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || Context == nullptr)
        {
            return;
        }
        Rml::Element* Root = Context->GetRootElement();
        if (Root == nullptr)
        {
            return;
        }

        // Iterative pre-order walk; an element is a slot only if it carries an id (the assignment anchor).
        struct FFrame { Rml::Element* Element; int32 Depth; };
        TVector<FFrame> Stack;
        Stack.push_back({Root, 0});

        while (!Stack.empty())
        {
            const FFrame Frame = Stack.back();
            Stack.pop_back();

            Rml::Element* E = Frame.Element;
            const Rml::String& Id = E->GetId();
            if (!Id.empty())
            {
                FRmlEditorSlot Slot;
                Slot.Id.assign(Id.c_str(), Id.size());
                const Rml::String& Tag = E->GetTagName();
                Slot.Tag.assign(Tag.c_str(), Tag.size());

                const Rml::Vector2f Off  = E->GetAbsoluteOffset(Rml::BoxArea::Border);
                const Rml::Vector2f Size = E->GetBox().GetSize(Rml::BoxArea::Border);
                Slot.OffsetPx   = FVector2(Off.x, Off.y);
                Slot.SizePx     = FVector2(Size.x, Size.y);
                Slot.Depth      = Frame.Depth;
                Slot.ChildCount = E->GetNumChildren();
                OutSlots.push_back(Move(Slot));
            }

            // Push children in reverse so the pop order stays document order.
            const int NumChildren = E->GetNumChildren();
            for (int i = NumChildren - 1; i >= 0; --i)
            {
                if (Rml::Element* Child = E->GetChild(i))
                {
                    Stack.push_back({Child, Frame.Depth + 1});
                }
            }
        }
    }

    // Scripting surface for World.UI; takes the bridge lock, game thread only.

    namespace
    {
        Rml::Element*         AsElement (void* P) { return static_cast<Rml::Element*>(P); }
        Rml::ElementDocument* AsDocument(void* P) { return static_cast<Rml::ElementDocument*>(P); }
        Rml::String           ToRml(FStringView V) { return Rml::String(V.data(), V.size()); }
    }

    void* LoadScreenDocument(CWorld* World, FStringView VirtualPath)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (World == nullptr || VirtualPath.empty())
        {
            return nullptr;
        }
        if (!State.bInitialized)
        {
            // A headless process never starts the UI, so a document it asks for is expected to come back empty.
            if (!GIsHeadless)
            {
                LOG_WARN("[RmlUi] World.UI.LoadDocument('{}') called before RmlUi initialized.", FString(VirtualPath.data(), VirtualPath.size()).c_str());
            }
            return nullptr;
        }
        FWorldUIContext* UI = WorldUI(World);
        if (UI == nullptr || UI->Context == nullptr)
        {
            LOG_WARN("[RmlUi] World.UI.LoadDocument('{}') found no UI context on world '{}'.",
                FString(VirtualPath.data(), VirtualPath.size()).c_str(), World->GetName().c_str());
            return nullptr;
        }
        DropStaleStyleSheets();
        Rml::ElementDocument* Doc = UI->Context->LoadDocument(ToRml(VirtualPath));
        if (Doc == nullptr)
        {
            LOG_WARN("[RmlUi] World.UI.LoadDocument failed for '{}'.", FString(VirtualPath.data(), VirtualPath.size()).c_str());
        }
        return Doc;
    }

    void* LoadScreenDocumentFromMemory(CWorld* World, FStringView Body, FStringView SourceUrl)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || World == nullptr || Body.empty())
        {
            return nullptr;
        }
        FWorldUIContext* UI = WorldUI(World);
        if (UI == nullptr || UI->Context == nullptr)
        {
            return nullptr;
        }
        DropStaleStyleSheets();
        return UI->Context->LoadDocumentFromMemory(ToRml(Body), ToRml(SourceUrl));
    }

    void* LoadScreenDocumentWithModel(CWorld* World, FStringView VirtualPath, FStringView FromModel, FStringView ToModel)
    {
        if (FromModel == ToModel)
        {
            return LoadScreenDocument(World, VirtualPath);
        }
        Rml::FileInterface* Files = Rml::GetFileInterface();
        Rml::String Body;
        if (Files == nullptr || !Files->LoadFile(ToRml(VirtualPath), Body))
        {
            return nullptr;
        }

        const Rml::String From(FromModel.data(), FromModel.size());
        const Rml::String To(ToModel.data(), ToModel.size());
        for (const char Quote : { '"', '\'' })
        {
            const Rml::String Old = Rml::String("data-model=") + Quote + From + Quote;
            const Rml::String New = Rml::String("data-model=") + Quote + To + Quote;
            for (size_t At = Body.find(Old); At != Rml::String::npos; At = Body.find(Old, At + New.size()))
            {
                Body.replace(At, Old.size(), New);
            }
        }
        return LoadScreenDocumentFromMemory(World, FStringView(Body.data(), Body.size()), VirtualPath);
    }

    bool HasDataModel(Rml::Context* Context, FStringView Name)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        // GetDataModel logs a miss as an error, and a miss is the answer this asks for.
        return State.bInitialized && Context != nullptr && Context->GetDataModels().count(Rml::String(Name.data(), Name.size())) > 0;
    }

    void UnloadScreenDocument(CWorld* World, void* Document)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || World == nullptr || Document == nullptr)
        {
            return;
        }
        FWorldUIContext* UI = WorldUI(World);
        if (UI != nullptr && UI->Context != nullptr && IsLiveDocument(Document))
        {
            UI->Context->UnloadDocument(AsDocument(Document));
        }
    }

    void ShowDocument(void* Document, bool bModal, bool bAutoFocus)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && IsLiveDocument(Document))
        {
            AsDocument(Document)->Show(bModal     ? Rml::ModalFlag::Modal : Rml::ModalFlag::None,
                                       bAutoFocus ? Rml::FocusFlag::Auto  : Rml::FocusFlag::None);
        }
    }

    void HideDocument(void* Document)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && IsLiveDocument(Document))
        {
            AsDocument(Document)->Hide();
        }
    }

    void PullDocumentToFront(void* Document)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && IsLiveDocument(Document))
        {
            AsDocument(Document)->PullToFront();
        }
    }

    void* GetDocumentRoot(void* Document)
    {
        // The ElementDocument IS the root element of its tree; hand it back as an Element handle.
        return Document;
    }

    void* DocumentGetElementById(void* Document, FStringView Id)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || Id.empty() || !IsLiveDocument(Document))
        {
            return nullptr;
        }
        return AsDocument(Document)->GetElementById(ToRml(Id));
    }

    void* ElementQuerySelector(void* Element, FStringView Selector)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || Element == nullptr || Selector.empty())
        {
            return nullptr;
        }
        return AsElement(Element)->QuerySelector(ToRml(Selector));
    }

    namespace
    {
        // Pointer-valued properties such as transforms and decorators parse to a new object every time, so those compare by text.
        bool PropertyAlreadySet(Rml::Element* Element, FStringView Name, FStringView Value)
        {
            Rml::PropertyDictionary Parsed;
            if (!Rml::StyleSheetSpecification::ParsePropertyDeclaration(Parsed, ToRml(Name), ToRml(Value)) || Parsed.GetNumProperties() == 0)
            {
                return false;
            }
            for (const auto& [Id, Property] : Parsed.GetProperties())
            {
                const Rml::Property* Current = Element->GetLocalProperty(Id);
                if (Current == nullptr)
                {
                    return false;
                }
                if (!(*Current == Property) && Current->ToString() != Property.ToString())
                {
                    return false;
                }
            }
            return true;
        }
    }

    void ElementSetInnerRml(void* Element, FStringView Markup)
    {
        LUMINA_PROFILE_SCOPE();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || Element == nullptr)
        {
            return;
        }
        Rml::Element* Target = AsElement(Element);
        const uint64 Hash = Hash::GetHash64(Markup.data(), Markup.size()) ^ Markup.size();
        auto Known = State.InnerRmlHashes.find(Target);
        if (Known != State.InnerRmlHashes.end() && Known->second == Hash)
        {
            return;
        }

        Target->SetInnerRML(ToRml(Markup));
        NoteElementChanged(Target);

        // Form controls and data bindings change their own children, so their markup stops describing them.
        auto Has = [Markup](FStringView Needle) { return Markup.find(Needle) != FStringView::npos; };
        if (!Has("<input") && !Has("<textarea") && !Has("<select") && !Has("data-"))
        {
            State.InnerRmlHashes[Target] = Hash;
        }
    }

    FString ElementGetInnerRml(void* Element)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || Element == nullptr)
        {
            return FString();
        }
        const Rml::String Value = AsElement(Element)->GetInnerRML();
        return FString(Value.c_str(), Value.size());
    }

    void ElementSetAttribute(void* Element, FStringView Name, FStringView Value)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Element != nullptr && !Name.empty())
        {
            Rml::Element* Target = AsElement(Element);
            const Rml::String Key = ToRml(Name);
            const Rml::Variant* Current = Target->GetAttribute(Key);
            const Rml::String NewValue = ToRml(Value);
            if (Current != nullptr && Current->GetType() == Rml::Variant::STRING && Current->Get<Rml::String>() == NewValue)
            {
                return;
            }
            Target->SetAttribute(Key, NewValue);
            NoteElementChanged(Target);
        }
    }

    FString ElementGetAttribute(void* Element, FStringView Name)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || Element == nullptr || Name.empty())
        {
            return FString();
        }
        const Rml::String Value = AsElement(Element)->GetAttribute<Rml::String>(ToRml(Name), Rml::String());
        return FString(Value.c_str(), Value.size());
    }

    void ElementSetProperty(void* Element, FStringView Name, FStringView Value)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Element != nullptr && !Name.empty())
        {
            Rml::Element* Target = AsElement(Element);
            if (PropertyAlreadySet(Target, Name, Value))
            {
                return;
            }
            Target->SetProperty(ToRml(Name), ToRml(Value));
            NoteElementChanged(Target);
        }
    }

    void ElementRemoveProperty(void* Element, FStringView Name)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Element != nullptr && !Name.empty())
        {
            AsElement(Element)->RemoveProperty(ToRml(Name));
            NoteElementChanged(AsElement(Element));
        }
    }

    void ElementSetClass(void* Element, FStringView Class, bool bActive)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Element != nullptr && !Class.empty())
        {
            Rml::Element* Target = AsElement(Element);
            const Rml::String Name = ToRml(Class);
            if (Target->IsClassSet(Name) == bActive)
            {
                return;
            }
            Target->SetClass(Name, bActive);
            NoteElementChanged(Target);
        }
    }

    bool ElementIsClassSet(void* Element, FStringView Class)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || Element == nullptr || Class.empty())
        {
            return false;
        }
        return AsElement(Element)->IsClassSet(ToRml(Class));
    }

    void ElementFocus(void* Element)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Element != nullptr)
        {
            AsElement(Element)->Focus();
            NoteElementChanged(AsElement(Element));
        }
    }

    void ElementBlur(void* Element)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Element != nullptr)
        {
            AsElement(Element)->Blur();
            NoteElementChanged(AsElement(Element));
        }
    }

    void ElementClick(void* Element)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Element != nullptr)
        {
            {
                FEventDispatchScope Dispatch;
                AsElement(Element)->Click();
            }
            NoteElementChanged(AsElement(Element));
        }
    }

    void ElementGetBox(void* Element, float* OutXYWH)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        OutXYWH[0] = OutXYWH[1] = OutXYWH[2] = OutXYWH[3] = 0.0f;
        if (!State.bInitialized || Element == nullptr)
        {
            return;
        }
        const Rml::Vector2f Offset = AsElement(Element)->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f Size = AsElement(Element)->GetBox().GetSize(Rml::BoxArea::Border);
        OutXYWH[0] = Offset.x;
        OutXYWH[1] = Offset.y;
        OutXYWH[2] = Size.x;
        OutXYWH[3] = Size.y;
    }

    void* AddElementEventListener(CWorld* World, void* Element, FStringView EventType)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || Element == nullptr || EventType.empty())
        {
            return nullptr;
        }
        FManagedUIListener* Listener = new FManagedUIListener(World, ToRml(EventType));
        AsElement(Element)->AddEventListener(Listener->Type, Listener, false);
        State.UIListeners.push_back(Listener);
        return Listener;
    }

    void* GetElementEventListenerDelegate(void* Listener)
    {
        return Listener != nullptr ? static_cast<FScriptDelegateBase*>(&static_cast<FManagedUIListener*>(Listener)->Event_) : nullptr;
    }

    void RemoveElementEventListener(CWorld* World, void* Listener)
    {
        (void)World;   // listeners are found by identity; world is kept for API symmetry with the connect call.
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (Listener == nullptr)
        {
            return;
        }
        FManagedUIListener* L = static_cast<FManagedUIListener*>(Listener);
        for (size_t i = 0; i < State.UIListeners.size(); ++i)
        {
            if (State.UIListeners[i] != L)
            {
                continue;
            }
            if (L->bAttached && L->Element != nullptr)
            {
                L->Element->RemoveEventListener(L->Type, L, false);
            }
            State.UIListeners[i] = State.UIListeners.back();
            State.UIListeners.pop_back();
            delete L;
            return;
        }
    }

    // Data binding for LuminaSharp ViewModel and World.UI.AddModel.

    void* CreateDataModel(CWorld* World, FStringView Name, void* Context, FManagedDataSetThunk SetThunk, FManagedDataEventThunk EventThunk)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || World == nullptr || Name.empty())
        {
            return nullptr;
        }
        FWorldUIContext* UI = WorldUI(World);
        if (UI == nullptr || UI->Context == nullptr)
        {
            return nullptr;
        }

        const Rml::String ModelName(Name.data(), Name.size());
        Rml::DataModelConstructor Ctor = UI->Context->CreateDataModel(ModelName);
        if (!Ctor)
        {
            LOG_WARN("[RmlUi] CreateDataModel('{}') failed (a model with that name already exists?).", ModelName.c_str());
            return nullptr;
        }

        FManagedDataModel* M = new FManagedDataModel();
        M->World       = World;
        M->Name        = ModelName;
        M->Constructor = Ctor;
        M->Handle      = Ctor.GetModelHandle();
        M->Context     = Context;
        M->SetThunk    = SetThunk;
        M->EventThunk  = EventThunk;
        State.DataModels.push_back(M);
        return M;
    }

    int32 DataModelBindScalar(void* ModelPtr, FStringView Name, int32 Type)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || ModelPtr == nullptr || Name.empty())
        {
            return -1;
        }
        FManagedDataModel* M = static_cast<FManagedDataModel*>(ModelPtr);

        // Only commit to the cache vectors on success, so a failed bind never burns an id.
        const int32       Field = (int32)M->Values.size();
        const Rml::String VarName(Name.data(), Name.size());
        const bool        bString = ((EUIVarType)Type == EUIVarType::String);

        const bool bOk = M->Constructor.BindFunc(VarName,
            // Getter hands RmlUi the cached value, with no managed crossing on read.
            [M, Field](Rml::Variant& Out) { Out = M->Values[Field]; },
            // Coerce before caching, since a form control hands the setter back a string.
            [M, Field, bString](const Rml::Variant& In)
            {
                if (bString)
                {
                    const Rml::String Str = In.Get<Rml::String>();
                    M->Values[Field] = Str;
                    if (M->SetThunk != nullptr)
                    {
                        M->SetThunk(M->Context, Field, (int32)EUIVarType::String, 0.0, Str.c_str(), (int32)Str.size());
                    }
                }
                else
                {
                    const double Num = In.Get<double>(0.0);
                    StoreNumber(M->Values[Field], M->Types[Field], Num);
                    if (M->SetThunk != nullptr)
                    {
                        M->SetThunk(M->Context, Field, M->Types[Field], Num, nullptr, 0);
                    }
                }
            });

        if (!bOk)
        {
            LOG_WARN("[RmlUi] DataModelBindScalar('{}') failed on model '{}'.", VarName.c_str(), M->Name.c_str());
            return -1;
        }

        // Commit (BindFunc only registers the funcs, it never invokes the getter, so seeding now is safe).
        M->Values.emplace_back();
        M->Types.push_back(Type);
        M->VarNames.push_back(VarName);
        if (bString)
        {
            M->Values[Field] = Rml::String();
        }
        else
        {
            StoreNumber(M->Values[Field], Type, 0.0);
        }
        return Field;
    }

    void DataModelBindCommand(void* ModelPtr, FStringView Name, int32 CommandId)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || ModelPtr == nullptr || Name.empty())
        {
            return;
        }
        FManagedDataModel* M = static_cast<FManagedDataModel*>(ModelPtr);
        const Rml::String CmdName(Name.data(), Name.size());
        M->Constructor.BindEventCallback(CmdName,
            [M, CommandId, CmdName](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& Arguments)
            {
                if (M->EventThunk == nullptr)
                {
                    if (M->OwnerContext != nullptr && M->FiredCommands.size() < 64)
                    {
                        Rml::String Call = CmdName + "(";
                        for (size_t Index = 0; Index < Arguments.size(); ++Index)
                        {
                            Call += (Index > 0 ? ", " : "") + Arguments[Index].Get<Rml::String>();
                        }
                        Call += ")";
                        M->FiredCommands.push_back(FString(Call.c_str(), Call.size()));
                    }
                    return;
                }
                // Stringify each RML argument uniformly; keep the storage alive across the dispatch call.
                TVector<Rml::String> Strings;
                Strings.reserve(Arguments.size());
                for (const Rml::Variant& Arg : Arguments)
                {
                    Strings.push_back(Arg.Get<Rml::String>());
                }
                TVector<FUIArg> Argv;
                Argv.reserve(Strings.size());
                for (const Rml::String& Str : Strings)
                {
                    Argv.push_back(FUIArg{ Str.c_str(), (int32)Str.size() });
                }
                M->EventThunk(M->Context, CommandId, (int32)Argv.size(), Argv.empty() ? nullptr : Argv.data());
            });
    }

    int32 DataModelBindList(void* ModelPtr, FStringView Name)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || ModelPtr == nullptr || Name.empty())
        {
            return -1;
        }
        FManagedDataModel* M = static_cast<FManagedDataModel*>(ModelPtr);

        TUniquePtr<FListField> List = MakeUnique<FListField>();
        List->Name = Rml::String(Name.data(), Name.size());
        FListField* LP = List.get();
        List->MemberDef = Rml::MakeUnique<FListMemberDef>(LP);
        List->StructDef = Rml::MakeUnique<FListStructDef>(LP);
        List->ArrayDef  = Rml::MakeUnique<FListArrayDef>(LP);

        if (!M->Constructor.BindCustomDataVariable(List->Name, Rml::DataVariable(List->ArrayDef.get(), nullptr)))
        {
            LOG_WARN("[RmlUi] DataModelBindList('{}') failed on model '{}'.", List->Name.c_str(), M->Name.c_str());
            return -1;
        }

        const int32 Field = (int32)M->Lists.size();
        M->Lists.push_back(Move(List));
        return Field;
    }

    int32 DataModelBindListMember(void* ModelPtr, int32 ListField, FStringView MemberName)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (ModelPtr == nullptr || MemberName.empty())
        {
            return -1;
        }
        FManagedDataModel* M = static_cast<FManagedDataModel*>(ModelPtr);
        if (ListField < 0 || ListField >= (int32)M->Lists.size())
        {
            return -1;
        }
        FListField* L = M->Lists[ListField].get();
        const int32 Col = (int32)L->MemberNames.size();
        L->MemberNames.emplace_back(MemberName.data(), MemberName.size());
        return Col;
    }

    void DataModelListResize(void* ModelPtr, int32 ListField, int32 RowCount)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (ModelPtr == nullptr || RowCount < 0)
        {
            return;
        }
        FManagedDataModel* M = static_cast<FManagedDataModel*>(ModelPtr);
        if (ListField < 0 || ListField >= (int32)M->Lists.size())
        {
            return;
        }
        FListField* L = M->Lists[ListField].get();
        const size_t Cols = L->MemberNames.size();
        L->PreviousRows = Move(L->Rows);
        L->Rows.assign((size_t)RowCount, TVector<Rml::Variant>());
        for (TVector<Rml::Variant>& Row : L->Rows)
        {
            Row.resize(Cols);
            for (Rml::Variant& Cell : Row)
            {
                Cell = Rml::String();
            }
        }
    }

    void DataModelListSetCell(void* ModelPtr, int32 ListField, int32 Row, int32 Col, FStringView Value)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (ModelPtr == nullptr)
        {
            return;
        }
        FManagedDataModel* M = static_cast<FManagedDataModel*>(ModelPtr);
        if (ListField < 0 || ListField >= (int32)M->Lists.size())
        {
            return;
        }
        FListField* L = M->Lists[ListField].get();
        if (Row >= 0 && Row < (int32)L->Rows.size() && Col >= 0 && Col < (int32)L->Rows[Row].size())
        {
            L->Rows[Row][Col] = Rml::String(Value.data(), Value.size());
        }
    }

    void DataModelListDirty(void* ModelPtr, int32 ListField)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (ModelPtr == nullptr)
        {
            return;
        }
        FManagedDataModel* M = static_cast<FManagedDataModel*>(ModelPtr);
        if (M->Handle && ListField >= 0 && ListField < (int32)M->Lists.size())
        {
            M->Handle.DirtyVariable(M->Lists[ListField]->Name);
        }
    }

    void DataModelSetNumber(void* ModelPtr, int32 Field, double Value)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (ModelPtr == nullptr)
        {
            return;
        }
        FManagedDataModel* M = static_cast<FManagedDataModel*>(ModelPtr);
        if (Field >= 0 && Field < (int32)M->Values.size())
        {
            StoreNumber(M->Values[Field], M->Types[Field], Value);
        }
    }

    void DataModelSetString(void* ModelPtr, int32 Field, FStringView Value)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (ModelPtr == nullptr)
        {
            return;
        }
        FManagedDataModel* M = static_cast<FManagedDataModel*>(ModelPtr);
        if (Field >= 0 && Field < (int32)M->Values.size())
        {
            M->Values[Field] = Rml::String(Value.data(), Value.size());
        }
    }

    void DataModelDirty(void* ModelPtr, int32 Field)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (ModelPtr == nullptr)
        {
            return;
        }
        FManagedDataModel* M = static_cast<FManagedDataModel*>(ModelPtr);
        if (M->Handle && Field >= 0 && Field < (int32)M->VarNames.size())
        {
            M->Handle.DirtyVariable(M->VarNames[Field]);
        }
    }

    void DataModelDirtyAll(void* ModelPtr)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (ModelPtr == nullptr)
        {
            return;
        }
        FManagedDataModel* M = static_cast<FManagedDataModel*>(ModelPtr);
        if (M->Handle)
        {
            M->Handle.DirtyAllVariables();
        }
    }

    void DestroyDataModel(void* ModelPtr)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (ModelPtr == nullptr)
        {
            return;
        }
        // Validate against the live set, since a torn-down world already reaped the model.
        for (size_t i = 0; i < State.DataModels.size(); ++i)
        {
            FManagedDataModel* M = State.DataModels[i];
            if (M != ModelPtr)
            {
                continue;
            }
            if (State.bInitialized && M->World != nullptr)
            {
                if (FWorldUIContext* UI = WorldUI(M->World))
                {
                    if (UI->Context != nullptr)
                    {
                        UI->Context->RemoveDataModel(M->Name);
                    }
                }
            }
            else if (State.bInitialized && M->OwnerContext != nullptr)
            {
                M->OwnerContext->RemoveDataModel(M->Name);
            }
            State.DataModels[i] = State.DataModels.back();
            State.DataModels.pop_back();
            delete M;
            return;
        }
    }



    namespace
    {
        // The name of an enum value without its type prefix, empty when the value has no name such as a bitmask.
        Rml::String EnumValueName(const FEnumProperty* Property, int64 Raw)
        {
            if (CEnum* Enum = Property->GetEnum())
            {
                for (const TPair<FName, uint64>& Entry : Enum->Names)
                {
                    if ((int64)Entry.second == Raw)
                    {
                        const Rml::String Qualified = Entry.first.c_str();
                        const size_t Separator = Qualified.rfind("::");
                        return Separator == Rml::String::npos ? Qualified : Qualified.substr(Separator + 2);
                    }
                }
            }
            return Rml::String();
        }

        bool EnumValueByName(const FEnumProperty* Property, const Rml::String& Name, int64& OutRaw)
        {
            CEnum* Enum = Property->GetEnum();
            if (Enum == nullptr)
            {
                return false;
            }
            for (const TPair<FName, uint64>& Entry : Enum->Names)
            {
                const Rml::String Qualified = Entry.first.c_str();
                const size_t Separator = Qualified.rfind("::");
                if (Qualified == Name || (Separator != Rml::String::npos && Qualified.compare(Separator + 2, Rml::String::npos, Name) == 0))
                {
                    OutRaw = (int64)Entry.second;
                    return true;
                }
            }
            return false;
        }

        // The C runtime prints a NaN as -nan(ind), so a value that is not a number reads as its name instead.
        template<typename TNumber>
        Rml::Variant FiniteOrName(TNumber Number)
        {
            if (std::isnan(Number))
            {
                return Rml::Variant(Rml::String("NaN"));
            }
            if (std::isinf(Number))
            {
                return Rml::Variant(Rml::String(Number > 0 ? "Infinity" : "-Infinity"));
            }
            return Rml::Variant(Number);
        }

        // Reads a bound value into what RmlUi shows. Numbers stay numbers, so data-if and comparisons work on them, and an enum reads as its name.
        bool ReadVariant(const FProperty* Property, const void* Value, Rml::Variant& Out)
        {
            switch (Property->GetType())
            {
                case EPropertyTypeFlags::Bool:   Out = *static_cast<const bool*>(Value); return true;
                case EPropertyTypeFlags::Int8:   Out = (int)*static_cast<const int8*>(Value); return true;
                case EPropertyTypeFlags::Int16:  Out = (int)*static_cast<const int16*>(Value); return true;
                case EPropertyTypeFlags::Int32:  Out = (int)*static_cast<const int32*>(Value); return true;
                case EPropertyTypeFlags::Int64:  Out = (int64_t)*static_cast<const int64*>(Value); return true;
                case EPropertyTypeFlags::UInt8:  Out = (int)*static_cast<const uint8*>(Value); return true;
                case EPropertyTypeFlags::UInt16: Out = (int)*static_cast<const uint16*>(Value); return true;
                case EPropertyTypeFlags::UInt32: Out = (int64_t)*static_cast<const uint32*>(Value); return true;
                case EPropertyTypeFlags::UInt64: Out = (int64_t)*static_cast<const uint64*>(Value); return true;
                case EPropertyTypeFlags::Float:  Out = FiniteOrName(*static_cast<const float*>(Value)); return true;
                case EPropertyTypeFlags::Double: Out = FiniteOrName(*static_cast<const double*>(Value)); return true;
                case EPropertyTypeFlags::Entity: Out = (int64_t)*static_cast<const uint32*>(Value); return true;
                case EPropertyTypeFlags::String:
                {
                    const FString& Text = *static_cast<const FString*>(Value);
                    Out = Rml::String(Text.c_str(), Text.size());
                    return true;
                }
                case EPropertyTypeFlags::Name:
                    Out = Rml::String(static_cast<const FName*>(Value)->c_str());
                    return true;
                case EPropertyTypeFlags::Enum:
                {
                    const FEnumProperty* EnumProperty = static_cast<const FEnumProperty*>(Property);
                    const int64 Raw = EnumProperty->GetInnerProperty()->GetSignedIntPropertyValue(Value);
                    const Rml::String Name = EnumValueName(EnumProperty, Raw);
                    Out = Name.empty() ? Rml::Variant((int64_t)Raw) : Rml::Variant(Name);
                    return true;
                }
                default:
                    return false;
            }
        }

        // False when In does not convert, such as text where a number goes, so the value is left as it was.
        bool WriteVariant(const FProperty* Property, void* Value, const Rml::Variant& In)
        {
            auto WriteNumber = [&In, Value]<typename TNumber, typename TRead>(TNumber*, TRead*) -> bool
            {
                TRead Read{};
                if (!In.GetInto(Read))
                {
                    return false;
                }
                *static_cast<TNumber*>(Value) = (TNumber)Read;
                return true;
            };

            switch (Property->GetType())
            {
                case EPropertyTypeFlags::Bool:   return WriteNumber((bool*)nullptr,   (bool*)nullptr);
                case EPropertyTypeFlags::Int8:   return WriteNumber((int8*)nullptr,   (int*)nullptr);
                case EPropertyTypeFlags::Int16:  return WriteNumber((int16*)nullptr,  (int*)nullptr);
                case EPropertyTypeFlags::Int32:  return WriteNumber((int32*)nullptr,  (int*)nullptr);
                case EPropertyTypeFlags::Int64:  return WriteNumber((int64*)nullptr,  (int64_t*)nullptr);
                case EPropertyTypeFlags::UInt8:  return WriteNumber((uint8*)nullptr,  (int*)nullptr);
                case EPropertyTypeFlags::UInt16: return WriteNumber((uint16*)nullptr, (int*)nullptr);
                case EPropertyTypeFlags::UInt32: return WriteNumber((uint32*)nullptr, (int64_t*)nullptr);
                case EPropertyTypeFlags::UInt64: return WriteNumber((uint64*)nullptr, (int64_t*)nullptr);
                case EPropertyTypeFlags::Float:  return WriteNumber((float*)nullptr,  (float*)nullptr);
                case EPropertyTypeFlags::Double: return WriteNumber((double*)nullptr, (double*)nullptr);
                case EPropertyTypeFlags::String:
                {
                    const Rml::String Text = In.Get<Rml::String>();
                    *static_cast<FString*>(Value) = FString(Text.c_str(), Text.size());
                    return true;
                }
                case EPropertyTypeFlags::Name:
                    *static_cast<FName*>(Value) = FName(In.Get<Rml::String>().c_str());
                    return true;
                case EPropertyTypeFlags::Enum:
                {
                    const FEnumProperty* EnumProperty = static_cast<const FEnumProperty*>(Property);
                    int64 Raw = 0;
                    if (!(In.GetType() == Rml::Variant::STRING && EnumValueByName(EnumProperty, In.Get<Rml::String>(), Raw)))
                    {
                        int64_t Number = 0;
                        if (!In.GetInto(Number))
                        {
                            return false;
                        }
                        Raw = Number;
                    }
                    EnumProperty->GetInnerProperty()->SetIntPropertyValue(Value, Raw);
                    return true;
                }
                default:
                    return false;
            }
        }

        bool IsScalarType(EPropertyTypeFlags Type)
        {
            switch (Type)
            {
                case EPropertyTypeFlags::Bool:   case EPropertyTypeFlags::Int8:   case EPropertyTypeFlags::Int16:
                case EPropertyTypeFlags::Int32:  case EPropertyTypeFlags::Int64:  case EPropertyTypeFlags::UInt8:
                case EPropertyTypeFlags::UInt16: case EPropertyTypeFlags::UInt32: case EPropertyTypeFlags::UInt64:
                case EPropertyTypeFlags::Float:  case EPropertyTypeFlags::Double: case EPropertyTypeFlags::String:
                case EPropertyTypeFlags::Name:   case EPropertyTypeFlags::Enum:   case EPropertyTypeFlags::Entity:
                    return true;
                default:
                    return false;
            }
        }

        // A Change function runs with no arguments or with (Old, New) of the bound property's own type, since anything else would copy the value into the wrong type.
        const FFunction* FindChangeHandler(const CClass* Class, const FProperty* Property, bool bReport)
        {
            const FCStringView ChangeName = Property->GetMetadata("Change");
            if (ChangeName.empty() || Class == nullptr)
            {
                return nullptr;
            }
            const FString HandlerName(ChangeName.data(), ChangeName.size());
            const FFunction* Change = Class->FindFunction(FName(HandlerName.c_str()));
            if (Change == nullptr)
            {
                if (bReport)
                {
                    LOG_WARN("[RmlUi] {}.{} names Change = {}, which is not a reflected function of the class.",
                        Class->GetName().c_str(), Property->GetPropertyName().c_str(), HandlerName.c_str());
                }
                return nullptr;
            }
            const TSpan<FProperty* const> Params = Change->GetArguments();
            const bool bCallable = Params.empty() || (Params.size() == 2 && Params[0]->HasSameValueType(Property) && Params[1]->HasSameValueType(Property));
            if (!bCallable)
            {
                if (bReport)
                {
                    LOG_WARN("[RmlUi] {}.{} names Change = {}, which must take no arguments or (Old, New) of type {}, so it will not run.",
                        Class->GetName().c_str(), Property->GetPropertyName().c_str(), HandlerName.c_str(), PropertyTypePlainNames[(size_t)Property->GetType()]);
                }
                return nullptr;
            }
            return Change;
        }

        // A frame big enough for a function's parameters, aligned the way FFunction expects.
        struct FModelCallFrame
        {
            explicit FModelCallFrame(const FFunction& InFunction)
                : Function(InFunction)
                , Storage(InFunction.GetParmsSize() / sizeof(std::max_align_t) + 1)
            {
                Function.InitializeFrame(Storage.data());
            }

            ~FModelCallFrame()
            {
                Function.DestructFrame(Storage.data());
            }

            void* Data() { return Storage.data(); }

            const FFunction& Function;
            TVector<std::max_align_t> Storage;
        };

        struct FObjectModel;

        class FObjectValueDefinition final : public Rml::VariableDefinition
        {
        public:
            FObjectValueDefinition(FObjectModel* InModel, const FProperty* InProperty, const FProperty* InRoot)
                : Rml::VariableDefinition(Rml::DataVariableType::Scalar), Model(InModel), Property(InProperty), Root(InRoot) {}

            bool Get(void* Ptr, Rml::Variant& Out) override;
            bool Set(void* Ptr, const Rml::Variant& In) override;

        private:
            FObjectModel*    Model;
            const FProperty* Property;
            const FProperty* Root;
        };

        class FObjectStructDefinition final : public Rml::VariableDefinition
        {
        public:
            FObjectStructDefinition(FObjectModel* InModel, const CStruct* InStruct, const FProperty* InRoot)
                : Rml::VariableDefinition(Rml::DataVariableType::Struct), Model(InModel), Struct(InStruct), Root(InRoot) {}

            Rml::DataVariable Child(void* Ptr, const Rml::DataAddressEntry& Address) override;
            Rml::StringList ReflectMemberNames() override;

        private:
            FObjectModel*    Model;
            const CStruct*   Struct;
            const FProperty* Root;
        };

        class FObjectArrayDefinition final : public Rml::VariableDefinition
        {
        public:
            FObjectArrayDefinition(FObjectModel* InModel, const FArrayProperty* InArray, const FProperty* InRoot)
                : Rml::VariableDefinition(Rml::DataVariableType::Array), Model(InModel), Array(InArray), Root(InRoot) {}

            int Size(void* Ptr) override;
            Rml::DataVariable Child(void* Ptr, const Rml::DataAddressEntry& Address) override;

        private:
            FObjectModel*         Model;
            const FArrayProperty* Array;
            const FProperty*      Root;
        };

        // One CObject bound as a data model. RmlUi reads and writes its memory in place through these definitions.
        struct FObjectModel
        {
            struct FValue
            {
                FProperty*  Property = nullptr;
                Rml::String Name;
            };

            struct FComputed
            {
                const FFunction* Function = nullptr;
                Rml::String      Name;
                Rml::Variant     Last;
            };

            struct FDefinitionEntry
            {
                const FProperty*         Property = nullptr;
                const FProperty*         Root = nullptr;
                Rml::VariableDefinition* Definition = nullptr;
            };

            Rml::Context*             Context = nullptr;
            Rml::String               Name;
            Rml::DataModelConstructor Constructor;
            Rml::DataModelHandle      Handle;
            CObject*                  Object = nullptr;
            bool                      bDesignTime = false;

            // Destroyed during an event, so it waits for the next poll to leave RmlUi and must not touch Object meanwhile.
            bool                      bDead = false;
            TVector<FValue>           Values;
            TVector<FComputed>        Computed;
            FPropertySnapshot         Snapshot;
            TVector<FDefinitionEntry> DefinitionIndex;
            TVector<TUniquePtr<Rml::VariableDefinition>> Definitions;
            TVector<FString>          FiredCommands;
            TVector<Rml::String>      Warned;

            // Markup mistakes repeat every click, so each one is reported once per model.
            template<typename... TArgs>
            void WarnOnce(const Rml::String& Key, Fmt::TFormatString<std::decay_t<TArgs>...> Format, TArgs&&... Args)
            {
                if (std::find(Warned.begin(), Warned.end(), Key) != Warned.end())
                {
                    return;
                }
                Warned.push_back(Key);
                Logging::Log(ELogLevel::Warn, Format, std::forward<TArgs>(Args)...);
            }

            // Null for a type a UI cannot show, such as an object reference or a map.
            Rml::VariableDefinition* DefinitionFor(const FProperty* Property, const FProperty* InRoot)
            {
                for (const FDefinitionEntry& Entry : DefinitionIndex)
                {
                    if (Entry.Property == Property && Entry.Root == InRoot)
                    {
                        return Entry.Definition;
                    }
                }

                TUniquePtr<Rml::VariableDefinition> Created;
                const EPropertyTypeFlags Type = Property->GetType();
                if (Type == EPropertyTypeFlags::Struct)
                {
                    Created = MakeUnique<FObjectStructDefinition>(this, static_cast<const FStructProperty*>(Property)->GetStruct(), InRoot);
                }
                else if (Type == EPropertyTypeFlags::Vector)
                {
                    Created = MakeUnique<FObjectArrayDefinition>(this, static_cast<const FArrayProperty*>(Property), InRoot);
                }
                else if (IsScalarType(Type))
                {
                    Created = MakeUnique<FObjectValueDefinition>(this, Property, InRoot);
                }
                if (!Created)
                {
                    return nullptr;
                }

                Rml::VariableDefinition* Definition = Created.get();
                Definitions.push_back(Move(Created));
                DefinitionIndex.push_back(FDefinitionEntry{ Property, InRoot, Definition });
                return Definition;
            }

            void Evaluate(const FComputed& Entry, Rml::Variant& Out)
            {
                // A preview never runs game code, so it shows the name where the value would go.
                if (bDesignTime || Object == nullptr)
                {
                    Out = Entry.Name;
                    return;
                }
                FModelCallFrame Frame(*Entry.Function);
                Entry.Function->Invoke(Object, Frame.Data());
                const FProperty* Return = Entry.Function->GetReturnParam();
                if (Return == nullptr || !ReadVariant(Return, Return->GetValuePtr<void>(Frame.Data()), Out))
                {
                    Out = Rml::Variant();
                }
            }

            void RunCommand(const FFunction* Function, const Rml::String& CommandName, const Rml::VariantList& Arguments)
            {
                if (bDesignTime)
                {
                    if (FiredCommands.size() < 64)
                    {
                        Rml::String Call = CommandName + "(";
                        for (size_t Index = 0; Index < Arguments.size(); ++Index)
                        {
                            Call += (Index > 0 ? ", " : "") + Arguments[Index].Get<Rml::String>();
                        }
                        FiredCommands.push_back(FString((Call + ")").c_str()));
                    }
                    return;
                }
                if (Object == nullptr)
                {
                    return;
                }

                FEventDispatchScope Dispatch;
                FModelCallFrame Frame(*Function);
                const TSpan<FProperty* const> Params = Function->GetArguments();
                if (Params.size() != Arguments.size())
                {
                    WarnOnce(CommandName, "[RmlUi] {}.{} takes {} argument(s) but the markup passes {}; missing ones run as defaults and extra ones are dropped.",
                        Name.c_str(), CommandName.c_str(), Params.size(), Arguments.size());
                }
                for (size_t Index = 0; Index < Params.size() && Index < Arguments.size(); ++Index)
                {
                    if (!WriteVariant(Params[Index], Params[Index]->GetValuePtr<void>(Frame.Data()), Arguments[Index]))
                    {
                        WarnOnce(CommandName + "#" + std::to_string(Index), "[RmlUi] {}.{} could not read '{}' as its {} argument {}, so it runs with the default.",
                            Name.c_str(), CommandName.c_str(), Arguments[Index].Get<Rml::String>().c_str(), PropertyTypePlainNames[(size_t)Params[Index]->GetType()],
                            Params[Index]->GetPropertyName().c_str());
                    }
                }
                Function->Invoke(Object, Frame.Data());
                NoteUIChanged();
            }

            // A two-way view wrote Root, so its PROPERTY(Change = ...) function runs with the old and new values.
            void ValueWrittenByView(const FProperty* InRoot)
            {
                NoteUIChanged();
                const int32 Index = Snapshot.IndexOf(InRoot);
                if (Index < 0 || Object == nullptr)
                {
                    return;
                }
                // A text field submits on Enter as well as on each edit, which writes back the value it already has.
                if (Snapshot.IsTracked(Index) && Snapshot.Matches(Index, Object))
                {
                    return;
                }

                const FFunction* Change = !bDesignTime ? FindChangeHandler(Object->GetClass(), InRoot, false) : nullptr;
                if (Change != nullptr)
                {
                    FEventDispatchScope Dispatch;
                    const TSpan<FProperty* const> Params = Change->GetArguments();
                    FModelCallFrame Frame(*Change);
                    if (Params.size() == 2)
                    {
                        Params[0]->CopyCompleteValue(Params[0]->GetValuePtr<void>(Frame.Data()), Snapshot.IsTracked(Index) ? Snapshot.Slot(Index) : InRoot->GetValuePtr<void>(Object));
                        Params[1]->CopyCompleteValue(Params[1]->GetValuePtr<void>(Frame.Data()), InRoot->GetValuePtr<void>(Object));
                    }
                    Change->Invoke(Object, Frame.Data());
                }
                // The handler may have destroyed the script this model reads.
                if (Object != nullptr)
                {
                    Snapshot.Store(Index, Object);
                }
            }

            void Poll()
            {
                if (bDead || Object == nullptr)
                {
                    return;
                }
                bool bChanged = false;
                for (size_t Index = 0; Index < Values.size(); ++Index)
                {
                    if (!Snapshot.Matches(Index, Object))
                    {
                        Snapshot.Store(Index, Object);
                        Handle.DirtyVariable(Values[Index].Name);
                        bChanged = true;
                    }
                }
                if (!bDesignTime)
                {
                    for (FComputed& Entry : Computed)
                    {
                        Rml::Variant Current;
                        Evaluate(Entry, Current);
                        if (!(Current == Entry.Last))
                        {
                            Entry.Last = Current;
                            Handle.DirtyVariable(Entry.Name);
                            bChanged = true;
                        }
                    }
                }
                if (bChanged)
                {
                    NoteUIChanged();
                }
            }
        };

        bool FObjectValueDefinition::Get(void* Ptr, Rml::Variant& Out)
        {
            return !Model->bDead && ReadVariant(Property, Ptr, Out);
        }

        int FObjectArrayDefinition::Size(void* Ptr)
        {
            return Model->bDead ? 0 : (int)Array->GetNum(Ptr);
        }

        bool FObjectValueDefinition::Set(void* Ptr, const Rml::Variant& In)
        {
            if (Model->bDead || !WriteVariant(Property, Ptr, In))
            {
                return false;
            }
            Model->ValueWrittenByView(Root);
            return true;
        }

        Rml::DataVariable FObjectStructDefinition::Child(void* Ptr, const Rml::DataAddressEntry& Address)
        {
            if (Address.name.empty() || Model->bDead)
            {
                return Rml::DataVariable();
            }
            FProperty* Member = Struct->GetProperty(FName(Address.name.c_str()));
            Rml::VariableDefinition* Definition = Member != nullptr ? Model->DefinitionFor(Member, Root) : nullptr;
            return Definition != nullptr ? Rml::DataVariable(Definition, Member->GetValuePtr<void>(Ptr)) : Rml::DataVariable();
        }

        Rml::StringList FObjectStructDefinition::ReflectMemberNames()
        {
            Rml::StringList Names;
            for (const FProperty* Member : Struct->GetProperties())
            {
                Names.push_back(Member->GetPropertyName().c_str());
            }
            return Names;
        }

        Rml::DataVariable FObjectArrayDefinition::Child(void* Ptr, const Rml::DataAddressEntry& Address)
        {
            const int Count = Size(Ptr);
            if (Address.index < 0 && Address.name == "size")
            {
                return Rml::MakeLiteralIntVariable(Count);
            }
            if (Address.index < 0 || Address.index >= Count)
            {
                return Rml::DataVariable();
            }
            Rml::VariableDefinition* Definition = Model->DefinitionFor(Array->GetInternalProperty(), Root);
            return Definition != nullptr ? Rml::DataVariable(Definition, Array->GetAt(Ptr, (size_t)Address.index)) : Rml::DataVariable();
        }

        TVector<FObjectModel*>& ObjectModels()
        {
            static TVector<FObjectModel*> Models;
            return Models;
        }

        void ReapContextObjectModels(Rml::Context* Context)
        {
            TVector<FObjectModel*>& Models = ObjectModels();
            for (size_t Index = 0; Index < Models.size();)
            {
                if (Models[Index]->Context == Context)
                {
                    delete Models[Index];
                    Models[Index] = Models.back();
                    Models.pop_back();
                }
                else
                {
                    ++Index;
                }
            }
        }
    }

    void* CreateObjectModel(Rml::Context* Context, FStringView Name, CObject* Object, bool bDesignTime)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || Context == nullptr || Object == nullptr || Name.empty())
        {
            return nullptr;
        }

        const Rml::String ModelName(Name.data(), Name.size());
        Rml::DataModelConstructor Constructor = Context->CreateDataModel(ModelName);
        if (!Constructor)
        {
            LOG_WARN("[RmlUi] Data model '{}' already exists in this context, so {} could not bind as it.", ModelName.c_str(), Object->GetClass()->GetName().c_str());
            return nullptr;
        }

        FObjectModel* Model = new FObjectModel();
        Model->Context     = Context;
        Model->Name        = ModelName;
        Model->Constructor = Constructor;
        Model->Handle      = Constructor.GetModelHandle();
        Model->Object      = Object;
        Model->bDesignTime = bDesignTime;
        ObjectModels().push_back(Model);

        TVector<UIBinding::FBoundValue> Values;
        UIBinding::GetBoundValues(Object->GetClass(), Values);
        TVector<FProperty*> Snapshotted;
        for (const UIBinding::FBoundValue& Value : Values)
        {
            Rml::VariableDefinition* Definition = Model->DefinitionFor(Value.Property, Value.Property);
            if (Definition == nullptr)
            {
                LOG_WARN("[RmlUi] {}.{} is bound but its type cannot be shown in a UI.", Object->GetClass()->GetName().c_str(), Value.Property->GetPropertyName().c_str());
                continue;
            }
            const Rml::String VariableName(Value.Name.c_str(), Value.Name.size());
            Constructor.BindCustomDataVariable(VariableName, Rml::DataVariable(Definition, Value.Property->GetValuePtr<void>(Object)));
            Model->Values.push_back(FObjectModel::FValue{ Value.Property, VariableName });
            Snapshotted.push_back(Value.Property);
            if (!bDesignTime)
            {
                FindChangeHandler(Object->GetClass(), Value.Property, true);
            }
        }
        Model->Snapshot.Capture(Snapshotted, Object);

        TVector<UIBinding::FBoundFunction> Functions;
        UIBinding::GetBoundFunctions(Object->GetClass(), Functions);
        for (const UIBinding::FBoundFunction& Bound : Functions)
        {
            const Rml::String FunctionName(Bound.Name.c_str(), Bound.Name.size());
            if (Bound.bComputed)
            {
                Model->Computed.push_back(FObjectModel::FComputed{ Bound.Function, FunctionName, Rml::Variant() });
                const size_t Index = Model->Computed.size() - 1;
                Model->Evaluate(Model->Computed[Index], Model->Computed[Index].Last);
                Constructor.BindFunc(FunctionName, [Model, Index](Rml::Variant& Out) { Out = Model->Computed[Index].Last; });
            }
            else
            {
                const FFunction* Function = Bound.Function;
                Constructor.BindEventCallback(FunctionName, [Model, Function, FunctionName](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& Arguments)
                {
                    Model->RunCommand(Function, FunctionName, Arguments);
                });
            }
        }
        return Model;
    }

    void DestroyObjectModel(void* ModelPtr)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        TVector<FObjectModel*>& Models = ObjectModels();
        for (size_t Index = 0; Index < Models.size(); ++Index)
        {
            FObjectModel* Model = Models[Index];
            if (Model != ModelPtr)
            {
                continue;
            }
            if (GEventDispatchDepth > 0)
            {
                Model->bDead = true;
                Model->Object = nullptr;
                return;
            }
            if (State.bInitialized && Model->Context != nullptr)
            {
                Model->Context->RemoveDataModel(Model->Name);
            }
            Models[Index] = Models.back();
            Models.pop_back();
            delete Model;
            return;
        }
    }

    void PollObjectModels(Rml::Context* Context)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        TVector<FObjectModel*>& Models = ObjectModels();
        for (size_t Index = 0; Index < Models.size();)
        {
            FObjectModel* Model = Models[Index];
            if (Model->Context == Context && Model->bDead && GEventDispatchDepth == 0)
            {
                Context->RemoveDataModel(Model->Name);
                Models[Index] = Models.back();
                Models.pop_back();
                delete Model;
                continue;
            }
            if (Model->Context == Context)
            {
                Model->Poll();
            }
            ++Index;
        }
    }

    bool SetObjectModelValue(void* ModelPtr, FStringView Variable, FStringView Text)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        for (FObjectModel* Model : ObjectModels())
        {
            if (Model != ModelPtr)
            {
                continue;
            }
            void* Container = nullptr;
            if (const FProperty* Property = UIBinding::ResolvePath(Model->Object, Variable, Container))
            {
                const bool bSet = Reflection::FromText(Property, Container, Text);
                NoteUIChanged();
                return bSet;
            }
        }
        return false;
    }

    FString GetObjectModelValue(void* ModelPtr, FStringView Variable)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        for (FObjectModel* Model : ObjectModels())
        {
            if (Model != ModelPtr)
            {
                continue;
            }
            void* Container = nullptr;
            if (const FProperty* Property = UIBinding::ResolvePath(Model->Object, Variable, Container))
            {
                return Reflection::ToText(Property, Container);
            }
        }
        return FString();
    }

    namespace
    {
        FManagedDataModel* FindDesignModel(FEditorEntry& Entry, FStringView Name)
        {
            for (void* Model : Entry.DesignModels)
            {
                FManagedDataModel* M = static_cast<FManagedDataModel*>(Model);
                if (FStringView(M->Name.c_str(), M->Name.size()) == Name)
                {
                    return M;
                }
            }
            return nullptr;
        }

        int32 FindDesignField(const FManagedDataModel& M, FStringView Variable)
        {
            for (size_t Index = 0; Index < M.VarNames.size(); ++Index)
            {
                if (FStringView(M.VarNames[Index].c_str(), M.VarNames[Index].size()) == Variable)
                {
                    return (int32)Index;
                }
            }
            return -1;
        }

        void StoreDesignValue(FManagedDataModel* M, int32 Field, EUIVarType Type, FStringView Value)
        {
            if (Type == EUIVarType::String)
            {
                DataModelSetString(M, Field, Value);
                return;
            }
            const FString Text(Value.data(), Value.size());
            double Number = 0.0;
            if (Text == "true")
            {
                Number = 1.0;
            }
            else if (!Text.empty())
            {
                Number = std::strtod(Text.c_str(), nullptr);
            }
            DataModelSetNumber(M, Field, Number);
        }

        void FillDesignRows(FManagedDataModel* M, int32 ListField, const TVector<TVector<FString>>& Rows)
        {
            DataModelListResize(M, ListField, (int32)Rows.size());
            for (size_t Row = 0; Row < Rows.size(); ++Row)
            {
                for (size_t Col = 0; Col < Rows[Row].size(); ++Col)
                {
                    DataModelListSetCell(M, ListField, (int32)Row, (int32)Col, FStringView(Rows[Row][Col].c_str(), Rows[Row][Col].size()));
                }
            }
        }

        int32 FindDesignList(FManagedDataModel& M, FStringView List)
        {
            for (size_t Index = 0; Index < M.Lists.size(); ++Index)
            {
                if (M.Lists[Index] && FStringView(M.Lists[Index]->Name.c_str(), M.Lists[Index]->Name.size()) == List)
                {
                    return (int32)Index;
                }
            }
            return -1;
        }
    }

    void SetEditorDesignModels(Rml::Context* Context, const TVector<FUIDesignModel>& Models)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        FEditorEntry* Entry = State.bInitialized ? FindEditorEntry(Context) : nullptr;
        if (Entry == nullptr)
        {
            return;
        }

        // Dropped first, since a document still bound to an old model would keep reading it.
        if (Entry->Document != nullptr)
        {
            Context->UnloadDocument(Entry->Document);
            Entry->Document = nullptr;
            Context->Update();
        }
        for (void* Model : Entry->DesignModels)
        {
            DestroyDataModel(Model);
        }
        Entry->DesignModels.clear();

        for (const FUIDesignModel& Design : Models)
        {
            const Rml::String ModelName(Design.Name.c_str(), Design.Name.size());
            Rml::DataModelConstructor Ctor = Context->CreateDataModel(ModelName);
            if (!Ctor)
            {
                continue;
            }

            FManagedDataModel* M = new FManagedDataModel();
            M->Name         = ModelName;
            M->Constructor  = Ctor;
            M->Handle       = Ctor.GetModelHandle();
            M->OwnerContext = Context;
            State.DataModels.push_back(M);
            Entry->DesignModels.push_back(M);

            for (const FUIDesignScalar& Scalar : Design.Scalars)
            {
                const int32 Field = DataModelBindScalar(M, FStringView(Scalar.Name.c_str(), Scalar.Name.size()), (int32)Scalar.Type);
                if (Field >= 0)
                {
                    StoreDesignValue(M, Field, Scalar.Type, FStringView(Scalar.Value.c_str(), Scalar.Value.size()));
                }
            }

            for (const FUIDesignList& List : Design.Lists)
            {
                const int32 ListField = DataModelBindList(M, FStringView(List.Name.c_str(), List.Name.size()));
                if (ListField < 0)
                {
                    continue;
                }
                for (const FString& Member : List.Members)
                {
                    DataModelBindListMember(M, ListField, FStringView(Member.c_str(), Member.size()));
                }
                FillDesignRows(M, ListField, List.Rows);
            }

            for (size_t Index = 0; Index < Design.Commands.size(); ++Index)
            {
                const FString& Name = Design.Commands[Index].Name;
                DataModelBindCommand(M, FStringView(Name.c_str(), Name.size()), (int32)Index);
            }
            DataModelDirtyAll(M);
        }
    }

    void SetEditorDesignValue(Rml::Context* Context, FStringView Model, FStringView Variable, FStringView Value)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        FEditorEntry* Entry = State.bInitialized ? FindEditorEntry(Context) : nullptr;
        FManagedDataModel* M = Entry != nullptr ? FindDesignModel(*Entry, Model) : nullptr;
        const int32 Field = M != nullptr ? FindDesignField(*M, Variable) : -1;
        if (Field < 0)
        {
            return;
        }
        StoreDesignValue(M, Field, (EUIVarType)M->Types[Field], Value);
        DataModelDirty(M, Field);
    }

    FString GetEditorDesignValue(Rml::Context* Context, FStringView Model, FStringView Variable)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        FEditorEntry* Entry = State.bInitialized ? FindEditorEntry(Context) : nullptr;
        FManagedDataModel* M = Entry != nullptr ? FindDesignModel(*Entry, Model) : nullptr;
        const int32 Field = M != nullptr ? FindDesignField(*M, Variable) : -1;
        if (Field < 0)
        {
            return FString();
        }
        const Rml::String Text = M->Values[Field].Get<Rml::String>();
        return FString(Text.c_str(), Text.size());
    }

    void SetEditorDesignRows(Rml::Context* Context, FStringView Model, FStringView List, const TVector<TVector<FString>>& Rows)
    {
        NoteUIChanged();
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        FEditorEntry* Entry = State.bInitialized ? FindEditorEntry(Context) : nullptr;
        FManagedDataModel* M = Entry != nullptr ? FindDesignModel(*Entry, Model) : nullptr;
        const int32 ListField = M != nullptr ? FindDesignList(*M, List) : -1;
        if (ListField < 0)
        {
            return;
        }
        FillDesignRows(M, ListField, Rows);
        DataModelListDirty(M, ListField);
    }

    void ConsumeEditorDesignCommands(Rml::Context* Context, TVector<FString>& Out)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        FEditorEntry* Entry = State.bInitialized ? FindEditorEntry(Context) : nullptr;
        if (Entry == nullptr)
        {
            return;
        }
        for (void* Model : Entry->DesignModels)
        {
            FManagedDataModel* M = static_cast<FManagedDataModel*>(Model);
            for (FString& Fired : M->FiredCommands)
            {
                Out.push_back(Move(Fired));
            }
            M->FiredCommands.clear();
        }
        for (FObjectModel* Model : ObjectModels())
        {
            if (Model->Context != Context)
            {
                continue;
            }
            for (FString& Fired : Model->FiredCommands)
            {
                Out.push_back(Move(Fired));
            }
            Model->FiredCommands.clear();
        }
    }
}
