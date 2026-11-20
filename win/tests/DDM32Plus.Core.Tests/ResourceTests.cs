// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.RegularExpressions;
using System.Xml.Linq;
using DDM32Plus.Core.Releases;

namespace DDM32Plus.Core.Tests;

/// <summary>
/// The UI strings (src/DDM32Plus/Resources/Strings.resx English, Strings.tr.resx Turkish): same keys in both
/// languages, same placeholders, every key the XAML and code use exists, and no visible text is written into the
/// XAML itself.
/// </summary>
public partial class ResourceTests
{
    private static string WinRoot()
    {
        var d = new DirectoryInfo(AppContext.BaseDirectory);
        while (d is not null && !File.Exists(Path.Combine(d.FullName, "DDM32Plus.sln")))
            d = d.Parent;
        return d?.FullName ?? throw new DirectoryNotFoundException("win/DDM32Plus.sln not found above the test folder");
    }

    private static string App => Path.Combine(WinRoot(), "src", "DDM32Plus");

    private static Dictionary<string, string> Load(string name) =>
        XDocument.Load(Path.Combine(App, "Resources", name)).Root!.Elements("data")
            .ToDictionary(e => (string)e.Attribute("name")!, e => (string)e.Element("value")!);

    [GeneratedRegex(@"\{(\d+)(:[^}]*)?\}")]
    private static partial Regex Placeholder();

    [GeneratedRegex(@"\{l:Tr (\w+)\}")]
    private static partial Regex XamlKey();

    [GeneratedRegex(@"Loc\.(?:T|F)\(""(\w+)""\s*[,)]")]
    private static partial Regex CodeKey();

    // icon glyphs (&#xE897;) are not text
    [GeneratedRegex(@"\s(Text|Content|Header|ToolTip|Title)=""([^""{&][^""]*)""")]
    private static partial Regex LiteralText();

    [Fact]
    public void BothLanguagesHaveTheSameKeysAndPlaceholders()
    {
        var en = Load("Strings.resx");
        var tr = Load("Strings.tr.resx");
        Assert.True(en.Count > 150);
        Assert.Equal(en.Keys.Order(StringComparer.Ordinal), tr.Keys.Order(StringComparer.Ordinal));
        foreach (var (key, text) in en)
        {
            Assert.False(string.IsNullOrWhiteSpace(text), $"{key} (en) is empty");
            Assert.False(string.IsNullOrWhiteSpace(tr[key]), $"{key} (tr) is empty");
            Assert.True(Placeholders(text).SequenceEqual(Placeholders(tr[key])), $"{key}: placeholders differ");
            Assert.True(Count(text, "**") % 2 == 0 && Count(tr[key], "**") % 2 == 0, $"{key}: unpaired **");
        }
    }

    private static string[] Placeholders(string s) =>
        [.. Placeholder().Matches(s).Select(m => m.Groups[1].Value).Distinct().Order(StringComparer.Ordinal)];

    private static int Count(string s, string what) => (s.Length - s.Replace(what, "", StringComparison.Ordinal).Length) / what.Length;

    [Fact]
    public void EveryUsedKeyExists()
    {
        var keys = Load("Strings.resx").Keys.ToHashSet(StringComparer.Ordinal);
        var used = new List<string>();
        foreach (var f in Directory.EnumerateFiles(App, "*.xaml", SearchOption.AllDirectories))
            used.AddRange(XamlKey().Matches(File.ReadAllText(f)).Select(m => m.Groups[1].Value));
        foreach (var f in Directory.EnumerateFiles(App, "*.cs", SearchOption.AllDirectories).Where(f => !f.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}")))
            used.AddRange(CodeKey().Matches(File.ReadAllText(f)).Select(m => m.Groups[1].Value));
        // keys built at run time
        used.AddRange(Enum.GetNames<ReleaseLabel>().Select(n => "Label_" + n));
        used.AddRange(Enumerable.Range(1, 8).SelectMany(i => new[] { $"Help_{i}_T", $"Help_{i}_B" }));
        used.AddRange(["Step_Prep", "Step_Backup", "Step_Check", "Step_Install", "Step_Done"]);
        used.AddRange(["Disc_Title", "Disc_Intro", "Disc_B1", "Disc_B2", "Disc_B3", "Disc_B4", "Disc_B5", "Disc_Outro"]);
        Assert.True(used.Count > 100);
        var missing = used.Where(k => !keys.Contains(k)).Distinct().ToList();
        Assert.True(missing.Count == 0, "missing keys: " + string.Join(", ", missing));
    }

    [Fact]
    public void NoVisibleTextIsWrittenIntoTheXaml()
    {
        foreach (var f in Directory.EnumerateFiles(App, "*.xaml", SearchOption.AllDirectories)
                     .Where(f => !f.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}")))
        {
            var hits = LiteralText().Matches(File.ReadAllText(f)).Select(m => m.Value.Trim()).ToList();
            Assert.True(hits.Count == 0, $"{Path.GetFileName(f)}: {string.Join(", ", hits)}");
        }
    }
}
