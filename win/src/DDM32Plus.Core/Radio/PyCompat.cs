// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Numerics;
using System.Text;

namespace DDM32Plus.Core.Radio;

/// <summary>
/// An error of a radio tool, named like the Python exception the same failure raises in pc/radiotools (DumpError,
/// ValueError, CodecError, ...): receipts and backup manifests record "Type: message", and the C# tools must write
/// the same bytes as the Python tools (win/docs/porting-plan.md).
/// </summary>
public class ToolError(string pyType, string message) : Exception(message)
{
    public string PyType { get; } = pyType;

    /// <summary>f'{type(exc).__name__}: {exc}'</summary>
    public string Record => $"{PyType}: {Message}";
}

/// <summary>ValueError</summary>
public class ValueError(string message, string pyType = "ValueError") : ToolError(pyType, message);

/// <summary>prepare_unit_image.CodecError (a ValueError)</summary>
public sealed class CodecError(string message) : ValueError(message, "CodecError");

/// <summary>The pinned monitor host's DumpError and its kinds</summary>
public class DumpError(string message, string pyType = "DumpError") : ToolError(pyType, message);

/// <summary>A damaged, short or missing frame, or a sector that failed its CRC: worth asking again.</summary>
public sealed class FrameError(string message) : DumpError(message, "FrameError");

/// <summary>The stub answered with a status other than OK.</summary>
public sealed class StatusError(string message, int status) : DumpError(message, "StatusError")
{
    public int Status { get; } = status;
}

/// <summary>FileExistsError: an output that exists already is never replaced.</summary>
public sealed class FileExists(string path) : ToolError("FileExistsError", path);

/// <summary>argparse's error or a SystemExit with a message: the tool stops before doing anything.</summary>
public sealed class ToolExit(string code) : ToolError("SystemExit", code);

/// <summary>
/// An ordered JSON object (a Python dict): keys keep their first position, a new value for a key keeps its place.
/// </summary>
public sealed class PyDict : IEnumerable<KeyValuePair<string, object?>>
{
    private readonly List<string> order = [];
    private readonly Dictionary<string, object?> values = new(StringComparer.Ordinal);

    public object? this[string key]
    {
        get => values[key];
        set
        {
            if (!values.ContainsKey(key))
                order.Add(key);
            values[key] = value;
        }
    }

    public bool ContainsKey(string key) => values.ContainsKey(key);
    public int Count => order.Count;
    public IEnumerable<string> Keys => order;

    public void Add(string key, object? value) => this[key] = value;

    /// <summary>dict(other) then the updates, in order (Python dict(a, **kw) / update).</summary>
    public PyDict Update(PyDict other)
    {
        foreach (var kv in other)
            this[kv.Key] = kv.Value;
        return this;
    }

    public PyDict Copy() => new PyDict().Update(this);

    public IEnumerator<KeyValuePair<string, object?>> GetEnumerator()
    {
        foreach (var k in order)
            yield return new KeyValuePair<string, object?>(k, values[k]);
    }

    System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() => GetEnumerator();
}

/// <summary>Python's json.dumps output, byte for byte (ensure_ascii; indent=None or a number of spaces).</summary>
public static class PyJson
{
    public static string Dumps(object? value, int? indent = 2)
    {
        var sb = new StringBuilder();
        Write(sb, value, indent, 0);
        return sb.ToString();
    }

    private static void Write(StringBuilder sb, object? v, int? indent, int level)
    {
        switch (v)
        {
            case null:
                sb.Append("null");
                break;
            case bool b:
                sb.Append(b ? "true" : "false");
                break;
            case string s:
                Str(sb, s);
                break;
            case int or long or uint or ulong or short or byte or BigInteger:
                sb.Append(Convert.ToString(v, CultureInfo.InvariantCulture));
                break;
            case double d:
                sb.Append(Repr(d));
                break;
            case PyDict dict:
                Container(sb, '{', '}', dict.Select(kv => (object?)kv), dict.Count, indent, level);
                break;
            case System.Collections.IEnumerable list:
                var items = list.Cast<object?>().ToList();
                Container(sb, '[', ']', items, items.Count, indent, level);
                break;
            default:
                throw new ArgumentException($"not JSON: {v.GetType()}");
        }
    }

    private static void Container(StringBuilder sb, char open, char close, IEnumerable<object?> items, int count,
        int? indent, int level)
    {
        sb.Append(open);
        if (count == 0)
        {
            sb.Append(close);
            return;
        }
        var first = true;
        foreach (var item in items)
        {
            if (!first)
                sb.Append(indent is null ? ", " : ",");
            first = false;
            if (indent is { } n)
                sb.Append('\n').Append(' ', n * (level + 1));
            if (item is KeyValuePair<string, object?> kv)
            {
                Str(sb, kv.Key);
                sb.Append(": ");
                Write(sb, kv.Value, indent, level + 1);
            }
            else
                Write(sb, item, indent, level + 1);
        }
        if (indent is { } m)
            sb.Append('\n').Append(' ', m * level);
        sb.Append(close);
    }

    private static void Str(StringBuilder sb, string s)
    {
        sb.Append('"');
        foreach (var c in s)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                case '\b': sb.Append("\\b"); break;
                case '\f': sb.Append("\\f"); break;
                default:
                    if (c < 0x20 || c > 0x7e)
                        sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                    else
                        sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
    }

    /// <summary>Python's repr of a float (shortest round trip).</summary>
    public static string Repr(double d)
    {
        if (double.IsNaN(d)) return "NaN";
        if (double.IsInfinity(d)) return d > 0 ? "Infinity" : "-Infinity";
        var r = d.ToString("R", CultureInfo.InvariantCulture);
        if (r.Contains('E'))
        {
            // Python: 1e-05, 1e+16
            var parts = r.Split('E');
            var exp = int.Parse(parts[1], CultureInfo.InvariantCulture);
            return parts[0] + "e" + (exp < 0 ? "-" : "+") + Math.Abs(exp).ToString("00", CultureInfo.InvariantCulture);
        }
        return r.Contains('.') ? r : r + ".0";
    }

    /// <summary>Python's hex(n): 0x1a2b</summary>
    public static string Hex(long n) => n < 0 ? "-0x" + (-n).ToString("x", CultureInfo.InvariantCulture)
        : "0x" + n.ToString("x", CultureInfo.InvariantCulture);
}

/// <summary>zlib.crc32</summary>
public static class Crc32
{
    private static readonly uint[] Table = MakeTable();

    private static uint[] MakeTable()
    {
        var t = new uint[256];
        for (uint n = 0; n < 256; n++)
        {
            var c = n;
            for (var k = 0; k < 8; k++)
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }

    public static uint Compute(ReadOnlySpan<byte> data, uint crc = 0)
    {
        var c = ~crc;
        foreach (var b in data)
            c = Table[(c ^ b) & 0xFF] ^ (c >> 8);
        return ~c;
    }
}

/// <summary>The pinned monitor host's own LZ4 block decoder (stubtool.lz4_block).</summary>
public static class Lz4
{
    public static byte[] Block(ReadOnlySpan<byte> src, int size)
    {
        var out_ = new List<byte>(size);
        var i = 0;
        var n = src.Length;
        while (i < n)
        {
            int tok = src[i++];
            var lit = tok >> 4;
            if (lit == 15)
            {
                while (true)
                {
                    int b = src[i++];
                    lit += b;
                    if (b != 255)
                        break;
                }
            }
            var end = Math.Min(n, i + lit);         // Python slices past the end without error
            for (var k = i; k < end; k++)
                out_.Add(src[k]);
            i += lit;
            if (i >= n)
                break;
            var off = src[i] | (src[i + 1] << 8);
            i += 2;
            var ml = tok & 15;
            if (ml == 15)
            {
                while (true)
                {
                    int b = src[i++];
                    ml += b;
                    if (b != 255)
                        break;
                }
            }
            ml += 4;
            if (off == 0 || off > out_.Count)
                throw new DumpError("corrupt LZ4 block");
            var start = out_.Count - off;
            for (var k = 0; k < ml; k++)
                out_.Add(out_[start + k]);
        }
        if (out_.Count != size)
            throw new DumpError($"LZ4 block decodes to {out_.Count} bytes, wanted {size}");
        return [.. out_];
    }
}

/// <summary>hashlib.sha256(...).hexdigest()</summary>
public static class Hash
{
    public static string Sha(ReadOnlySpan<byte> data) =>
        Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(data));
}
