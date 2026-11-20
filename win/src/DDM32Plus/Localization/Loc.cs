// SPDX-License-Identifier: GPL-3.0-or-later
using System.ComponentModel;
using System.Globalization;
using System.Resources;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Markup;

namespace DDM32Plus.Localization;

/// <summary>
/// The UI strings (Resources/Strings.resx English, Strings.tr.resx Turkish). XAML binds to them with {l:Tr Key};
/// a language change updates every bound text at once, code-made texts listen to <see cref="Changed"/>.
/// </summary>
public sealed class Loc : INotifyPropertyChanged
{
    public static Loc Instance { get; } = new();

    private readonly ResourceManager resources = new("DDM32Plus.Resources.Strings", typeof(Loc).Assembly);

    public CultureInfo Culture { get; private set; } = new("tr-TR");

    /// <summary>"tr" or "en".</summary>
    public string Language => Culture.TwoLetterISOLanguageName == "en" ? "en" : "tr";

    public event PropertyChangedEventHandler? PropertyChanged;

    public event Action? Changed;

    public string this[string key] => resources.GetString(key, Culture) ?? $"[{key}]";

    /// <summary>A string in a given language ("tr" or "en"), whatever the UI language is.</summary>
    public string In(string lang, string key) =>
        resources.GetString(key, new CultureInfo(lang == "en" ? "en-GB" : "tr-TR")) ?? $"[{key}]";

    public void SetLanguage(string code)
    {
        Culture = new CultureInfo(code == "en" ? "en-GB" : "tr-TR");
        CultureInfo.CurrentUICulture = Culture;
        CultureInfo.DefaultThreadCurrentUICulture = Culture;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs("Item[]"));
        Changed?.Invoke();
    }

    public static string T(string key) => Instance[key];

    public static string F(string key, params object?[] args) => string.Format(Instance.Culture, Instance[key], args);
}

/// <summary>{l:Tr Key}: a one-way binding to Loc.Instance[Key].</summary>
[MarkupExtensionReturnType(typeof(object))]
public sealed class TrExtension : MarkupExtension
{
    public TrExtension()
    {
    }

    public TrExtension(string key) => Key = key;

    [ConstructorArgument("key")]
    public string Key { get; set; } = "";

    public override object ProvideValue(IServiceProvider serviceProvider) =>
        new Binding($"[{Key}]") { Source = Loc.Instance, Mode = BindingMode.OneWay }.ProvideValue(serviceProvider);
}

/// <summary>
/// Rich.Text on a TextBlock: the string's **bold** parts in bold and its line breaks as line breaks (the resx
/// texts' only markup, standing in for the Python program's HTML).
/// </summary>
public static class Rich
{
    public static readonly DependencyProperty TextProperty = DependencyProperty.RegisterAttached(
        "Text", typeof(string), typeof(Rich), new PropertyMetadata(null, OnText));

    public static string? GetText(DependencyObject d) => (string?)d.GetValue(TextProperty);

    public static void SetText(DependencyObject d, string? value) => d.SetValue(TextProperty, value);

    private static void OnText(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is TextBlock tb)
            Fill(tb.Inlines, e.NewValue as string ?? "");
    }

    public static void Fill(InlineCollection inlines, string text)
    {
        inlines.Clear();
        var lines = text.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');
        for (var i = 0; i < lines.Length; i++)
        {
            if (i > 0)
                inlines.Add(new LineBreak());
            var parts = lines[i].Split("**");
            for (var j = 0; j < parts.Length; j++)
            {
                if (parts[j].Length == 0)
                    continue;
                var run = new Run(parts[j]);
                inlines.Add(j % 2 == 1 ? new Bold(run) : run);
            }
        }
    }
}
