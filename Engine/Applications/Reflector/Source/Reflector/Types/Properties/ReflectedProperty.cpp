#include "ReflectedProperty.h"

#include "Reflector/ReflectionSpecifiers.h"
#include "Reflector/Public/PropertyFlags.h"
#include "Reflector/Utils/MetadataUtils.h"

namespace Lumina
{
    void FReflectedProperty::GenerateMetadata(const std::string& InMetadata)
    {
        if (InMetadata.empty())
        {
            return;
        }

        FMetadataParser Parser(InMetadata);
        Metadata = std::move(Parser.Metadata);

        for (const FMetadataPair& MetadataPair : Metadata)
        {
            PropertyFlags |= Reflection::PropertyFlagForSpecifier(MetadataPair.Key);

            if (MetadataPair.Key == "Getter")
            {
                GetterFunc = MetadataPair.Value.empty() ? ("Get" + Name) : MetadataPair.Value;
            }
            else if (MetadataPair.Key == "Setter")
            {
                SetterFunc = MetadataPair.Value.empty() ? ("Set" + Name) : MetadataPair.Value;
            }
        }
    }

    bool FReflectedProperty::FindConflictingSpecifiers(std::string& OutMessage) const
    {
        struct FConflict
        {
            EPropertyFlags A;
            EPropertyFlags B;
            const char*    NameA;
            const char*    NameB;
            const char*    Resolution;
        };

        // Each says what to write instead, since the useful part is which specifier to delete.
        static const FConflict Conflicts[] =
        {
            { EPropertyFlags::Editable, EPropertyFlags::ReadOnly, "Editable", "ReadOnly",
              "ReadOnly already shows the property in the details panel with editing disabled; drop Editable." },

            { EPropertyFlags::ScriptReadOnly, EPropertyFlags::ScriptWritable, "ScriptReadOnly", "ScriptWritable",
              "A script property is either readable-only or writable; keep one." },

            { EPropertyFlags::ScriptHidden, EPropertyFlags::ScriptReadOnly, "ScriptHidden", "ScriptReadOnly",
              "ScriptHidden removes the property from script entirely, so an access specifier cannot apply." },

            { EPropertyFlags::ScriptHidden, EPropertyFlags::ScriptWritable, "ScriptHidden", "ScriptWritable",
              "ScriptHidden removes the property from script entirely, so an access specifier cannot apply." },
        };

        for (const FConflict& Conflict : Conflicts)
        {
            if (EnumHasAllFlags(PropertyFlags, Conflict.A | Conflict.B))
            {
                OutMessage = std::string("PROPERTY '") + Name + "' declares both '" + Conflict.NameA
                           + "' and '" + Conflict.NameB + "', which conflict. " + Conflict.Resolution;
                return true;
            }
        }

        if (HasSpecifier("Hidden") && EnumHasAnyFlags(PropertyFlags, EPropertyFlags::Editable | EPropertyFlags::ReadOnly))
        {
            OutMessage = std::string("PROPERTY '") + Name + "' declares 'Hidden' with '"
                       + (EnumHasAnyFlags(PropertyFlags, EPropertyFlags::Editable) ? "Editable" : "ReadOnly")
                       + "', which conflict. Hidden keeps it out of the details panel; drop one.";
            return true;
        }

        return false;
    }

    void FReflectedProperty::ApplyDefaultVisibility()
    {
        if (!EnumHasAnyFlags(PropertyFlags, EPropertyFlags::ReadOnly) && !HasSpecifier("Hidden"))
        {
            PropertyFlags |= EPropertyFlags::Editable;
        }
    }

    bool FReflectedProperty::HasSpecifier(std::string_view Key) const
    {
        for (const FMetadataPair& Pair : Metadata)
        {
            if (Pair.Key == Key)
            {
                return true;
            }
        }
        return false;
    }
}
