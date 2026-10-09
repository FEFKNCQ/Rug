using System;
using System.IO;
using System.Text.Json;

namespace Rug;

internal sealed class AppSettings
{
    public string? RugPath { get; set; }
    public float Scale { get; set; } = 0.46f;

    public string? PrevWallpaper { get; set; }
    public string? PrevWallpaperStyle { get; set; }
    public string? PrevTileWallpaper { get; set; }

    public static string DataDir { get; } =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Rug");

    private static string FilePath => Path.Combine(DataDir, "settings.json");

    public static AppSettings Load()
    {
        try
        {
            if (File.Exists(FilePath))
            {
                var s = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(FilePath));
                if (s != null)
                {
                    s.Scale = Math.Clamp(s.Scale, 0.25f, 0.75f);
                    return s;
                }
            }
        }
        catch (Exception)
        {

        }
        return new AppSettings();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(DataDir);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception)
        {

        }
    }
}
