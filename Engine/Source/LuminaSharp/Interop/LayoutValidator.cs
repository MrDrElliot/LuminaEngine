using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;

namespace LuminaSharp;

/// <summary>
/// Cross-language layout safety net. At bootstrap every <see cref="NativeLayoutAttribute">[NativeLayout]</see>
/// mirror's managed size (<c>Unsafe.SizeOf&lt;T&gt;()</c>) is compared against the real native <c>sizeof</c>
/// reported by the engine.
/// </summary>
internal static unsafe partial class LayoutValidator
{
    /// <summary>Native size for a registered key, or -1 if the native side doesn't know it (also a failure).</summary>
    [NativeCall("LuminaSharp_Layout_GetSize")]
    private static partial int NativeSize(string Name);

    /// <summary>Native (int)EPropertyTypeFlags for a plain enumerator name ("Int8".."InstancedStruct"), or -1.</summary>
    [NativeCall("LuminaSharp_PropertyType_Value")]
    private static partial int PropertyTypeNativeValue(string Name);

    /// <summary>Native EPropertyTypeFlags::Count (the member total, including None).</summary>
    [NativeCall("LuminaSharp_PropertyType_Count")]
    private static partial int PropertyTypeNativeCount();

    private static readonly MethodInfo SizeOf =
        typeof(Unsafe).GetMethod(nameof(Unsafe.SizeOf), BindingFlags.Public | BindingFlags.Static)!;

    // Namespaces where every non-enum value struct is a native mirror and MUST carry [NativeLayout]. Catches a
    // newly-added RHI struct that someone forgot to register, before it can become a silent interop hole.
    private static readonly string[] StrictNamespaces = { "LuminaSharp.Rendering" };

    /// <summary>Validate every mirror. Returns true if all match; otherwise logs each offender and returns false.</summary>
    internal static bool ValidateAll()
    {
        Type[] Types;
        try
        {
            Types = typeof(LayoutValidator).Assembly.GetTypes();
        }
        catch (ReflectionTypeLoadException Ex)
        {
            Types = Array.FindAll(Ex.Types, T => T != null)!;
        }

        int Validated = 0;
        StringBuilder Failures = new();

        foreach (Type Type in Types)
        {
            NativeLayoutAttribute? Attribute = Type.GetCustomAttribute<NativeLayoutAttribute>();

            if (Attribute == null)
            {
                if (Type.IsValueType && !Type.IsEnum && !Type.IsPrimitive && !Type.IsGenericType
                    && !Type.IsNested && Type.Name.IndexOf('<') < 0
                    && Type.Namespace is string Ns && Array.IndexOf(StrictNamespaces, Ns) >= 0)
                {
                    Failures.Append($"\n  - {Type.FullName}: a struct in '{Ns}' is missing [NativeLayout].");
                }
                continue;
            }

            if (!Type.IsValueType)
            {
                Failures.Append($"\n  - {Type.FullName}: [NativeLayout] is only valid on value types.");
                continue;
            }

            int ManagedSize = (int)SizeOf.MakeGenericMethod(Type).Invoke(null, null)!;
            int NativeBytes = NativeSize(Attribute.NativeType);

            if (NativeBytes < 0)
            {
                Failures.Append($"\n  - {Type.FullName}: native type '{Attribute.NativeType}' is not registered (LE_REGISTER_LAYOUT missing).");
            }
            else if (NativeBytes != ManagedSize)
            {
                Failures.Append($"\n  - {Type.FullName}: SIZE MISMATCH, C# is {ManagedSize} bytes, native '{Attribute.NativeType}' is {NativeBytes} bytes.");
            }
            else
            {
                Validated++;
            }
        }

        // The reflected EPropertyType enum is a hand-written mirror of native EPropertyTypeFlags; the script
        // schema wire sends its values as kind bytes, so a drift would silently misread every property. Check
        // each enumerator's integer value against native and the total member count.
        Array PropertyTypes = Enum.GetValues(typeof(EPropertyType));
        foreach (EPropertyType Value in PropertyTypes)
        {
            int NativeValue = PropertyTypeNativeValue(Value.ToString());
            if (NativeValue != (int)Value)
            {
                Failures.Append($"\n  - EPropertyType.{Value} is {(int)Value} in C# but native reports {NativeValue}.");
            }
        }
        int NativeTypeCount = PropertyTypeNativeCount();
        if (NativeTypeCount != PropertyTypes.Length)
        {
            Failures.Append($"\n  - EPropertyType has {PropertyTypes.Length} members in C# but native EPropertyTypeFlags has {NativeTypeCount}.");
        }

        if (Failures.Length > 0)
        {
            Debug.LogError($"FATAL: C#/C++ interop layout validation FAILED, refusing to start C# (a mismatched blittable layout or enum would corrupt memory). Fix the C# mirror or the native type:{Failures}");
            return false;
        }

        Debug.Log($"Interop layout validated: {Validated} native mirrors + {PropertyTypes.Length} EPropertyType members match native.");
        return true;
    }
}
