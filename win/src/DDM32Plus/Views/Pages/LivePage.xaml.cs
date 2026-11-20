// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows.Controls;
using DDM32Plus.Services;

namespace DDM32Plus.Views.Pages;

/// <summary>The live page: its texts and layout; the radio part comes in the next stage (win/docs/porting-plan.md).</summary>
public partial class LivePage : UserControl, IPage
{
    public LivePage() => InitializeComponent();

    public void Init(AppState state, MainWindow window)
    {
    }

    public void Refresh()
    {
    }
}
