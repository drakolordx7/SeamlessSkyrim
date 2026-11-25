using System.Text.RegularExpressions;

namespace Census.Mo2;

/// <summary>Where a resolved plugin file came from.</summary>
public sealed record PluginSource(string Name, string Kind, string Path, int Priority);

/// <summary>
/// A read-only view of a portable Mod Organizer 2 instance: the active profile's load order,
/// enabled plugins, mod priorities and the game Data folder. <see cref="ResolveFile"/> reproduces
/// the virtual file system's rule for root-level files: overwrite, then the highest-priority
/// enabled mod that has the file, then the game Data folder.
/// </summary>
public sealed class Mo2Instance
{
    public required string InstanceDir { get; init; }
    public required string ProfileName { get; init; }
    public required string ProfileDir { get; init; }
    public required string GameDataDir { get; init; }
    public required string ModsDir { get; init; }
    public required string OverwriteDir { get; init; }

    /// <summary>Plugins in loadorder.txt order (all, enabled or not).</summary>
    public required IReadOnlyList<string> LoadOrder { get; init; }
    /// <summary>Plugins with a leading '*' in plugins.txt (case-insensitive set).</summary>
    public required IReadOnlySet<string> EnabledPlugins { get; init; }
    /// <summary>modlist.txt mods, highest priority first.</summary>
    public required IReadOnlyList<ModEntry> Mods { get; init; }
    /// <summary>"Creation Club: X" / "DLC: X" entries (unmanaged '*' lines) from modlist.txt.</summary>
    public required IReadOnlyList<string> UnmanagedEntries { get; init; }

    public sealed record ModEntry(string Name, bool Enabled, int Priority, string Dir, bool DirExists);

    /// <summary>All candidates per root-level file name, highest priority first.</summary>
    private readonly Dictionary<string, List<PluginSource>> _rootFiles = new(StringComparer.OrdinalIgnoreCase);

    public static Mo2Instance Open(string instanceDir, string? profileOverride = null, string? gameDataOverride = null)
    {
        instanceDir = Path.GetFullPath(instanceDir);
        var iniPath = Path.Combine(instanceDir, "ModOrganizer.ini");
        if (!File.Exists(iniPath)) throw new FileNotFoundException("ModOrganizer.ini not found", iniPath);
        var ini = Ini.Parse(File.ReadAllLines(iniPath));

        string baseDir = ExpandDir(ini.Get("Settings", "base_directory"), instanceDir, instanceDir);
        string modsDir = ExpandDir(ini.Get("Settings", "mod_directory"), baseDir, Path.Combine(baseDir, "mods"));
        string profilesDir = ExpandDir(ini.Get("Settings", "profiles_directory"), baseDir, Path.Combine(baseDir, "profiles"));
        string overwriteDir = ExpandDir(ini.Get("Settings", "overwrite_directory"), baseDir, Path.Combine(baseDir, "overwrite"));

        string profile = profileOverride ?? Ini.Unwrap(ini.Get("General", "selected_profile")) ?? "Default";
        string profileDir = Path.Combine(profilesDir, profile);
        if (!Directory.Exists(profileDir)) throw new DirectoryNotFoundException($"Profile folder not found: {profileDir}");

        string dataDir;
        if (gameDataOverride is not null) dataDir = gameDataOverride;
        else
        {
            var gamePath = Ini.Unwrap(ini.Get("General", "gamePath"))
                ?? throw new InvalidDataException("gamePath missing in ModOrganizer.ini");
            dataDir = Path.Combine(gamePath, "Data");
        }
        if (!Directory.Exists(dataDir)) throw new DirectoryNotFoundException($"Game Data folder not found: {dataDir}");

        var loadOrder = ReadLines(Path.Combine(profileDir, "loadorder.txt"))
            .Select(l => l.Trim()).Where(l => l.Length > 0 && l[0] != '#').ToList();

        // plugins.txt (Skyrim SE format): '*' prefix = enabled.
        var enabled = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var raw in ReadLines(Path.Combine(profileDir, "plugins.txt")))
        {
            var l = raw.Trim();
            if (l.Length == 0 || l[0] == '#') continue;
            if (l[0] == '*') enabled.Add(l[1..].Trim());
        }

        // modlist.txt: first line = highest priority; + enabled, - disabled, * unmanaged.
        var rawMods = new List<(string name, bool en)>();
        var unmanaged = new List<string>();
        foreach (var raw in ReadLines(Path.Combine(profileDir, "modlist.txt")))
        {
            var l = raw.TrimEnd();
            if (l.Length < 2 || l[0] == '#') continue;
            string name = l[1..];
            switch (l[0])
            {
                case '+': rawMods.Add((name, true)); break;
                case '-': rawMods.Add((name, false)); break;
                case '*': unmanaged.Add(name); break;
            }
        }
        var mods = new List<ModEntry>();
        for (int i = 0; i < rawMods.Count; i++)
        {
            var (name, en) = rawMods[i];
            string dir = Path.Combine(modsDir, name);
            mods.Add(new ModEntry(name, en, rawMods.Count - 1 - i, dir, Directory.Exists(dir)));
        }

        var inst = new Mo2Instance
        {
            InstanceDir = instanceDir, ProfileName = profile, ProfileDir = profileDir,
            GameDataDir = dataDir, ModsDir = modsDir, OverwriteDir = overwriteDir,
            LoadOrder = loadOrder, EnabledPlugins = enabled, Mods = mods, UnmanagedEntries = unmanaged,
        };
        inst.IndexRootFiles();
        return inst;
    }

    private void IndexRootFiles()
    {
        void Add(string dir, PluginSource src)
        {
            if (!Directory.Exists(dir)) return;
            foreach (var f in Directory.EnumerateFiles(dir))
            {
                string fn = Path.GetFileName(f);
                if (!_rootFiles.TryGetValue(fn, out var list)) _rootFiles[fn] = list = new();
                list.Add(src with { Path = f });
            }
        }
        // Insertion order = priority, highest first.
        Add(OverwriteDir, new PluginSource("overwrite", "overwrite", "", int.MaxValue));
        foreach (var m in Mods.Where(m => m.Enabled && m.DirExists))
            Add(m.Dir, new PluginSource(m.Name, "mod", "", m.Priority));
        Add(GameDataDir, new PluginSource("game Data", "game", "", -1));
    }

    /// <summary>Winning file for a root-level name, or null. <paramref name="shadowed"/> lists the losing candidates.</summary>
    public PluginSource? ResolveFile(string fileName, out IReadOnlyList<PluginSource> shadowed)
    {
        if (_rootFiles.TryGetValue(fileName, out var list) && list.Count > 0)
        {
            shadowed = list.Skip(1).ToList();
            return list[0];
        }
        shadowed = Array.Empty<PluginSource>();
        return null;
    }

    private static IEnumerable<string> ReadLines(string path) =>
        File.Exists(path) ? File.ReadAllLines(path) : Array.Empty<string>();

    private static string ExpandDir(string? raw, string baseDir, string fallback)
    {
        raw = Ini.Unwrap(raw);
        if (string.IsNullOrWhiteSpace(raw)) return fallback;
        raw = raw.Replace("%BASE_DIR%", baseDir, StringComparison.OrdinalIgnoreCase);
        return Path.GetFullPath(Path.IsPathRooted(raw) ? raw : Path.Combine(baseDir, raw));
    }
}

/// <summary>Tiny QSettings-style ini reader (sections, key=value, single line values).</summary>
internal sealed class Ini
{
    private readonly Dictionary<string, string> _v = new(StringComparer.OrdinalIgnoreCase);

    public static Ini Parse(IEnumerable<string> lines)
    {
        var ini = new Ini();
        string section = "General";
        foreach (var raw in lines)
        {
            var l = raw.Trim();
            if (l.Length == 0 || l[0] == ';' || l[0] == '#') continue;
            if (l[0] == '[' && l[^1] == ']') { section = l[1..^1]; continue; }
            int eq = l.IndexOf('=');
            if (eq <= 0) continue;
            ini._v[section + "/" + l[..eq].Trim()] = l[(eq + 1)..].Trim();
        }
        return ini;
    }

    public string? Get(string section, string key) => _v.GetValueOrDefault(section + "/" + key);

    private static readonly Regex ByteArray = new(@"^@ByteArray\((.*)\)$", RegexOptions.Singleline);

    /// <summary>Strips the QSettings @ByteArray(...) wrapper and doubled backslashes.</summary>
    public static string? Unwrap(string? v)
    {
        if (v is null) return null;
        var m = ByteArray.Match(v);
        if (m.Success) v = m.Groups[1].Value;
        return v.Replace("\\\\", "\\");
    }
}
