namespace LuminaBuildTool.Core;

public static class XmlUtils
{
    // Attribute-safe: every generated project file puts these through an attribute value at some point.
    public static string Escape(string Value)
    {
        return Value
            .Replace("&", "&amp;")
            .Replace("<", "&lt;")
            .Replace(">", "&gt;")
            .Replace("\"", "&quot;");
    }
}
