// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;

namespace DDM32Plus.Views;

/// <summary>Türkçe / English, asked once at the first start (bilingual: no language is chosen yet).</summary>
public partial class LanguageDialog : Window
{
    public string? Choice { get; private set; }

    public LanguageDialog() => InitializeComponent();

    private void OnTurkish(object sender, RoutedEventArgs e)
    {
        Choice = "tr";
        DialogResult = true;
    }

    private void OnEnglish(object sender, RoutedEventArgs e)
    {
        Choice = "en";
        DialogResult = true;
    }
}
