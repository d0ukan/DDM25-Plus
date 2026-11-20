// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using DDM32Plus.Core;
using DDM32Plus.Localization;
using DDM32Plus.Services;

namespace DDM32Plus.Views;

/// <summary>Help and About, built from the resx texts.</summary>
public partial class InfoDialog : Window
{
    private InfoDialog(string title)
    {
        InitializeComponent();
        Title = title;
    }

    private void Heading(string text, double size)
    {
        Body.Children.Add(new TextBlock
        {
            Text = text, FontSize = size, FontWeight = FontWeights.Bold, TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 10, 0, 4),
        });
    }

    private void Para(string text, double left = 0)
    {
        var tb = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(left, 2, 0, 4) };
        Rich.Fill(tb.Inlines, text);
        Body.Children.Add(tb);
    }

    public static void ShowHelp(Window owner, AppState state)
    {
        var d = new InfoDialog(Loc.T("Help_WindowTitle")) { Owner = owner };
        d.Heading(Loc.T("Help_Title"), 22);
        for (var i = 1; i <= 8; i++)
        {
            d.Heading(Loc.T($"Help_{i}_T"), 16);
            d.Para(i == 7 ? Loc.F("Help_7_B", state.Paths.Backups) : Loc.T($"Help_{i}_B"));
        }
        d.ShowDialog();
    }

    public static void ShowAbout(Window owner)
    {
        var d = new InfoDialog(Loc.T("About_WindowTitle")) { Owner = owner, Width = 560, Height = 480 };
        d.Heading(AppInfo.Name, 22);
        d.Para(Loc.F("About_Line", AppInfo.Author, AppState.ProgramVersion));
        d.Para(AppInfo.RepoUrl);
        d.Para(Loc.T("About_Desc"));
        d.Heading(Loc.T("About_Credits"), 16);
        foreach (var k in new[] { "About_C1", "About_C2", "About_C3", "About_C4" })
            d.Para("• " + Loc.T(k), 8);
        d.Para(Loc.T("About_Footer"));
        d.ShowDialog();
    }
}
