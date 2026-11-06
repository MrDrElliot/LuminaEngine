#include "UIScript.h"

#include <algorithm>
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIteration.h"
#include "Core/Object/ScriptClass.h"
#include "Core/Reflection/PropertyText.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Log/Log.h"
#include "RmlUiBridge.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Scripting/DotNet/DotNetUI.h"
#include "World/World.h"

namespace Lumina
{
    void CUIScript::Show()
    {
        UIScripts::Show(this);
    }

    void CUIScript::Hide()
    {
        UIScripts::Hide(this);
    }

    void CUIScript::Toggle()
    {
        if (IsShown())
        {
            Hide();
        }
        else
        {
            Show();
        }
    }

    bool CUIScript::IsShown() const
    {
        return UIScripts::IsShown(this);
    }

    SUIDocument CUIScript::GetDocument() const
    {
        SUIDocument Result;
        Result.Handle = reinterpret_cast<uint64>(UIScripts::GetDocument(this));
        return Result;
    }
}

namespace Lumina::UIBinding
{
    namespace
    {
        FString MetaInChain(const CClass* Class, FStringView Key)
        {
            const FName KeyName(*FString(Key.GetData(), Key.Len()));
            for (const CClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
            {
                if (Current->HasMeta(KeyName))
                {
                    return Current->GetMeta(KeyName);
                }
            }
            return FString();
        }

        bool BindsAnything(const CClass* Class)
        {
            TVector<FBoundValue> Values;
            GetBoundValues(Class, Values);
            if (!Values.IsEmpty())
            {
                return true;
            }
            TVector<FBoundFunction> Functions;
            GetBoundFunctions(Class, Functions);
            return !Functions.IsEmpty();
        }

        template<typename TVisitor>
        void ForEachClass(TVisitor&& Visitor)
        {
            TVector<CClass*> Classes;
            ForEachObject([&Classes](CObjectBase* Object, int32)
            {
                if (Object != nullptr && Object->IsA<CClass>())
                {
                    Classes.Add(static_cast<CClass*>(Object));
                }
            });
            for (CClass* Class : Classes)
            {
                Visitor(Class);
            }
        }

        EUIVarType DesignTypeOf(const FProperty* Property)
        {
            switch (Property->GetType())
            {
                case EPropertyTypeFlags::Bool:   return EUIVarType::Bool;
                case EPropertyTypeFlags::Float:  return EUIVarType::Float;
                case EPropertyTypeFlags::Double: return EUIVarType::Double;
                case EPropertyTypeFlags::Int8:   case EPropertyTypeFlags::Int16:  case EPropertyTypeFlags::Int32:
                case EPropertyTypeFlags::Int64:  case EPropertyTypeFlags::UInt8:  case EPropertyTypeFlags::UInt16:
                case EPropertyTypeFlags::UInt32: case EPropertyTypeFlags::UInt64: case EPropertyTypeFlags::Enum:
                case EPropertyTypeFlags::Entity:
                    return EUIVarType::Int;
                default:
                    return EUIVarType::String;
            }
        }
    }

    void GetBoundValues(const CStruct* Type, TVector<FBoundValue>& Out)
    {
        Out.Reset();
        if (Type == nullptr)
        {
            return;
        }
        for (FProperty* Property : Type->GetProperties())
        {
            if (!Property->HasMetadata("Bind"))
            {
                continue;
            }
            const FCStringView Renamed = Property->GetMetadata("Bind");
            FBoundValue& Value = Out.EmplaceGetRef();
            Value.Name = Renamed.IsEmpty() ? FString(*Property->GetPropertyName()) : FString(Renamed.GetData(), Renamed.Len());
            Value.Property = Property;
        }
    }

    void GetBoundFunctions(const CClass* Class, TVector<FBoundFunction>& Out)
    {
        Out.Reset();
        if (Class == nullptr)
        {
            return;
        }
        for (const FFunction* Function : Class->GetFunctions())
        {
            if (!EnumHasAnyFlags(Function->GetFunctionFlags(), EFunctionFlags::UIBind))
            {
                continue;
            }

            // A C# get-only property arrives as its getter, which RML names by the property.
            FString Name(*Function->GetFunctionName());
            if (Name.FindLast("get_", 0) == 0)
            {
                Name = Name.Mid(4);
            }
            const bool bTaken = Algo::AnyOf(Out, [&Name](const FBoundFunction& Existing) { return Existing.Name == Name; });
            if (bTaken)
            {
                continue;
            }

            FBoundFunction& Bound = Out.EmplaceGetRef();
            Bound.Name = Name;
            Bound.Function = Function;
            Bound.bComputed = Function->GetArguments().IsEmpty() && Function->GetReturnParam() != nullptr;
        }
    }

    FString ModelName(const CClass* Class)
    {
        if (Class == nullptr)
        {
            return FString();
        }
        FString Declared = MetaInChain(Class, "DataModel");
        if (!Declared.IsEmpty())
        {
            return Declared;
        }

        FString Name(*Class->GetName());
        const int32 Dot = Name.FindLast('.');
        if (Dot != Constants::kIndexNone)
        {
            return Name.Mid(Dot + 1);
        }
        if (Name.Len() > 1 && Name[0] == 'C' && std::isupper((unsigned char)Name[1]))
        {
            return Name.Mid(1);
        }
        return Name;
    }

    CClass* FindModelClass(FStringView Name)
    {
        CClass* Found = nullptr;
        ForEachClass([&](CClass* Class)
        {
            if (Found == nullptr && FStringView(*ModelName(Class)) == Name && BindsAnything(Class))
            {
                Found = Class;
            }
        });
        return Found;
    }

    void EnumerateModelClasses(TVector<FUIModelInfo>& Out)
    {
        Out.Reset();
        ForEachClass([&Out](CClass* Class)
        {
            if (!BindsAnything(Class))
            {
                return;
            }
            FUIModelInfo& Info = Out.EmplaceGetRef();
            Info.Name = ModelName(Class);
            Info.TypeName = FString(*Class->GetName());
            Info.DocumentPath = MetaInChain(Class, "UIDocument");
            Info.bScript = UIScripts::IsUIScript(Class);
        });
    }

    const FProperty* ResolvePath(CObject* Object, FStringView Path, void*& OutContainer)
    {
        OutContainer = nullptr;
        if (Object == nullptr || Path.IsEmpty())
        {
            return nullptr;
        }

        const int32 RootEnd = Path.Find('.');
        const FStringView Root = RootEnd == Constants::kIndexNone ? Path : Path.Left(RootEnd);
        TVector<FBoundValue> Values;
        GetBoundValues(Object->GetClass(), Values);
        const FProperty* Property = nullptr;
        for (const FBoundValue& Value : Values)
        {
            if (FStringView(*Value.Name, Value.Name.Len()) == Root)
            {
                Property = Value.Property;
            }
        }
        void* Container = Object;

        FStringView Rest = RootEnd == Constants::kIndexNone ? FStringView() : Path.Mid(RootEnd + 1);
        while (Property != nullptr && !Rest.IsEmpty())
        {
            if (Property->GetType() != EPropertyTypeFlags::Struct)
            {
                return nullptr;
            }
            Container = const_cast<FProperty*>(Property)->GetValuePtr<void>(Container);
            const int32 SegmentEnd = Rest.Find('.');
            const FStringView Segment = SegmentEnd == Constants::kIndexNone ? Rest : Rest.Left(SegmentEnd);
            const FProperty* Member = nullptr;
            for (const FProperty* Candidate : static_cast<const FStructProperty*>(Property)->GetStruct()->GetProperties())
            {
                if (FStringView(*Candidate->GetPropertyName()) == Segment)
                {
                    Member = Candidate;
                }
            }
            Property = Member;
            Rest = SegmentEnd == Constants::kIndexNone ? FStringView() : Rest.Mid(SegmentEnd + 1);
        }

        OutContainer = Property != nullptr ? Container : nullptr;
        return Property;
    }

    void Describe(const CObject* Object, FUIDesignModel& Out)
    {
        Out = FUIDesignModel();
        if (Object == nullptr)
        {
            return;
        }
        const CClass* Class = Object->GetClass();
        Out.Name = ModelName(Class);
        Out.SourceType = FString(*Class->GetName());
        Out.DocumentPath = MetaInChain(Class, "UIDocument");
        Out.bFromScript = UIScripts::IsUIScript(Class);

        TVector<FBoundValue> Values;
        GetBoundValues(Class, Values);
        for (const FBoundValue& Value : Values)
        {
            if (Value.Property->GetType() == EPropertyTypeFlags::Vector)
            {
                const FArrayProperty* Array = static_cast<const FArrayProperty*>(Value.Property);
                const FProperty* Inner = Array->GetInternalProperty();
                FUIDesignList& List = Out.Lists.EmplaceGetRef();
                List.Name = Value.Name;

                const CStruct* ItemType = Inner->GetType() == EPropertyTypeFlags::Struct ? static_cast<const FStructProperty*>(Inner)->GetStruct() : nullptr;
                if (ItemType != nullptr)
                {
                    for (const FProperty* Member : ItemType->GetProperties())
                    {
                        List.Members.Emplace(*Member->GetPropertyName());
                    }
                }

                void* Items = const_cast<FProperty*>(Value.Property)->GetValuePtr<void>(const_cast<CObject*>(Object));
                const int32 Count = Math::Min(Array->Num(Items), 64);
                for (int32 Row = 0; Row < Count; ++Row)
                {
                    const void* Item = Array->GetAt(Items, Row);
                    TVector<FString>& Cells = List.Rows.EmplaceGetRef();
                    if (ItemType != nullptr)
                    {
                        for (const FProperty* Member : ItemType->GetProperties())
                        {
                            Cells.Add(Reflection::ToText(Member, Item));
                        }
                    }
                    else
                    {
                        Cells.Add(Reflection::ToText(Inner, Item));
                    }
                }
                continue;
            }

            if (Value.Property->GetType() == EPropertyTypeFlags::Struct)
            {
                const void* Fields = const_cast<FProperty*>(Value.Property)->GetValuePtr<void>(const_cast<CObject*>(Object));
                for (const FProperty* Member : static_cast<const FStructProperty*>(Value.Property)->GetStruct()->GetProperties())
                {
                    if (Reflection::IsTextConvertible(Member))
                    {
                        FUIDesignScalar& Scalar = Out.Scalars.EmplaceGetRef();
                        Scalar.Name = Value.Name + "." + *Member->GetPropertyName();
                        Scalar.Type = DesignTypeOf(Member);
                        Scalar.Value = Reflection::ToText(Member, Fields);
                        Scalar.bWritable = true;
                    }
                }
                continue;
            }

            FUIDesignScalar& Scalar = Out.Scalars.EmplaceGetRef();
            Scalar.Name = Value.Name;
            Scalar.Type = DesignTypeOf(Value.Property);
            Scalar.Value = Reflection::ToText(Value.Property, Object);
            Scalar.bWritable = true;
        }

        TVector<FBoundFunction> Functions;
        GetBoundFunctions(Class, Functions);
        for (const FBoundFunction& Bound : Functions)
        {
            if (Bound.bComputed)
            {
                FUIDesignScalar& Scalar = Out.Scalars.EmplaceGetRef();
                Scalar.Name = Bound.Name;
                Scalar.Type = DesignTypeOf(Bound.Function->GetReturnParam());
                Scalar.Value = Bound.Name;
                continue;
            }
            FUIDesignCommand& Command = Out.Commands.EmplaceGetRef();
            Command.Name = Bound.Name;
            for (const FProperty* Param : Bound.Function->GetArguments())
            {
                Command.Params.Emplace(*Param->GetPropertyName());
            }
        }
    }
}

namespace Lumina::UIScripts
{
    namespace
    {
        struct FOpenUI
        {
            CEntityScript* Script = nullptr;
            CWorld*        World = nullptr;
            void*          Model = nullptr;
            void*          Document = nullptr;
            FString        Path;
            FString        ModelName;
            // What the script asked for, kept across a reload that fails, so fixing the file brings the UI back.
            bool           bWantShown = false;
            // The input mode lease this UI holds, so it is released even if Interactive changed since.
            uint64         CursorLease = 0;
            int32          Generation = -1;
        };

        TVector<FOpenUI>& OpenUIs()
        {
            static TVector<FOpenUI> UIs;
            return UIs;
        }

        FOpenUI* Find(const CEntityScript* Script)
        {
            for (FOpenUI& Open : OpenUIs())
            {
                if (Open.Script == Script)
                {
                    return &Open;
                }
            }
            return nullptr;
        }

        int32 CurrentGeneration()
        {
            return DotNet::IsInitialized() ? DotNet::GetScriptGeneration() : -1;
        }

        FString DocumentPathOf(const CEntityScript* Script)
        {
            const CClass* Class = Script->GetClass();
            if (const CUIScript* UIScript = Cast<CUIScript>(Script))
            {
                const FStringView Path = UIScript->Document.ResolvePath();
                if (!Path.IsEmpty())
                {
                    return FString(Path.GetData(), Path.Len());
                }
            }

            for (const CClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
            {
                if (Current->HasMeta("UIDocument"))
                {
                    return Current->GetMeta("UIDocument");
                }
            }
            return FString();
        }

        void ResolveElements(FOpenUI& Open)
        {
            for (FProperty* Property : Open.Script->GetClass()->GetProperties())
            {
                if (!Property->HasMetadata("Element") || Property->GetType() != EPropertyTypeFlags::Struct
                    || static_cast<const FStructProperty*>(Property)->GetStruct() != SUIElement::StaticStruct())
                {
                    continue;
                }
                const FCStringView Declared = Property->GetMetadata("Element");
                const FString Id = Declared.IsEmpty() ? FString(*Property->GetPropertyName()) : FString(Declared.GetData(), Declared.Len());
                void* Element = RmlUi::DocumentGetElementById(Open.Document, FStringView(*Id, Id.Len()));
                if (Element == nullptr)
                {
                    LOG_WARN("[UI] {}.{} found no element with id '{}' in '{}'.", *Open.Script->GetClass()->GetName(),
                        *Property->GetPropertyName(), *Id, *Open.Path);
                }
                Property->GetValuePtr<SUIElement>(Open.Script)->Handle = reinterpret_cast<uint64>(Element);
            }
        }

        // The callbacks may open another UI and move the list, so nothing reads Open after they start.
        void NotifyLoaded(FOpenUI& Open)
        {
            ResolveElements(Open);
            CEntityScript* Script = Open.Script;
            if (CUIScript* UIScript = Cast<CUIScript>(Script))
            {
                UIScript->OnDocumentLoaded();
            }
        }

        bool LoadDocument(FOpenUI& Open)
        {
            if (Open.Path.IsEmpty())
            {
                LOG_WARN("[UI] {} has no document. Set Document on the entity, or give the class a UIDocument.", *Open.Script->GetClass()->GetName());
                return false;
            }
            const FString Declared = UIBinding::ModelName(Open.Script->GetClass());
            Open.Document = RmlUi::LoadScreenDocumentWithModel(Open.World, FStringView(*Open.Path, Open.Path.Len()),
                FStringView(*Declared, Declared.Len()), FStringView(*Open.ModelName, Open.ModelName.Len()));
            if (Open.Document == nullptr)
            {
                LOG_ERROR("[UI] {} could not load '{}'.", *Open.Script->GetClass()->GetName(), *Open.Path);
                return false;
            }
            return true;
        }

        void CreateModel(FOpenUI& Open)
        {
            Rml::Context* Context = RmlUi::GetContextForWorld(Open.World);
            const FString Declared = UIBinding::ModelName(Open.Script->GetClass());

            // A second instance of the class binds under its own name, and its copy of the markup is renamed to match.
            Open.ModelName = Declared;
            for (int32 Suffix = 2; Context != nullptr && RmlUi::HasDataModel(Context, FStringView(*Open.ModelName, Open.ModelName.Len())); ++Suffix)
            {
                Open.ModelName = Declared + "_" + std::to_string(Suffix).c_str();
            }

            TVector<UIBinding::FBoundValue> Values;
            TVector<UIBinding::FBoundFunction> Functions;
            UIBinding::GetBoundValues(Open.Script->GetClass(), Values);
            UIBinding::GetBoundFunctions(Open.Script->GetClass(), Functions);
            Open.Model = (Context != nullptr && (!Values.IsEmpty() || !Functions.IsEmpty()))
                ? RmlUi::CreateObjectModel(Context, FStringView(*Open.ModelName, Open.ModelName.Len()), Open.Script, false) : nullptr;
            Open.Generation = CurrentGeneration();
        }

        void ReleaseHeldCursor(FOpenUI& Open)
        {
            if (Open.CursorLease != 0)
            {
                Open.World->GetInput()->ReleaseInputMode(Open.CursorLease);
                Open.CursorLease = 0;
            }
        }

        // Puts the document on screen when the script wants it shown and there is one to show.
        void Present(FOpenUI& Open)
        {
            if (!Open.bWantShown || Open.Document == nullptr)
            {
                return;
            }
            const CUIScript* UIScript = Cast<CUIScript>(Open.Script);
            const bool bInteractive = UIScript != nullptr && UIScript->bInteractive;
            RmlUi::ShowDocument(Open.Document, false, bInteractive);
            if (bInteractive && Open.CursorLease == 0 && Open.World != nullptr)
            {
                Open.CursorLease = Open.World->GetInput()->AcquireInputMode(EInputMode::GameAndUI, EMouseMode::Normal);
            }
        }

        // Loads the markup again, and the model too when the class was reminted under it, keeping whether it was shown.
        void Reopen(FOpenUI& Open, bool bRebindModel)
        {
            if (Open.Document != nullptr)
            {
                RmlUi::UnloadScreenDocument(Open.World, Open.Document);
                Open.Document = nullptr;
            }
            if (bRebindModel)
            {
                RmlUi::DestroyObjectModel(Open.Model);
                CreateModel(Open);
                Open.Path = DocumentPathOf(Open.Script);
            }
            if (!LoadDocument(Open))
            {
                ReleaseHeldCursor(Open);
                return;
            }
            Present(Open);
            NotifyLoaded(Open);
        }
    }

    bool IsUIScript(const CClass* Class)
    {
        return Class != nullptr && Class->IsChildOf(CUIScript::StaticClass());
    }

    void Open(CEntityScript* Script)
    {
        if (Script == nullptr || !IsUIScript(Script->GetClass()) || Find(Script) != nullptr)
        {
            return;
        }
        CWorld* World = Script->GetWorld();
        if (World == nullptr || !World->HasPresentation())
        {
            return;
        }

        // Registered even without a document, so fixing the path, the file or the class later brings the UI up.
        FOpenUI Open;
        Open.Script = Script;
        Open.World = World;
        Open.Path = DocumentPathOf(Script);
        const CUIScript* UIScript = Cast<CUIScript>(Script);
        Open.bWantShown = UIScript == nullptr || UIScript->bShowOnStart;

        // The model exists before the load, since RmlUi resolves bindings while it parses.
        CreateModel(Open);
        const bool bLoaded = LoadDocument(Open);
        OpenUIs().Add(Open);
        if (bLoaded)
        {
            Present(*Find(Script));
            NotifyLoaded(*Find(Script));
        }
    }

    void Close(CEntityScript* Script)
    {
        TVector<FOpenUI>& UIs = OpenUIs();
        for (int32 Index = 0; Index < UIs.Num(); ++Index)
        {
            FOpenUI& Open = UIs[Index];
            if (Open.Script != Script)
            {
                continue;
            }
            ReleaseHeldCursor(Open);
            RmlUi::UnloadScreenDocument(Open.World, Open.Document);
            RmlUi::DestroyObjectModel(Open.Model);
            UIs.RemoveAt((int64)Index);
            return;
        }
    }

    void Forget(CEntityScript* Script)
    {
        Close(Script);
    }

    void Replace(CEntityScript* Old, CEntityScript* New)
    {
        FOpenUI* Open = Find(Old);
        if (Open == nullptr || New == nullptr)
        {
            return;
        }
        // The model reads Old's memory, which goes when the reinstance finishes, so it cannot wait for the rebind.
        RmlUi::DestroyObjectModel(Open->Model);
        Open->Model = nullptr;
        Open->Script = New;
        Open->Generation = -2;
    }

    void Show(CEntityScript* Script)
    {
        FOpenUI* Open = Find(Script);
        if (Open == nullptr || Open->bWantShown)
        {
            return;
        }
        Open->bWantShown = true;
        Present(*Open);
    }

    void Hide(CEntityScript* Script)
    {
        FOpenUI* Open = Find(Script);
        if (Open == nullptr || !Open->bWantShown)
        {
            return;
        }
        Open->bWantShown = false;
        if (Open->Document != nullptr)
        {
            RmlUi::HideDocument(Open->Document);
        }
        ReleaseHeldCursor(*Open);
    }

    bool IsShown(const CEntityScript* Script)
    {
        const FOpenUI* Open = Find(Script);
        return Open != nullptr && Open->bWantShown && Open->Document != nullptr;
    }

    void* GetDocument(const CEntityScript* Script)
    {
        const FOpenUI* Open = Find(Script);
        return Open != nullptr ? Open->Document : nullptr;
    }

    void Tick(CWorld* World)
    {
        const int32 Generation = CurrentGeneration();
        for (int32 Index = 0; Index < OpenUIs().Num(); ++Index)
        {
            // A script reload remints the class and its properties, so the model would point at freed ones.
            FOpenUI& Open = OpenUIs()[Index];
            if (Open.World != World)
            {
                continue;
            }
            if (Open.Generation != Generation && ToScriptClass(Open.Script->GetClass()) != nullptr)
            {
                Reopen(Open, true);
                continue;
            }
            // The Document property can be edited while playing, from the inspector or by the script itself.
            FString Path = DocumentPathOf(Open.Script);
            if (Path != Open.Path)
            {
                Open.Path = Move(Path);
                Reopen(Open, false);
            }
        }
    }

    void DocumentChanged(FStringView Path)
    {
        for (int32 Index = 0; Index < OpenUIs().Num(); ++Index)
        {
            FOpenUI& Open = OpenUIs()[Index];
            if (Open.Path.Len() == Path.Len() && std::equal(Path.begin(), Path.end(), Open.Path.begin(),
                [](char A, char B) { return std::tolower((unsigned char)A) == std::tolower((unsigned char)B); }))
            {
                Reopen(Open, false);
            }
        }
    }

    void ForgetWorld(CWorld* World)
    {
        TVector<FOpenUI>& UIs = OpenUIs();
        UIs.RemoveAll([World](const FOpenUI& Open) { return Open.World == World; });
    }
}
