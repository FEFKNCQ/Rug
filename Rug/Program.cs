using System;
using System.Threading;
using System.Windows.Forms;

namespace Rug;

internal static class Program
{
    private const string SingleInstanceName = @"Local\Rug.SingleInstance";
    private const string ShowSettingsName = @"Local\Rug.ShowSettings";

    [STAThread]
    private static void Main()
    {
        using var mutex = new Mutex(true, SingleInstanceName, out bool first);
        using var showSettings = new EventWaitHandle(false, EventResetMode.AutoReset, ShowSettingsName);

        if (!first)
        {

            showSettings.Set();
            return;
        }

        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        var settings = AppSettings.Load();
        var tray = new TrayContext(settings);

        new Thread(() =>
        {
            while (true)
            {
                showSettings.WaitOne();
                tray.ShowSettingsFromAnyThread();
            }
        }) { IsBackground = true, Name = "Rug show settings" }.Start();

        tray.ShowSettings();
        Application.Run(tray);
    }
}
