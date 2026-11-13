namespace LuminaBuildTool.Core;

public static class EnumUtils
{
    // Enum.TryParse accepts any numeric string convertible to the underlying type, even one naming no member.
    public static bool TryParseStrict<TEnum>(string Value, out TEnum Parsed) where TEnum : struct, Enum
    {
        return Enum.TryParse(Value, ignoreCase: true, out Parsed) && Enum.IsDefined(Parsed);
    }
}
