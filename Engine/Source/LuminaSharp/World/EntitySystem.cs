using System;
using System.Collections.Generic;
using System.Reflection;
using LuminaSharp;

namespace Lumina;

// One stage scheduled worker over a world's component store, instanced once per world. Its TQuery, TEventReader and TEventWriter fields are its declaration.
[DispatchWhen(nameof(Configure), nameof(HoldsSystemParams))]
public partial class CEntitySystem
{
    // Held here as well as in the fields, since native keeps pointing at each member's block for as long as the system lives.
    private readonly List<SystemParam> BoundParams = new();

    // Runs after system T and anything deriving from it, in every stage both tick in.
    protected void RunsAfter<T>() where T : CEntitySystem => RunsAfterSystem(NativeTypeName.Of<T>());

    // Runs before system T and anything deriving from it, in every stage both tick in.
    protected void RunsBefore<T>() where T : CEntitySystem => RunsBeforeSystem(NativeTypeName.Of<T>());

    partial void __BeforeScriptEvent_Configure() => BindSystemParams();

    internal void BindSystemParams()
    {
        foreach (FieldInfo Field in SystemParams.FieldsOf(GetType()))
        {
            if (Field.GetValue(this) is not SystemParam Param)
            {
                if (Field.FieldType.IsAbstract)
                {
                    continue;
                }
                Param = (SystemParam)Activator.CreateInstance(Field.FieldType)!;
                Field.SetValue(this, Param);
            }

            Param.Bind(this, Field);
            if (!BoundParams.Contains(Param))
            {
                BoundParams.Add(Param);
            }
        }
    }

    private static bool HoldsSystemParams(TypeLibrary Library, Type ScriptType) => SystemParams.FieldsOf(ScriptType).Length > 0;
}
