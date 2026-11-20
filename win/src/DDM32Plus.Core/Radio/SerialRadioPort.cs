// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO.Ports;

namespace DDM32Plus.Core.Radio;

/// <summary>
/// A real serial port with pyserial's behaviour as the Python tools use it: 115200 8N1, DTR and RTS low, read(n)
/// returns what arrived within the timeout. Opening it refuses unless <see cref="RadioAccess.Enabled"/> (a second
/// lock behind RadioAccess.Factory, the only place that creates one).
/// </summary>
public sealed class SerialRadioPort(string name) : IRawSerial, IDisposable
{
    private readonly SerialPort port = new()
    {
        PortName = name, BaudRate = 115200, DataBits = 8, Parity = Parity.None, StopBits = StopBits.One,
        Handshake = Handshake.None, DtrEnable = false, RtsEnable = false, ReadTimeout = 20, WriteTimeout = 3000,
        ReadBufferSize = 1 << 20,
    };

    public double Timeout { get; set; } = 0.02;

    public int BaudRate
    {
        get => port.BaudRate;
        set => port.BaudRate = value;
    }

    public void Open()
    {
#if DDM32_RADIO_ACCESS
        port.Open();
#else
        throw new RadioAccess.DisabledException();
#endif
    }

    public void Close()
    {
        if (port.IsOpen)
            port.Close();
    }

    public void Dispose() => port.Dispose();

    public int? Write(byte[] data)
    {
        port.Write(data, 0, data.Length);
        return data.Length;
    }

    public byte[] Read(int n)
    {
        var buf = new byte[n];
        var got = 0;
        var end = DateTime.UtcNow + TimeSpan.FromSeconds(Timeout);
        while (got < n)
        {
            var avail = port.BytesToRead;
            if (avail > 0)
            {
                got += port.Read(buf, got, Math.Min(avail, n - got));
                continue;
            }
            if (DateTime.UtcNow >= end)
                break;
            Thread.Sleep(1);
        }
        return buf[..got];
    }

    public void ResetInputBuffer() => port.DiscardInBuffer();

    public void Flush() => port.BaseStream.Flush();
}
