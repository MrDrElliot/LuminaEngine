namespace LuminaBuildTool.Core;

/// <summary>
/// Parses "Mode Positional... -Flag -Key=Value" argument lists. Keys are case insensitive.
/// </summary>
public sealed class CommandLine
{
    private readonly Dictionary<string, string> Options = new(StringComparer.OrdinalIgnoreCase);

    private readonly List<string> Positionals = new();

    public CommandLine(IEnumerable<string> Args)
    {
        foreach (string Arg in Args)
        {
            if (Arg.Length == 0)
            {
                continue;
            }

            if (Arg[0] is '-' or '/')
            {
                string Body = Arg.Substring(1).TrimStart('-');
                int Split = Body.IndexOfAny(new[] { '=', ':' });

                if (Split >= 0)
                {
                    Options[Body.Substring(0, Split)] = Body.Substring(Split + 1).Trim('"');
                }
                else
                {
                    Options[Body] = "true";
                }
            }
            else
            {
                Positionals.Add(Arg);
            }
        }
    }

    public IReadOnlyList<string> Arguments => Positionals;

    public string? GetPositional(int Index) => Index < Positionals.Count ? Positionals[Index] : null;

    public bool HasFlag(string Name) => Options.ContainsKey(Name);

    public string? GetString(string Name) => Options.TryGetValue(Name, out string? Value) ? Value : null;

    public string GetString(string Name, string Default) => GetString(Name) ?? Default;

    public bool GetBool(string Name, bool Default)
    {
        string? Value = GetString(Name);

        if (Value is null)
        {
            return Default;
        }

        return Value.Equals("true", StringComparison.OrdinalIgnoreCase)
            || Value.Equals("1", StringComparison.Ordinal)
            || Value.Equals("on", StringComparison.OrdinalIgnoreCase)
            || Value.Equals("yes", StringComparison.OrdinalIgnoreCase);
    }

    public int GetInt(string Name, int Default)
    {
        string? Value = GetString(Name);
        return Value is not null && int.TryParse(Value, out int Parsed) ? Parsed : Default;
    }

    public TEnum GetEnum<TEnum>(string Name, TEnum Default) where TEnum : struct, Enum
    {
        string? Value = GetString(Name);

        if (Value is null)
        {
            return Default;
        }

        if (!Enum.TryParse(Value, ignoreCase: true, out TEnum Parsed))
        {
            throw new BuildException($"'{Value}' is not a valid {typeof(TEnum).Name}. Valid values: {string.Join(", ", Enum.GetNames<TEnum>())}");
        }

        return Parsed;
    }
}
