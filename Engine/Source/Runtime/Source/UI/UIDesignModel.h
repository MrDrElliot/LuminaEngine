#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "UI/UITypes.h"

namespace Lumina
{
    // One bound value as the editor previews it, with the default the C# model would start from.
    struct FUIDesignScalar
    {
        FString    Name;
        EUIVarType Type = EUIVarType::String;
        FString    Value;
        bool       bWritable = false;
    };

    // A data-for collection, as named members and rows of their values.
    struct FUIDesignList
    {
        FString                   Name;
        TVector<FString>          Members;
        TVector<TVector<FString>> Rows;
    };

    // A data-event command and the names of the arguments it takes.
    struct FUIDesignCommand
    {
        FString          Name;
        TVector<FString> Params;
    };

    // A data model the editor preview binds in place of the running game's, described by its C# type or inferred from the markup.
    struct FUIDesignModel
    {
        FString                   Name;
        FString                   SourceType;
        FString                   DocumentPath;
        bool                      bFromScript = false;
        TVector<FUIDesignScalar>  Scalars;
        TVector<FUIDesignList>    Lists;
        TVector<FUIDesignCommand> Commands;

        const FUIDesignScalar* FindScalar(FStringView InName) const
        {
            for (const FUIDesignScalar& Scalar : Scalars)
            {
                if (FStringView(Scalar.Name.c_str(), Scalar.Name.size()) == InName)
                {
                    return &Scalar;
                }
            }
            return nullptr;
        }

        bool HasMember(FStringView InName) const
        {
            if (FindScalar(InName) != nullptr)
            {
                return true;
            }
            // A bound struct is described member by member, as Player.Name and Player.Level.
            for (const FUIDesignScalar& Scalar : Scalars)
            {
                const FStringView Path(Scalar.Name.c_str(), Scalar.Name.size());
                if (Path.size() > InName.size() && Path[InName.size()] == '.' && Path.substr(0, InName.size()) == InName)
                {
                    return true;
                }
            }
            for (const FUIDesignList& List : Lists)
            {
                if (FStringView(List.Name.c_str(), List.Name.size()) == InName)
                {
                    return true;
                }
            }
            return false;
        }

        bool HasCommand(FStringView InName) const
        {
            for (const FUIDesignCommand& Command : Commands)
            {
                if (FStringView(Command.Name.c_str(), Command.Name.size()) == InName)
                {
                    return true;
                }
            }
            return false;
        }
    };

    // A C# model type the editor can offer, by the name a data-model attribute uses.
    struct FUIModelInfo
    {
        FString Name;
        FString TypeName;
        FString DocumentPath;
        bool    bScript = false;
    };
}
