#include "RuntimePCH.h"
#include "RmlUiBridge.h"

#include "RmlUiFileInterface.h"
#include "RmlUiRenderer.h"
#include "WorldUIContext.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/DataVariable.h>
#include <RmlUi/Core/Variant.h>
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
#include <filesystem>
#include "World/World.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "World/Entity/Components/WidgetComponent.h"

extern "C" void* LuminaRmlFreeTypeAlloc(size_t Size)
{
    LUMINA_MEMORY_SCOPE("RmlUi");
    return ::Lumina::Memory::Malloc(Size);
}

extern "C" void* LuminaRmlFreeTypeRealloc(void* Block, size_t NewSize)
{
    LUMINA_MEMORY_SCOPE("RmlUi");
    return ::Lumina::Memory::Realloc(Block, NewSize);
}

extern "C" void LuminaRmlFreeTypeFree(void* Block)
{
    ::Lumina::Memory::Free(Block);
}

namespace Lumina::RmlUi
{
    static TConsoleVar<int32> CVarWidgetMaxRendersPerFrame("UI.Widget.MaxRendersPerFrame", 8,
        "Max world-space widget RT rasterizations per frame; the rest reuse last frame's RT.");

    static TConsoleVar<int32> CVarWidgetDormancyFrames("UI.Widget.DormancyFrames", 4,
        "Frames of unchanged output before a world-space widget stops ticking; 0 = always tick.");

    // RmlUi has no implicit default font: an element whose cascade never sets 'font-family' renders nothing
    // ("No font face defined"). The bridge registers the engine font under this family and sets it on every
    // context's root element, so all documents inherit it unless they specify their own font-family.
    static constexpr const char* GDefaultUIFontFamily = "Lumina";

    namespace
    {
        class FLuminaSystemInterface final : public Rml::SystemInterface
        {
        public:
            FLuminaSystemInterface() : StartTime(std::chrono::steady_clock::now()) {}

            double GetElapsedTime() override
            {
                using namespace std::chrono;
                return duration<double>(steady_clock::now() - StartTime).count();
            }

            bool LogMessage(Rml::Log::Type Type, const Rml::String& Message) override
            {
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

                // Scheme-prefixed source (e.g. "material:/Game/..") -- a ':' before
                // any '/'. Pass through so custom URI schemes survive path joining.
                const size_t Colon = Path.find(':');
                const size_t Slash = Path.find('/');
                if (Colon != Rml::String::npos && (Slash == Rml::String::npos || Colon < Slash))
                {
                    OutPath = Path;
                    return;
                }

                // Relative path -> resolve against the document's directory,
                // preserving its leading '/'.
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
            std::chrono::steady_clock::time_point StartTime;
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
        };

        // Bridges one RmlUi element event to a managed callback (LuminaSharp World.UI). Owned by the bridge
        // (FState::UIListeners), reaped when the world's UI is destroyed. RmlUi does NOT delete listeners on
        // element destruction -- it only calls OnDetach -- so we track bAttached to avoid touching a freed
        // element if RemoveElementEventListener is called after the element is already gone.
        class FManagedUIListener final : public Rml::EventListener
        {
        public:
            FManagedUIListener(FManagedUIEventThunk InThunk, void* InContext, CWorld* InWorld, Rml::String InType)
                : Thunk(InThunk), Context(InContext), World(InWorld), Type(Move(InType)) {}

            void ProcessEvent(Rml::Event& Event) override
            {
                if (Thunk == nullptr)
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

                Thunk(Context, &Data);
            }

            void OnAttach(Rml::Element* InElement) override { Element = InElement; bAttached = true; }
            void OnDetach(Rml::Element*) override           { bAttached = false; }

            FManagedUIEventThunk Thunk     = nullptr;
            void*                Context   = nullptr;
            CWorld*              World     = nullptr;
            Rml::String          Type;
            Rml::Element*        Element   = nullptr;
            bool                 bAttached = false;
        };

        // A list (array-of-struct) bound for data-for. Backed by a snapshot of string cells the managed side
        // pushes on change (pull from RmlUi would mean a managed crossing per cell per refresh). Owns the
        // three custom VariableDefinitions that expose Rows to RmlUi; RmlUi only references them (it does not
        // own custom-bound DataVariables), so their lifetime is this struct's. Element/member identity is
        // packed into the DataVariable's void* handle: row in the high 32 bits, column in the low 32.
        struct FListField
        {
            Rml::String                             Name;
            TVector<Rml::String>                    MemberNames;   // column names ({{ item.Name }})
            TVector<TVector<Rml::Variant>>          Rows;          // Rows[row][col] -- string variants
            Rml::UniquePtr<Rml::VariableDefinition> ArrayDef;
            Rml::UniquePtr<Rml::VariableDefinition> StructDef;
            Rml::UniquePtr<Rml::VariableDefinition> MemberDef;
        };

        // Leaf: reads one cell. The handle packs (row << 32 | col).
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
                return false;
            }
        private:
            FListField* List;
        };

        // One row: resolves {{ item.<member> }} to a member-cell handle. Incoming handle carries the row index.
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

        // The array: Size + index -> row handle. ".size" is handled by RmlUi via the literal int path.
        class FListArrayDef final : public Rml::VariableDefinition
        {
        public:
            explicit FListArrayDef(FListField* InList) : Rml::VariableDefinition(Rml::DataVariableType::Array), List(InList) {}
            int Size(void* /*Ptr*/) override { return (int)List->Rows.size(); }
            Rml::DataVariable Child(void* /*Ptr*/, const Rml::DataAddressEntry& Address) override
            {
                const int Count = (int)List->Rows.size();
                const int Index = Address.index;
                if (Index < 0 || Index >= Count)
                {
                    if (Address.name == "size")
                    {
                        return Rml::MakeLiteralIntVariable(Count);
                    }
                    return Rml::DataVariable();
                }
                return Rml::DataVariable(List->StructDef.get(), reinterpret_cast<void*>((uintptr_t)Index));
            }
        private:
            FListField* List;
        };

        // A managed ViewModel's data model bound to a world context (World.UI.AddModel). Owns the per-variable
        // Variant cache the bound getters read; C# pushes values down and dirties them. Owned by the bridge
        // (FState::DataModels), reaped per-world on teardown like FManagedUIListener. Variant::Set is private,
        // so values are assigned through the public templated operator=.
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

            // Editor hot-reload: OnContentFileModified raises this on any .rml/.rcss save; next TickWorldUI restyles.
            TAtomic<bool>                       bUIReloadPending{false};

            CWorld*                             ActiveWorld = nullptr;

            Rml::Context*                       DebuggerHost = nullptr;
            bool                                bDebuggerVisible = false;
            bool                                bInitialized = false;

            uint32                              WidgetRenderCursor = 0;

            // Script-registered UI event listeners (World.UI). Owned here; reaped per-world on DestroyWorldUI
            // and en masse on Shutdown. Small N (a handful per screen), so linear scan on add/remove is fine.
            TVector<FManagedUIListener*>        UIListeners;

            // Script-registered data models (World.UI.AddModel). Owned here; reaped per-world like UIListeners.
            TVector<FManagedDataModel*>         DataModels;

            // Backing bytes for the default UI font. RmlUi's memory LoadFontFace references this buffer for the
            // face's lifetime (it does not copy), so it must outlive the font engine -- kept here until Shutdown.
            TVector<uint8>                      DefaultFontData;

            // Recursive: Update may fire event callbacks that re-enter the bridge on the same thread.
            FRecursiveMutex                     StateMutex;
        };

        FState& S()
        {
            static FState State;
            return State;
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
            const FFixedString Physical = VFS::ResolvePath(VirtualPath);
            if (Physical.empty())
            {
                return 0;
            }
            std::error_code Ec;
            const auto Time = std::filesystem::last_write_time(Physical.c_str(), Ec);
            return Ec ? 0 : (int64)Time.time_since_epoch().count();
        }

        // Establish the engine default font on a freshly created context: set GDefaultUIFontFamily on the
        // root element, which every document/element in the context inherits unless it sets its own
        // 'font-family'. Without this, RmlUi warns "No font face defined" and the text is invisible.
        void ApplyDefaultFontFamily(Rml::Context* Ctx)
        {
            if (Ctx == nullptr)
            {
                return;
            }
            if (Rml::Element* Root = Ctx->GetRootElement())
            {
                Root->SetProperty("font-family", GDefaultUIFontFamily);
            }
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

        void EnsureWidgetResources(FWidgetRuntime& E, CWorld* World, entt::entity Entity, uint32 Width, uint32 Height)
        {
            DestroyWidgetRuntime(E);

            char NameBuf[80];
            std::snprintf(NameBuf, sizeof(NameBuf), "widget_%p_%u",
                static_cast<void*>(World), entt::to_integral(Entity));

            E.Context = Rml::CreateContext(NameBuf, Rml::Vector2i(int(Width), int(Height)));
            if (E.Context == nullptr)
            {
                LOG_ERROR("[RmlUi] CreateContext failed for widget {}.", NameBuf);
                return;
            }
            ApplyDefaultFontFamily(E.Context);

            E.Target = RHI::Textures::Create(RHI::FTexture2DDesc
            {
                .Width  = Width,
                .Height = Height,
                .Format = EFormat::RGBA8_UNORM,
                .bRenderTarget = true,
            });
            E.ResourceID = E.Target.IsValid() ? (int32)E.Target.SampledSlot : -1;
            E.BuiltSize  = FUIntVector2(Width, Height);
        }

        void ProcessPendingUIReload()
        {
            FState& State = S();
            if (!State.bUIReloadPending.exchange(false, Atomic::MemoryOrderAcquire))
            {
                return;
            }

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
                    if (Rml::ElementDocument* Doc = Ctx->GetDocument(d))
                    {
                        Doc->ReloadStyleSheet();
                    }
                }
            }
            LOG_INFO("[RmlUi] UI hot-reload: restyled all documents across {} context(s).", NumContexts);
        }

        // Detach + delete every script UI listener bound to this world. Callers hold StateMutex. Safe whether
        // or not the elements still exist: a destroyed element already fired OnDetach (bAttached == false).
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

        // Delete every data model bound to this world. Callers hold StateMutex. The model's context is being
        // (or was already) destroyed by RemoveContext, which tears down the underlying Rml::DataModel too, so
        // we only free the wrapper here -- never touch Rml. An undisposed managed binding leaks its GCHandle.
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
                S().bUIReloadPending.store(true, Atomic::MemoryOrderRelease);
            }
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

        if (!State.Renderer->Initialize())
        {
            LOG_ERROR("[RmlUi] FRmlUiRenderer initialisation failed; tearing down.");
            Rml::Shutdown();
            Rml::SetRenderInterface(nullptr);
            Rml::SetFileInterface(nullptr);
            Rml::SetSystemInterface(nullptr);
            ResetOnFailure();
            return false;
        }

        // Register the default UI font under GDefaultUIFontFamily (from memory, so we control the family name).
        // ApplyDefaultFontFamily then sets it on each context root, giving every document a working font
        // without authoring 'font-family'. The byte buffer is kept in State for the font face's lifetime.
        constexpr const char* DefaultFontPath = "/Engine/Resources/UI/Fonts/LatoLatin-Regular.ttf";
        if (VFS::ReadFile(State.DefaultFontData, DefaultFontPath) && !State.DefaultFontData.empty())
        {
            const Rml::Span<const Rml::byte> FontSpan(State.DefaultFontData.data(), State.DefaultFontData.size());
            if (!Rml::LoadFontFace(FontSpan, GDefaultUIFontFamily, Rml::Style::FontStyle::Normal,
                    Rml::Style::FontWeight::Auto, true /*fallback_face*/))
            {
                LOG_WARN("[RmlUi] Default UI font failed to register as '{}'; text may not render.", GDefaultUIFontFamily);
            }
        }
        else
        {
            // Couldn't read the bytes: fall back to a path load so glyphs still exist (its family is derived
            // from the file metadata, so the GDefaultUIFontFamily default won't resolve -- author font-family).
            LOG_WARN("[RmlUi] Could not read default UI font '{}'; falling back to path load.", DefaultFontPath);
            Rml::LoadFontFace(DefaultFontPath, true /*fallback_face*/);
        }

        // Monospace face for digit-heavy HUDs (speedometer, RPM, etc.). Optional;
        // RmlUi will fall back to LatoLatin if the file is missing.
        Rml::LoadFontFace("/Engine/Resources/Fonts/JetbrainsMono/JetBrainsMono-ExtraBold.ttf", false);
        Rml::LoadFontFace("/Engine/Resources/Fonts/JetbrainsMono/JetBrainsMono-Bold.ttf", false);

        State.bInitialized = true;

        (void)FCoreDelegates::OnContentFileModified.AddStatic(&OnContentFileModified);

        LOG_INFO("[RmlUi] Initialized.");
        return true;
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

        // Widget contexts (owned by SWidgetComponent.Runtime) are torn down with their worlds
        // via the on_destroy hook; Rml::Shutdown drops any that outlive their world here.
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

        // Font faces are destroyed; the default-font bytes RmlUi referenced are now safe to release.
        State.DefaultFontData.clear();
        State.DefaultFontData.shrink_to_fit();

        State.EditorContexts.clear();
        State.ActiveWorld = nullptr;
        State.bInitialized = false;

        if (State.Renderer) State.Renderer->Shutdown();

        Rml::SetRenderInterface(nullptr);
        Rml::SetFileInterface(nullptr);
        Rml::SetSystemInterface(nullptr);

        // Clear fields individually -- StateMutex is non-movable.
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
        if (!State.bInitialized || World == nullptr)
        {
            return UI;
        }

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
            return UI;
        }

        UI->Context = Ctx;
        ApplyDefaultFontFamily(Ctx);

        // Newest world becomes the active UI target.
        State.ActiveWorld = World;
        SyncDebuggerToActiveContext();

        LOG_INFO("[RmlUi] Created context '{}' for world {} (initial {}x{}).", NameBuf, static_cast<void*>(World), InitialSize.x, InitialSize.y);
        return UI;
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

        // Backend already shut down (Rml::Shutdown destroyed every context): the pointer
        // is dangling, so just drop it without touching Rml.
        if (!State.bInitialized)
        {
            ReapWorldUIListeners(State, World);
            ReapWorldDataModels(State, World);
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
            // RmlUi tears down first (calls listener OnDetach), then we drop wrappers.
            Rml::RemoveContext(UI->Context->GetName());
            UI->Context = nullptr;
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
        if (!State.bInitialized || World == nullptr || World->GetUIContext() == nullptr)
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

        // Once per frame (flag self-clears): restyle all docs if a UI file changed on disk.
        ProcessPendingUIReload();

        FWorldUIContext* UI = World->GetUIContext();
        if (UI == nullptr || UI->Context == nullptr)
        {
            return;
        }

        const FWorldTarget Tgt = GetWorldTarget(World);
        if (RHI::IsValid(Tgt.Image))
        {
            // DisplaySize override (set by editor each frame) lets the UI lay out at the
            // panel's aspect instead of the RT's. Falls back to RT size in standalone.
            const FUIntVector2 LayoutSize = (UI->DisplaySize.x > 0 && UI->DisplaySize.y > 0) ? UI->DisplaySize : Tgt.Size;

            constexpr float NominalHeight = 1080.0f;
            UI->Context->SetDimensions(Rml::Vector2i(int(LayoutSize.x), int(LayoutSize.y)));

            const float DpRatio = std::max(1.0f, float(LayoutSize.y) / NominalHeight);
            UI->Context->SetDensityIndependentPixelRatio(DpRatio);
        }
        UI->Context->Update();
    }

    void RenderWorldUI(const CWorld* World, RHI::FCmdListH CmdList)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || State.Renderer == nullptr || World == nullptr)
        {
            return;
        }
        FWorldUIContext* UI = World->GetUIContext();
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
        FWorldUIContext* UI = World->GetUIContext();
        if (UI == nullptr)
        {
            return;
        }

        UI->WidgetJobs.clear();

        World->View<SWidgetComponent>().each([&](entt::entity Entity, SWidgetComponent& Comp)
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
                // On-disk edit: drop RmlUi's cached stylesheets/templates so the re-parse picks
                // up .rcss changes too, not just the .rml body.
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
                    return;   // settled: no Update, no job; keep last RT
                }
            }

            R.Context->SetDimensions(Rml::Vector2i(int(Width), int(Height)));
            R.Context->SetDensityIndependentPixelRatio(std::max(0.1f, float(Height) / 1080.0f));
            R.Context->Update();

            // RmlUi sets the next-update timeout to infinity at the start of Update and lowers it when
            // an animation/transition (incl. delayed) is pending. Huge => idle => dormancy-eligible.
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
        const FWorldUIContext* UI = World->GetUIContext();
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

            State.Renderer->BeginFrame(CmdList, Job.Target, Job.Size);
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

            // Renderer composites with LoadOp=Load, so clear the RT to transparent first.
            const float Transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            RHI::Barriers::AllToTransfer(CmdList);
            RHI::CmdClearTexture(CmdList, Job.Target, Transparent);
            RHI::Barriers::TransferToAll(CmdList);

            State.Renderer->EndFrame();
            State.Renderer->NoteTargetStable(Job.Target, false);      // just changed -> reset
            ++Rendered;
        }

        if (Rendered > 0)
        {
            // Widget RT writes visible to the scene's widget pass sampling them later this frame.
            RHI::CmdBarrier(CmdList, RHI::EStageFlags::RasterColorOut, RHI::EStageFlags::PixelShader);
        }

        State.WidgetRenderCursor = (uint32)((State.WidgetRenderCursor + 1) % JobCount);
    }

    void ReleaseWidget(CWorld* World, SWidgetComponent& Component)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        
        if (World != nullptr)
        {
            if (FWorldUIContext* UI = World->GetUIContext())
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
                E->Context->SetDensityIndependentPixelRatio(std::max(0.1f, E->DpiScale));
            }
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
            RHI::Barriers::AllToTransfer(CmdList);
            RHI::CmdClearTexture(CmdList, E->Target, Clear);
            RHI::Barriers::TransferToAll(CmdList);

            State.Renderer->BeginFrame(CmdList, E->Target, E->Size);
            E->Context->Render();
            State.Renderer->EndFrame();
            bAnyRendered = true;
        }

        if (bAnyRendered)
        {
            // Preview RT writes visible to ImGui sampling them this frame.
            RHI::CmdBarrier(CmdList, RHI::EStageFlags::RasterColorOut, RHI::EStageFlags::PixelShader);
        }
    }

    // Renderer pointer is set once in Initialise() and only cleared in Shutdown() -- safe to read unlocked.
    FRmlUiRenderer* GetRenderer()      { return S().Renderer.get(); }

    Rml::Context* GetContextForWorld(CWorld* World)
    {
        if (World == nullptr)
        {
            return nullptr;
        }
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        FWorldUIContext* UI = World->GetUIContext();
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
        const FWorldUIContext* UI = World->GetUIContext();
        return UI != nullptr && UI->Context != nullptr && UI->Context->IsMouseInteracting();
    }

    FLockedWorldContext::FLockedWorldContext(CWorld* World)
    {
        if (World == nullptr)
        {
            return;
        }
        FState& State = S();
        State.StateMutex.lock();
        bLocked = true;
        // Resolved INSIDE the locked scope so the Context* can't be torn down between
        // resolution and use; ~FLockedWorldContext releases.
        FWorldUIContext* UI = World->GetUIContext();
        Context = UI ? UI->Context : nullptr;
    }

    FLockedWorldContext::~FLockedWorldContext()
    {
        if (bLocked)
        {
            S().StateMutex.unlock();
        }
    }

    void SetWorldDisplaySize(CWorld* World, const FUIntVector2& Size)
    {
        if (World == nullptr)
        {
            return;
        }
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (FWorldUIContext* UI = World->GetUIContext())
        {
            UI->DisplaySize = Size;
        }
    }

    bool SetWorldInlineDocument(CWorld* World, FStringView Body, FStringView SourceUrl)
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
        FWorldUIContext* UI = World->GetUIContext();
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

            Rml::RemoveContext(E->Context->GetName());
            E->Context = nullptr;

            const size_t Last = State.EditorContexts.size() - 1;
            if (i != Last)
            {
                eastl::swap(State.EditorContexts[i], State.EditorContexts[Last]);
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

    bool ReplaceEditorContextDocument(Rml::Context* Context, FStringView Body, FStringView SourceUrl)
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

        Entry->Document = Entry->Context->LoadDocumentFromMemory(BodyStr, UrlStr);
        if (Entry->Document == nullptr)
        {
            return false;
        }
        Entry->Document->Show();
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

    //--------------------------------------------------------------------------------------------
    // Scripting surface implementation (World.UI). All take the bridge lock; handles are raw
    // Rml::ElementDocument* / Rml::Element*. Game thread only.
    //--------------------------------------------------------------------------------------------

    namespace
    {
        Rml::Element*         AsElement (void* P) { return static_cast<Rml::Element*>(P); }
        Rml::ElementDocument* AsDocument(void* P) { return static_cast<Rml::ElementDocument*>(P); }
        Rml::String           ToRml(FStringView V) { return Rml::String(V.data(), V.size()); }
    }

    void* LoadScreenDocument(CWorld* World, FStringView VirtualPath)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || World == nullptr || VirtualPath.empty())
        {
            return nullptr;
        }
        FWorldUIContext* UI = World->GetUIContext();
        if (UI == nullptr || UI->Context == nullptr)
        {
            return nullptr;
        }
        Rml::ElementDocument* Doc = UI->Context->LoadDocument(ToRml(VirtualPath));
        if (Doc == nullptr)
        {
            LOG_WARN("[RmlUi] World.UI.LoadDocument failed for '{}'.", FString(VirtualPath.data(), VirtualPath.size()).c_str());
        }
        return Doc;
    }

    void* LoadScreenDocumentFromMemory(CWorld* World, FStringView Body, FStringView SourceUrl)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || World == nullptr || Body.empty())
        {
            return nullptr;
        }
        FWorldUIContext* UI = World->GetUIContext();
        if (UI == nullptr || UI->Context == nullptr)
        {
            return nullptr;
        }
        return UI->Context->LoadDocumentFromMemory(ToRml(Body), ToRml(SourceUrl));
    }

    void UnloadScreenDocument(CWorld* World, void* Document)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || World == nullptr || Document == nullptr)
        {
            return;
        }
        FWorldUIContext* UI = World->GetUIContext();
        if (UI != nullptr && UI->Context != nullptr)
        {
            UI->Context->UnloadDocument(AsDocument(Document));
        }
    }

    void ShowDocument(void* Document, bool bModal, bool bAutoFocus)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Document != nullptr)
        {
            AsDocument(Document)->Show(bModal     ? Rml::ModalFlag::Modal : Rml::ModalFlag::None,
                                       bAutoFocus ? Rml::FocusFlag::Auto  : Rml::FocusFlag::None);
        }
    }

    void HideDocument(void* Document)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Document != nullptr)
        {
            AsDocument(Document)->Hide();
        }
    }

    void PullDocumentToFront(void* Document)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Document != nullptr)
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
        if (!State.bInitialized || Document == nullptr || Id.empty())
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

    void ElementSetInnerRml(void* Element, FStringView Rml)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Element != nullptr)
        {
            AsElement(Element)->SetInnerRML(ToRml(Rml));
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
            AsElement(Element)->SetAttribute(ToRml(Name), ToRml(Value));
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
            AsElement(Element)->SetProperty(ToRml(Name), ToRml(Value));
        }
    }

    void ElementRemoveProperty(void* Element, FStringView Name)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Element != nullptr && !Name.empty())
        {
            AsElement(Element)->RemoveProperty(ToRml(Name));
        }
    }

    void ElementSetClass(void* Element, FStringView Class, bool bActive)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Element != nullptr && !Class.empty())
        {
            AsElement(Element)->SetClass(ToRml(Class), bActive);
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
        }
    }

    void ElementBlur(void* Element)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Element != nullptr)
        {
            AsElement(Element)->Blur();
        }
    }

    void ElementClick(void* Element)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (State.bInitialized && Element != nullptr)
        {
            AsElement(Element)->Click();
        }
    }

    void* AddElementEventListener(CWorld* World, void* Element, FStringView EventType, FManagedUIEventThunk Thunk, void* Context)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || Element == nullptr || Thunk == nullptr || EventType.empty())
        {
            return nullptr;
        }
        FManagedUIListener* Listener = new FManagedUIListener(Thunk, Context, World, ToRml(EventType));
        AsElement(Element)->AddEventListener(Listener->Type, Listener, false);
        State.UIListeners.push_back(Listener);
        return Listener;
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

    //--------------------------------------------------------------------------------------------
    // Data binding (MVVM) implementation. Backs LuminaSharp's ViewModel / World.UI.AddModel.
    //--------------------------------------------------------------------------------------------

    void* CreateDataModel(CWorld* World, FStringView Name, void* Context, FManagedDataSetThunk SetThunk, FManagedDataEventThunk EventThunk)
    {
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (!State.bInitialized || World == nullptr || Name.empty())
        {
            return nullptr;
        }
        FWorldUIContext* UI = World->GetUIContext();
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

        // The field id == the slot this variable WILL occupy. Register the bound funcs first; only commit to
        // the cache vectors on success, so a failed bind never burns an id (keeps it dense for the C# side).
        const int32       Field = (int32)M->Values.size();
        const Rml::String VarName(Name.data(), Name.size());
        const bool        bString = ((EUIVarType)Type == EUIVarType::String);

        const bool bOk = M->Constructor.BindFunc(VarName,
            // Getter: hand RmlUi the cached value (no managed crossing on read).
            [M, Field](Rml::Variant& Out) { Out = M->Values[Field]; },
            // Setter: two-way controllers (data-value / data-checked) write here. Coerce the incoming value
            // (a form control hands back a string) to the registered type before caching, so the getter keeps
            // returning a consistently-typed Variant; then push the change to the managed property.
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
            [M, CommandId](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& Arguments)
            {
                if (M->EventThunk == nullptr)
                {
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
        FState& State = S();
        FRecursiveScopeLock Lock(State.StateMutex);
        if (ModelPtr == nullptr)
        {
            return;
        }
        // Validate against the live set rather than dereferencing blindly: if the world already tore down,
        // the model was reaped and this pointer is stale -- a disposed managed binding then no-ops safely.
        for (size_t i = 0; i < State.DataModels.size(); ++i)
        {
            FManagedDataModel* M = State.DataModels[i];
            if (M != ModelPtr)
            {
                continue;
            }
            if (State.bInitialized && M->World != nullptr)
            {
                if (FWorldUIContext* UI = M->World->GetUIContext())
                {
                    if (UI->Context != nullptr)
                    {
                        UI->Context->RemoveDataModel(M->Name);
                    }
                }
            }
            State.DataModels[i] = State.DataModels.back();
            State.DataModels.pop_back();
            delete M;
            return;
        }
    }

}
