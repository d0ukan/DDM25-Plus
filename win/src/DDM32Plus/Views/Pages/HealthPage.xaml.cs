// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using DDM32Plus.Core.Radio;
using DDM32Plus.Services;

namespace DDM32Plus.Views.Pages;

/// <summary>Telsizin durumu: the CH340 cable and driver check (Windows' device list, no port); the radio's own
/// health read (plus_live / health) comes in a later stage.</summary>
public partial class HealthPage : UserControl, IPage
{
    private AppState? state;

    public HealthPage() => InitializeComponent();

    public void Init(AppState state, MainWindow window) => this.state = state;

    public void Refresh()
    {
        if (state is null)
            return;
        CableText.Text = CableScanner.Text(state.Cable);
        CableLight.Fill = (Brush)FindResource(state.Cable.State switch
        {
            CableState.Ok => "Green",
            CableState.NoCable => "Grey",
            _ => "Red",
        });
    }

    private void OnCheckCable(object sender, RoutedEventArgs e) => state?.RefreshCable();
}
