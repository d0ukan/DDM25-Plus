// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using DDM32Plus.Localization;

namespace DDM32Plus.Views;

/// <summary>
/// The disclaimer (pc/ddm32_app/disclaimer.py): shown at the first start and from the menu, in the UI language with a
/// button for the other one; accepted only with the box ticked.
/// </summary>
public partial class DisclaimerDialog : Window
{
    private string lang;

    public DisclaimerDialog()
    {
        InitializeComponent();
        lang = Loc.Instance.Language;
        Fill();
    }

    private void Fill()
    {
        Body.Children.Clear();
        var title = new TextBlock { FontSize = 20, FontWeight = FontWeights.Bold, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 10) };
        title.Text = Loc.Instance.In(lang, "Disc_Title");
        Body.Children.Add(title);
        Body.Children.Add(Para(Loc.Instance.In(lang, "Disc_Intro")));
        foreach (var k in new[] { "Disc_B1", "Disc_B2", "Disc_B3", "Disc_B4", "Disc_B5" })
        {
            var row = new DockPanel { Margin = new Thickness(12, 2, 0, 2) };
            var dot = new TextBlock { Text = "•", Margin = new Thickness(0, 3, 8, 0) };
            DockPanel.SetDock(dot, Dock.Left);
            row.Children.Add(dot);
            row.Children.Add(Para(Loc.Instance.In(lang, k)));
            Body.Children.Add(row);
        }
        Body.Children.Add(Para(Loc.Instance.In(lang, "Disc_Outro")));
        LangButton.Content = lang == "en" ? Loc.Instance.In("tr", "Lang_Tr") : Loc.Instance.In("en", "Lang_En");
    }

    private static TextBlock Para(string text)
    {
        var tb = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 4, 0, 4) };
        Rich.Fill(tb.Inlines, text);
        return tb;
    }

    private void OnToggleLanguage(object sender, RoutedEventArgs e)
    {
        lang = lang == "en" ? "tr" : "en";
        Fill();
    }

    private void OnCheck(object sender, RoutedEventArgs e) => AcceptButton.IsEnabled = Check.IsChecked == true;

    private void OnAccept(object sender, RoutedEventArgs e)
    {
        if (Check.IsChecked == true)
            DialogResult = true;
    }
}
