// SPDX-License-Identifier: GPL-3.0-or-later
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using DDM32Plus.Core.Crypto;

namespace DDM32Plus.Core.Releases;

/// <summary>
/// The signed DDM32 Plus release manifest: signature check and schema check, a port of
/// pc/radiotools/signed_manifest.py. The manifest file's exact bytes are signed with Ed25519; the .sig file holds the
/// signature as 128 hex digits. The signature is checked before the JSON is parsed.
///
/// The JSON checks follow Python's types on purpose (the tests compare with the Python code's results): an integer
/// field takes only an integer literal (5, not 5.0 or 5e0); size fields also take true (Python's bool is an int),
/// the format fields do not. Python also reads NaN/Infinity literals, which System.Text.Json refuses: such a
/// manifest is refused here (the release tool never writes one).
/// </summary>
public static partial class SignedManifest
{
    [GeneratedRegex("^[0-9a-f]{64}$")]
    private static partial Regex Hex64();

    [GeneratedRegex("^[0-9A-Za-z][0-9A-Za-z._-]{0,120}$")]
    private static partial Regex FileName();

    [GeneratedRegex("^[0-9a-f]{128}$")]
    private static partial Regex Hex128();

    public static bool IsHex64(string s) => Hex64().IsMatch(s);

    public static bool IsSafeFileName(string s) => FileName().IsMatch(s);

    public static string Sha256Hex(ReadOnlySpan<byte> data) => Convert.ToHexStringLower(SHA256.HashData(data));

    public static string Sha256File(string path)
    {
        using var f = File.OpenRead(path);
        return Convert.ToHexStringLower(SHA256.HashData(f));
    }

    /// <summary>The 32-byte key to verify with (the embedded one by default).</summary>
    public static byte[] PublicKey(string? hexKey = null)
    {
        var text = PyStrip(hexKey ?? ReleaseConstants.PublicKeyHex).ToLowerInvariant();
        if (text.Length == 0)
            throw new ManifestException("No release signing key is embedded in this program yet; nothing can be verified");
        if (!Hex64().IsMatch(text))
            throw new ManifestException("The embedded release public key is malformed");
        return Convert.FromHexString(text);
    }

    public static string KeyId(byte[] key) => Sha256Hex(key)[..16];

    /// <summary>The 64 signature bytes from the .sig file's text (ASCII hex, surrounding whitespace allowed).</summary>
    public static byte[] SignatureFromText(byte[] text)
    {
        // Python: text.decode('ascii', 'replace').strip().lower()
        var chars = text.Select(b => b < 0x80 ? (char)b : '�').ToArray();
        return SignatureFromText(new string(chars));
    }

    public static byte[] SignatureFromText(string text)
    {
        var s = PyStrip(text).ToLowerInvariant();
        if (!Hex128().IsMatch(s) || s.EndsWith('\n'))
            throw new ManifestException("The manifest signature file is malformed");
        return Convert.FromHexString(s);
    }

    /// <summary>The .sig text for data (tests and tools only).</summary>
    public static string SignBytes(byte[] data, byte[] secret) => Convert.ToHexStringLower(Ed25519.Sign(secret, data)) + "\n";

    /// <summary>
    /// The parsed manifest when the signature over data verifies with the key and the schema holds; throws
    /// ManifestException otherwise.
    /// </summary>
    public static Manifest VerifyBytes(byte[] data, byte[] sigText, string? hexKey = null, ManifestSchema? schema = null)
    {
        var key = PublicKey(hexKey);
        if (data.Length > ReleaseConstants.MaxManifest)
            throw new ManifestException("Manifest too large or not bytes");
        var sig = SignatureFromText(sigText);
        if (!Ed25519.Verify(key, data, sig))
            throw new ManifestException("Manifest signature is not valid");
        string json;
        try
        {
            json = new UTF8Encoding(false, true).GetString(data);
        }
        catch (DecoderFallbackException)
        {
            throw new ManifestException("Signed manifest is not valid JSON");
        }
        if (json.StartsWith('﻿'))
            throw new ManifestException("Signed manifest is not valid JSON"); // Python: "Unexpected UTF-8 BOM"
        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = 1000 });
        }
        catch (JsonException)
        {
            throw new ManifestException("Signed manifest is not valid JSON");
        }
        using (doc)
            return CheckManifest(doc.RootElement, schema ?? ReleaseConstants.Current);
    }

    /// <summary>The manifest in folder (manifest and .sig of the schema), verified.</summary>
    public static Manifest VerifyFiles(string folder, string? hexKey = null, ManifestSchema? schema = null)
    {
        schema ??= ReleaseConstants.Current;
        byte[] data, sig;
        try
        {
            data = File.ReadAllBytes(Path.Combine(folder, schema.ManifestName));
            sig = File.ReadAllBytes(Path.Combine(folder, schema.SigName));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            throw new ManifestException($"No signed manifest ({schema.ManifestName} and {schema.SigName}) in {folder}");
        }
        return VerifyBytes(data, sig, hexKey, schema);
    }

    public static FirmwareEntry? FirmwareFor(Manifest manifest, string applicationSha256) =>
        manifest.Firmware.FirstOrDefault(e => e.ApplicationSha256 == applicationSha256);

    /// <summary>
    /// The public firmware gate: the manifest beside the package verifies with the embedded key and lists this exact
    /// application. Returns its firmware entry; throws ManifestException otherwise.
    /// </summary>
    public static FirmwareEntry ReleaseGate(byte[] application, string folder, string? hexKey = null)
    {
        var m = VerifyFiles(folder, hexKey);
        var entry = FirmwareFor(m, Sha256Hex(application));
        if (entry is null || entry.ApplicationSize != application.Length)
            throw new ManifestException("This application is not listed in the signed DDM32 Plus release manifest");
        return entry;
    }

    // --- schema ---------------------------------------------------------------------------------------------------
    private static void Need(bool cond, string what)
    {
        if (!cond)
            throw new ManifestException($"Manifest field invalid: {what}");
    }

    /// <summary>Schema check of a parsed (already signature-verified) manifest.</summary>
    public static Manifest CheckManifest(JsonElement m, ManifestSchema schema)
    {
        Need(m.ValueKind == JsonValueKind.Object && StrEquals(Get(m, "format"), schema.Format), "format");
        Need(StrEquals(Get(m, "repo"), schema.Repo), "repo");
        var release = Get(m, "release");
        Need(IsStr(release) && FileName().IsMatch(release!.Value.GetString()!), "release");
        var created = Get(m, "created_utc");
        Need(IsStr(created), "created_utc");
        var fw = Get(m, "firmware");
        Need(fw?.ValueKind == JsonValueKind.Array, "firmware");
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var entries = new List<FirmwareEntry>();
        var i = 0;
        foreach (var e in fw!.Value.EnumerateArray())
        {
            var where = $"firmware[{i++}]";
            var a = Asset(e, where, schema);
            Need(Get(e, "channel") is { ValueKind: JsonValueKind.String } ch &&
                 ReleaseConstants.Channels.Contains(ch.GetString()), $"{where}.channel");
            var appSha = Get(e, "application_sha256");
            Need(IsStr(appSha) && Hex64().IsMatch(appSha!.Value.GetString()!), $"{where}.application_sha256");
            Need(PyInt(Get(e, "application_size"), allowBool: true) is { } appSize &&
                 appSize > 0 && appSize <= ReleaseConstants.CodecSlot - ReleaseConstants.AppStart,
                $"{where}.application_size");
            var formats = new int[2];
            var k = 0;
            foreach (var name in new[] { "settings_format", "codeplug_format" })
            {
                Need(PyInt(Get(e, name), allowBool: false) is { } v && v >= 0 && v < 1 << 16, $"{where}.{name}");
                formats[k++] = (int)PyInt(Get(e, name), allowBool: false)!.Value;
            }
            var minc = Get(e, "min_compatible_version");
            Need(IsStr(minc), $"{where}.min_compatible_version");
            ReleaseVersion.Parse(minc!.Value.GetString());
            var notes = Get(e, "notes");
            Need(notes?.ValueKind == JsonValueKind.Object && IsStr(Get(notes.Value, "tr")) && IsStr(Get(notes.Value, "en")),
                $"{where}.notes");
            Need(!seen.Contains(a.Version), $"{where}.version (duplicate)");
            seen.Add(a.Version);
            entries.Add(new FirmwareEntry(a.Version, Get(e, "channel")!.Value.GetString()!, a.File, a.Url, a.Sha256, a.Size,
                appSha!.Value.GetString()!, (long)PyInt(Get(e, "application_size"), true)!.Value, formats[0], formats[1],
                minc.Value.GetString()!, new Notes(Get(notes!.Value, "tr")!.Value.GetString()!,
                    Get(notes.Value, "en")!.Value.GetString()!)));
        }
        AssetEntry? app = null, appNet = null;
        foreach (var (key, isNet) in new[] { ("app", false), ("app_net", true) })
        {
            var el = Get(m, key);
            if (el is null || el.Value.ValueKind == JsonValueKind.Null)
                continue;
            var a = Asset(el.Value, key, schema);
            Notes? notes = null;
            if (Get(el.Value, "notes") is { } n)
            {
                Need(n.ValueKind == JsonValueKind.Object, $"{key}.notes");
                notes = new Notes(Get(n, "tr") is { ValueKind: JsonValueKind.String } tr ? tr.GetString()! : "",
                    Get(n, "en") is { ValueKind: JsonValueKind.String } en ? en.GetString()! : "");
            }
            a = a with { Notes = notes };
            if (isNet) appNet = a; else app = a;
        }
        return new Manifest(schema, release!.Value.GetString()!, created!.Value.GetString()!, entries, app, appNet);
    }

    private static AssetEntry Asset(JsonElement entry, string where, ManifestSchema schema)
    {
        Need(entry.ValueKind == JsonValueKind.Object, where);
        var version = Get(entry, "version");
        Need(IsStr(version), $"{where}.version");
        ReleaseVersion.Parse(version!.Value.GetString());
        var file = Get(entry, "file");
        Need(IsStr(file) && FileName().IsMatch(file!.Value.GetString()!) && !file.Value.GetString()!.Contains(".."),
            $"{where}.file");
        var f = file!.Value.GetString()!;
        var url = Get(entry, "url");
        Need(IsStr(url) && url!.Value.GetString()! is var u && u.StartsWith(schema.UrlPrefix, StringComparison.Ordinal) &&
             u.EndsWith("/" + f, StringComparison.Ordinal) && !u.Contains("..") && !u.Contains('?') && !u.Contains('#'),
            $"{where}.url");
        var sha = Get(entry, "sha256");
        Need(IsStr(sha) && Hex64().IsMatch(sha!.Value.GetString()!), $"{where}.sha256");
        Need(PyInt(Get(entry, "size"), allowBool: true) is { } size && size > 0 && size <= ReleaseConstants.MaxAssetSize,
            $"{where}.size");
        return new AssetEntry(version.Value.GetString()!, f, url!.Value.GetString()!, sha!.Value.GetString()!,
            (long)PyInt(Get(entry, "size"), true)!.Value, null);
    }

    // --- Python semantics of the parsed JSON ----------------------------------------------------------------------
    /// <summary>dict.get(name): the member's value (the last one when repeated, like json.loads), or null.</summary>
    internal static JsonElement? Get(JsonElement obj, string name)
    {
        if (obj.ValueKind != JsonValueKind.Object)
            return null;
        JsonElement? found = null;
        foreach (var p in obj.EnumerateObject())
            if (p.NameEquals(name))
                found = p.Value;
        return found;
    }

    private static bool IsStr(JsonElement? e) => e?.ValueKind == JsonValueKind.String;

    private static bool StrEquals(JsonElement? e, string value) => IsStr(e) && e!.Value.GetString() == value;

    /// <summary>The value when Python's json would give an int (bool too when allowBool), else null.</summary>
    internal static BigInteger? PyInt(JsonElement? e, bool allowBool)
    {
        if (e is null)
            return null;
        switch (e.Value.ValueKind)
        {
            case JsonValueKind.True:
                return allowBool ? BigInteger.One : null;
            case JsonValueKind.False:
                return allowBool ? BigInteger.Zero : null;
            case JsonValueKind.Number:
                var raw = e.Value.GetRawText();
                if (raw.IndexOfAny(['.', 'e', 'E']) >= 0)
                    return null;
                return BigInteger.Parse(raw, System.Globalization.CultureInfo.InvariantCulture);
            default:
                return null;
        }
    }

    /// <summary>Python's str.strip() for the characters that can occur here (ASCII whitespace incl. \x1c-\x1f).</summary>
    internal static string PyStrip(string s)
    {
        static bool Ws(char c) => c is ' ' or '\t' or '\n' or '\v' or '\f' or '\r' or '\x1c' or '\x1d' or '\x1e' or '\x1f'
            || (c > 0x7f && char.IsWhiteSpace(c));
        int a = 0, b = s.Length;
        while (a < b && Ws(s[a])) a++;
        while (b > a && Ws(s[b - 1])) b--;
        return s[a..b];
    }
}
