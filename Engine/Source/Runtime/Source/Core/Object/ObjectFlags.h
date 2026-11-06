#pragma once
#include "Containers/String.h"
#include "Core/LuminaMacros.h"


namespace Lumina
{
    enum EObjectFlags
    {
        OF_None                 = 0,
        // Not saved.
        OF_Transient            = BIT(0),
        OF_Rooted               = BIT(1),
        OF_DefaultObject        = BIT(2),
        OF_NeedsLoad            = BIT(3),
        OF_Loading              = BIT(4),
        // PostLoad still owed, without deserializing again.
        OF_NeedsPostLoad        = BIT(5),
        OF_WasLoaded            = BIT(6),
        // Visible outside its package, as assets are.
        OF_Public               = BIT(7),
        OF_MarkedDestroy        = BIT(8),
        // Set only when the class appended storage-owning script properties, so native destructors never consult the class.
        OF_ScriptProperties     = BIT(9),
        // OnDestroy has run, unlike OF_MarkedDestroy, which the shutdown sweep sets a phase early.
        OF_DestroyStarted       = BIT(10),
    };

    ENUM_CLASS_FLAGS(EObjectFlags);

    inline FFixedString ObjectFlagsToString(EObjectFlags Flags)
    {
        FFixedString Result;

        if (Flags == OF_None)
        {
            return "None";
        }

        if (EnumHasAnyFlags(Flags, OF_Transient))       Result += "OF_Transient|";
        if (EnumHasAnyFlags(Flags, OF_Rooted))          Result += "OF_Rooted|";
        if (EnumHasAnyFlags(Flags, OF_DefaultObject))   Result += "OF_DefaultObject|";
        if (EnumHasAnyFlags(Flags, OF_NeedsLoad))       Result += "OF_NeedsLoad|";
        if (EnumHasAnyFlags(Flags, OF_Loading))         Result += "OF_Loading|";
        if (EnumHasAnyFlags(Flags, OF_NeedsPostLoad))   Result += "OF_NeedsPostLoad|";
        if (EnumHasAnyFlags(Flags, OF_WasLoaded))       Result += "OF_WasLoaded|";
        if (EnumHasAnyFlags(Flags, OF_Public))          Result += "OF_Public|";
        if (EnumHasAnyFlags(Flags, OF_MarkedDestroy))   Result += "OF_MarkedDestroy|";
        if (EnumHasAnyFlags(Flags, OF_ScriptProperties))Result += "OF_ScriptProperties|";
        if (EnumHasAnyFlags(Flags, OF_DestroyStarted))  Result += "OF_DestroyStarted|";

        if (!Result.IsEmpty() && Result.Last() == '|') Result.LeftChopInline(1);

        return Result;
    }
}
