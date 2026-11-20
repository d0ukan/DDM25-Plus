// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace DDM32Plus.Core.Storage;

/// <summary>
/// settings.json in the install folder: a JSON object written atomically (temporary file, then replace). A missing
/// or damaged file reads as empty settings; unknown keys are kept.
/// </summary>
public sealed class Settings(string path)
{
    private static readonly JsonSerializerOptions Pretty = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public string FilePath { get; } = path;

    public JsonObject Load()
    {
        try
        {
            return JsonNode.Parse(File.ReadAllText(FilePath)) as JsonObject ?? [];
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            return [];
        }
    }

    public void Save(string key, JsonNode? value)
    {
        var s = Load();
        s[key] = value;
        Write(s);
    }

    private void Write(JsonObject s)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
        var tmp = Path.ChangeExtension(FilePath, ".tmp");
        File.WriteAllText(tmp, s.ToJsonString(Pretty) + "\n");
        File.Move(tmp, FilePath, overwrite: true);
    }

    public string? GetString(string key) =>
        Load()[key] is JsonValue v && v.TryGetValue<string>(out var s) ? s : null;

    public bool GetBool(string key, bool fallback) =>
        Load()[key] is JsonValue v && v.TryGetValue<bool>(out var b) ? b : fallback;

    // --- typed settings -------------------------------------------------------------------------------------------
    public const string LanguageKey = "language";
    public const string AutoCheckKey = "auto_check";
    public const string IncludeBetaKey = "include_beta";
    public const string DisclaimerKey = "disclaimer";

    /// <summary>"tr" or "en", or null before the first-start question.</summary>
    public string? Language
    {
        get => GetString(LanguageKey) is "tr" or "en" ? GetString(LanguageKey) : null;
        set => Save(LanguageKey, value);
    }

    public bool AutoCheck
    {
        get => GetBool(AutoCheckKey, true);
        set => Save(AutoCheckKey, value);
    }

    public bool IncludeBeta
    {
        get => GetBool(IncludeBetaKey, true);
        set => Save(IncludeBetaKey, value);
    }

    /// <summary>The disclaimer was accepted for this text version (pc/ddm32_app/disclaimer.py accepted()).</summary>
    public bool DisclaimerAccepted(string version) =>
        Load()[DisclaimerKey] is JsonObject d && d["version"] is JsonValue vv && vv.TryGetValue<string>(out var v) &&
        v == version &&
        d["accepted"] is JsonValue a && a.TryGetValue<bool>(out var ok) && ok;

    public void AcceptDisclaimer(string version, string nowUtc) =>
        Save(DisclaimerKey, new JsonObject { ["version"] = version, ["accepted"] = true, ["accepted_utc"] = nowUtc });

    public void WithdrawDisclaimer(string version, string nowUtc) =>
        Save(DisclaimerKey, new JsonObject { ["version"] = version, ["accepted"] = false, ["withdrawn_utc"] = nowUtc });
}
