#include "AgentCorePCH.h"
#include "Agent/AgentObjectEdit.h"

#include "Agent/AgentPropertyPath.h"
#include "Agent/AgentReflectionUtils.h"
#include "Agent/AgentToolMarshal.h"
#include "Containers/StringFormat.h"
#include "Core/Object/Package/Package.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Reflection/Type/Properties/ArrayProperty.h"
#include "Core/Reflection/Type/Properties/StructProperty.h"
#include "Session/SessionOps.h"

namespace Lumina::Agent
{
    namespace AgentObjectEditInternal
    {
        FString Dump(const FJsonValue& Value)
        {
            return Value.ToString();
        }

        void CollectUnwritable(CStruct* Struct, FStringView Prefix, int32 Depth, TVector<FString>& OutPaths)
        {
            if (Struct == nullptr || Depth > 8)
            {
                return;
            }

            for (CStruct* Current : Detail::CollectStructChain(Struct))
            {
                Current->ForEachProperty<FProperty>([&](FProperty* Property)
                {
                    if (Property == nullptr)
                    {
                        return;
                    }

                    const FString Name(*Property->GetPropertyName().ToString());
                    const FString Path = Prefix.IsEmpty() ? Name : Lumina::Format("{}.{}", Prefix, Name);

                    switch (Property->GetType())
                    {
                    case EPropertyTypeFlags::Delegate:
                        OutPaths.Add(Path);
                        break;

                    case EPropertyTypeFlags::Struct:
                        CollectUnwritable(static_cast<FStructProperty*>(Property)->GetStruct(), FStringView(Path), Depth + 1, OutPaths);
                        break;

                    case EPropertyTypeFlags::Vector:
                        if (FProperty* Inner = static_cast<FArrayProperty*>(Property)->GetInternalProperty();
                            Inner != nullptr && Inner->GetType() == EPropertyTypeFlags::Struct)
                        {
                            CollectUnwritable(static_cast<FStructProperty*>(Inner)->GetStruct(),
                                FStringView(Lumina::Format("{}[]", Path)), Depth + 1, OutPaths);
                        }
                        break;

                    default:
                        break;
                    }
                });
            }
        }
    }

    using namespace AgentObjectEditInternal;

    FObjectEditResult SetObjectProperty(CStruct* Root, void* RootData, CObject* Owner, FStringView Path,
        const FJsonValue& Value, FName Label)
    {
        FObjectEditResult Result;

        FResolvedProperty Target;
        if (!ResolvePropertyPath(Root, RootData, Path, Target, Result.Error))
        {
            return Result;
        }

        // Held to the details panel's rule, since a read-only field such as a prefab's parent is derived and editing it breaks what it derives from.
        if (Target.Property->IsReadOnly())
        {
            Result.Error = Lumina::Format("'{}' is read-only, so it cannot be set directly.", Path);
            return Result;
        }

        // Checked before anything is recorded, so a bad value costs no snapshot.
        if (const FMarshalResult Checked = ValidatePropertyValue(Value, Target.Property, Path); !Checked.IsValid())
        {
            Result.Error = Checked.Error;
            return Result;
        }

        FJsonValue Before;
        if (WriteProperty(Target.Property, Target.ValuePtr, Before).IsValid())
        {
            Result.Previous = Dump(Before);
        }

        FMarshalResult Applied;
        const auto Mutate = [&]()
        {
            Applied = ReadProperty(Value, Target.Property, Target.ValuePtr, Path);
            if (Applied.IsValid() && Owner != nullptr)
            {
                Owner->PostPropertyChange(Target.Property);
            }
        };

        // Undoable only when an open editor owns it, so an edit to an unopened asset still applies.
        Result.bUndoable = SessionOps::RunObjectTransacted(Owner, Label, Mutate, Target.Property);
        if (!Result.bUndoable)
        {
            Mutate();
        }

        if (!Applied.IsValid())
        {
            Result.Error = Applied.Error;
            return Result;
        }

        // A default object lives in a native script package and persists through its config file instead.
        if (Owner != nullptr && Owner->GetPackage() != nullptr && !Owner->HasAnyFlag(OF_DefaultObject))
        {
            Owner->GetPackage()->MarkDirty();
        }

        FJsonValue After;
        if (WriteProperty(Target.Property, Target.ValuePtr, After).IsValid())
        {
            Result.Current = Dump(After);
        }

        return Result;
    }

    void CollectUnwritableFields(CStruct* Root, FStringView Prefix, TVector<FString>& OutPaths)
    {
        CollectUnwritable(Root, Prefix, 0, OutPaths);
    }
}
