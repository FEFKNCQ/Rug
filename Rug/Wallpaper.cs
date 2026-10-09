using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace Rug;

internal static class Wallpaper
{
    private const uint SPI_SETDESKWALLPAPER = 0x0014;
    private const uint SPIF_UPDATEINIFILE = 0x01;
    private const uint SPIF_SENDCHANGE = 0x02;
    private const string DesktopKey = @"Control Panel\Desktop";

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SystemParametersInfoW(uint action, uint param, string? value, uint winIni);

    private static string WallpaperDir => Path.Combine(AppSettings.DataDir, "wallpapers");

    public static string? Current()
    {
        using var key = Registry.CurrentUser.OpenSubKey(DesktopKey);
        return key?.GetValue("Wallpaper") as string;
    }

    public static void Apply(AppSettings settings, string floorPath)
    {
        Directory.CreateDirectory(WallpaperDir);
        string copy = Path.Combine(WallpaperDir, Path.GetFileName(floorPath));
        File.Copy(floorPath, copy, true);

        string? current = Current();
        bool alreadyOurs = current != null && current.StartsWith(WallpaperDir, StringComparison.OrdinalIgnoreCase);
        if (settings.PrevWallpaper == null && !alreadyOurs)
        {
            using var key = Registry.CurrentUser.OpenSubKey(DesktopKey);
            settings.PrevWallpaper = current ?? "";
            settings.PrevWallpaperStyle = key?.GetValue("WallpaperStyle")?.ToString() ?? "0";
            settings.PrevTileWallpaper = key?.GetValue("TileWallpaper")?.ToString() ?? "0";
        }

        using (var key = Registry.CurrentUser.OpenSubKey(DesktopKey, true))
        {
            key?.SetValue("WallpaperStyle", "10");
            key?.SetValue("TileWallpaper", "0");
        }
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, copy, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
        settings.Save();
    }

    public static bool CanRestore(AppSettings settings) => settings.PrevWallpaper != null;

    public static void Restore(AppSettings settings)
    {
        if (settings.PrevWallpaper == null) return;
        using (var key = Registry.CurrentUser.OpenSubKey(DesktopKey, true))
        {
            key?.SetValue("WallpaperStyle", settings.PrevWallpaperStyle ?? "0");
            key?.SetValue("TileWallpaper", settings.PrevTileWallpaper ?? "0");
        }
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, settings.PrevWallpaper, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
        settings.PrevWallpaper = null;
        settings.PrevWallpaperStyle = null;
        settings.PrevTileWallpaper = null;
        settings.Save();
    }
}
