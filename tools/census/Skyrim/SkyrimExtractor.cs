using Census.Model;
using Census.Mo2;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Cache;
using Mutagen.Bethesda.Plugins.Order;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;
using Noggog;

namespace Census.Skyrim;

/// <summary>
/// Reads a Skyrim SE load order read-only through Mutagen (binary overlays, winning overrides)
/// and produces engine-agnostic census records. Plugin file paths come from the MO2 resolver.
/// </summary>
public static class SkyrimExtractor
{
    private const GameRelease Release = GameRelease.SkyrimSE;
    private const float CellSize = 4096f;

    private static readonly HashSet<string> DlcPlugins = new(StringComparer.OrdinalIgnoreCase)
        { "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" };
    private static readonly HashSet<string> BasePlugins = new(StringComparer.OrdinalIgnoreCase)
        { "Skyrim.esm", "Update.esm" };

    private sealed record CellInfo(bool Interior, int[]? Grid);

    public static CensusData Extract(Mo2Instance mo2, Action<string> log)
    {
        var data = new CensusData { Game = "SkyrimSE" };
        var cc = LoadCreationNames(mo2);
        string KindOf(string plugin) =>
            BasePlugins.Contains(plugin) ? "base" :
            DlcPlugins.Contains(plugin) ? "dlc" :
            cc.Contains(plugin) ? "creation" : "mod";

        // ---- 1. plugin list and MO2-style path resolution
        var implicits = Implicits.Get(Release).BaseMasters;
        var modPaths = new List<ModPath>();
        var sourceByPlugin = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        int index = 0;
        var seenNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var name in mo2.LoadOrder)
        {
            if (!seenNames.Add(name))
            {
                data.Issues.Add($"loadorder.txt lists '{name}' a second time (different letter case); the first position is used");
                continue;
            }
            if (!ModKey.TryFromFileName(name, out var key))
            {
                data.Issues.Add($"load order entry '{name}' is not a valid plugin file name");
                continue;
            }
            bool enabled = implicits.Contains(key) || mo2.EnabledPlugins.Contains(name);
            var rec = new PluginRecord { Index = -1, Name = name, Enabled = enabled, OriginKind = KindOf(name) };
            data.Plugins.Add(rec);
            if (!enabled) continue;

            var src = mo2.ResolveFile(name, out var shadowed);
            if (src is null)
            {
                data.Issues.Add($"enabled plugin '{name}' not found in overwrite, any enabled mod or game Data");
                continue;
            }
            rec.Index = index++;
            rec.Path = src.Path;
            rec.Source = src.Name;
            rec.SourceKind = src.Kind;
            rec.AlsoIn = shadowed.Select(s => s.Name).ToArray();
            sourceByPlugin[name] = src.Name;
            modPaths.Add(new ModPath(key, src.Path));
        }
        log($"load order: {modPaths.Count} plugins to load ({data.Plugins.Count(p => !p.Enabled)} disabled)");

        // ---- 2. load read-only
        // Overlays are built from the MO2-resolved paths, so the load order never needs a merged Data folder.
        using var loadOrder = new LoadOrder<IModListing<ISkyrimModGetter>>(disposeItems: true);
        foreach (var mp in modPaths)
            loadOrder.Add(new ModListing<ISkyrimModGetter>(SkyrimMod.CreateFromBinaryOverlay(mp, SkyrimRelease.SkyrimSE), enabled: true));
        var cache = loadOrder.ToImmutableLinkCache();
        foreach (var listing in loadOrder.ListedOrder)
        {
            var rec = data.Plugins.First(p => p.Name.Equals(listing.ModKey.FileName, StringComparison.OrdinalIgnoreCase));
            rec.Loaded = listing.Mod is not null;
            if (listing.Mod is { } m)
                rec.Masters = m.ModHeader.MasterReferences.Select(x => x.Master.FileName.String).ToArray();
        }

        // ---- 3. per-plugin door tallies (own records and overrides, winning or not)
        foreach (var listing in loadOrder.ListedOrder)
        {
            if (listing.Mod is null) continue;
            int added = 0, edited = 0;
            foreach (var po in listing.Mod.EnumerateMajorRecords<IPlacedObjectGetter>())
            {
                if (po.TeleportDestination is null) continue;
                if (po.FormKey.ModKey == listing.ModKey) added++; else edited++;
            }
            data.PluginTallies[listing.ModKey.FileName] = new PluginDoorTally { Added = added, Edited = edited };
        }
        log($"tallied doors in {data.PluginTallies.Count} plugins");

        // ---- 4. winning door references
        // Worldspace persistent cells hold refs that are not tied to a grid square: collect their keys from every plugin.
        var persistentCells = new HashSet<FormKey>();
        foreach (var listing in loadOrder.ListedOrder)
            if (listing.Mod is { } wm)
                foreach (var w in wm.Worldspaces)
                    if (w.TopCell is { } top) persistentCells.Add(top.FormKey);

        var cellCache = new Dictionary<FormKey, CellInfo>();
        var baseCache = new Dictionary<FormKey, BaseObject>();

        CellInfo CellOf(FormKey key)
        {
            if (cellCache.TryGetValue(key, out var ci)) return ci;
            if (cache.TryResolve<ICellGetter>(key, out var cell))
            {
                bool interior = cell.Flags.HasFlag(Cell.Flag.IsInteriorCell);
                int[]? grid = cell.Grid is { } g ? new[] { (int)g.Point.X, (int)g.Point.Y } : null;
                ci = new CellInfo(interior, grid);
            }
            else ci = new CellInfo(false, null);
            return cellCache[key] = ci;
        }

        Place PlaceOf(IModContext ctx, P3Float? pos)
        {
            ctx.TryGetParent<ICellGetter>(out var cell);
            ctx.TryGetParent<IWorldspaceGetter>(out var ws);
            var place = new Place { CellId = cell is null ? null : Id(cell.FormKey) };
            var ci = cell is null ? null : CellOf(cell.FormKey);
            if (ci is { Interior: true })
            {
                place.Kind = SpaceKind.Interior;
                place.SpaceId = $"{SpaceKind.Interior}:{Id(cell!.FormKey)}";
                return place;
            }
            place.Kind = SpaceKind.Worldspace;
            place.SpaceId = $"{SpaceKind.Worldspace}:{(ws is null ? "unknown" : Id(ws.FormKey))}";
            if (ci?.Grid is { } grid && cell is { } c && !persistentCells.Contains(c.FormKey))
            {
                place.Grid = grid;
                place.GridSource = "cell";
            }
            else if (pos is { } p)
            {
                place.Grid = new[] { (int)MathF.Floor(p.X / CellSize), (int)MathF.Floor(p.Y / CellSize) };
                place.GridSource = "position";
            }
            return place;
        }

        BaseObject BaseOf(IFormLinkNullableGetter<IPlaceableObjectGetter> link)
        {
            if (link.IsNull) return new BaseObject();
            var fk = link.FormKey;
            if (baseCache.TryGetValue(fk, out var b)) return b;
            b = new BaseObject { Id = Id(fk) };
            if (cache.TryResolve<ISkyrimMajorRecordGetter>(fk, out var rec))
            {
                b.Type = rec.Registration.Name;
                b.EditorId = rec.EditorID;
                if (rec is IModeledGetter modeled) b.Model = modeled.Model?.File.GivenPath;
                if (rec is IDoorGetter door)
                {
                    b.Scripts = ScriptNames(door.VirtualMachineAdapter);
                    b.Flags = FlagNames(door.Flags);
                    b.MinimalUse = door.Flags.HasFlag(Door.Flag.MinimalUse);
                    b.Automatic = door.Flags.HasFlag(Door.Flag.Automatic);
                }
                else if (rec is IScriptedGetter scripted) b.Scripts = ScriptNames(scripted.VirtualMachineAdapter);
            }
            else b.Type = "missing";
            return baseCache[fk] = b;
        }

        var doorIds = new HashSet<string>();
        int seen = 0;
        foreach (var ctx in loadOrder.PriorityOrder.PlacedObject().WinningContextOverrides(cache))
        {
            seen++;
            var rec = ctx.Record;
            if (rec.TeleportDestination is not { } tel) continue;

            var pos = rec.Placement?.Position;
            var rot = rec.Placement?.Rotation;
            var baseObj = BaseOf(rec.Base);
            var scripts = ScriptNames(rec.VirtualMachineAdapter);
            string plugin = ctx.ModKey.FileName;
            string origin = rec.FormKey.ModKey.FileName;

            var door = new DoorRecord
            {
                Id = Id(rec.FormKey),
                Plugin = plugin,
                PluginSource = sourceByPlugin.GetValueOrDefault(plugin),
                Origin = origin,
                OriginKind = KindOf(origin),
                Base = baseObj,
                At = PlaceOf(ctx, pos),
                Pos = Vec(pos),
                Rot = Vec(rot),
                Dest = new Destination
                {
                    DoorId = tel.Door.IsNull ? null : Id(tel.Door.FormKey),
                    LandPos = Vec(tel.Position),
                    LandRot = Vec(tel.Rotation),
                    Flags = FlagNames(tel.Flags),
                },
                Scripts = scripts,
                HasScripts = scripts.Length > 0,
                BaseHasScripts = baseObj.Scripts.Length > 0,
                MinimalUse = baseObj.MinimalUse,
                Automatic = baseObj.Automatic,
                InitiallyDisabled = rec.SkyrimMajorRecordFlags.HasFlag(SkyrimMajorRecord.SkyrimMajorRecordFlag.InitiallyDisabled),
                OpenByDefault = rec.IsOpenByDefault,
            };
            if (rec.Lock is { } lk)
            {
                door.Lock = new LockInfo
                {
                    Level = (int)lk.Level & 0xFF,
                    LevelName = LockLevelName((int)lk.Level & 0xFF),
                    KeyId = lk.Key.IsNull ? null : Id(lk.Key.FormKey),
                    Flags = LockFlagNames((int)lk.Flags),
                };
            }
            data.Doors.Add(door);
            doorIds.Add(door.Id);
        }
        log($"scanned {seen:N0} winning placed objects, {data.Doors.Count:N0} with teleport data");

        // ---- 5. destinations that are not teleport doors (or do not exist)
        foreach (var dest in data.Doors.Select(d => d.Dest.DoorId).Where(i => i is not null).Distinct())
        {
            if (doorIds.Contains(dest!)) continue;
            var fk = ParseId(dest!);
            var ep = new Endpoint { Id = dest! };
            if (fk is not null && cache.TryResolveContext<IPlacedObject, IPlacedObjectGetter>(fk.Value, out var ctx))
            {
                var pos = ctx.Record.Placement?.Position;
                ep.Exists = true;
                ep.Plugin = ctx.ModKey.FileName;
                ep.BaseType = BaseOf(ctx.Record.Base).Type;
                ep.At = PlaceOf(ctx, pos);
                ep.Pos = Vec(pos);
                ep.Rot = Vec(ctx.Record.Placement?.Rotation);
            }
            else if (fk is not null && cache.TryResolve<ISkyrimMajorRecordGetter>(fk.Value, out var other))
            {
                ep.Exists = true;
                ep.BaseType = other.Registration.Name;
            }
            data.Endpoints.Add(ep);
        }

        // ---- 6. spaces: interior cells and worldspaces (winning records)
        foreach (var ctx in loadOrder.PriorityOrder.Cell().WinningContextOverrides(cache))
        {
            var cell = ctx.Record;
            if (!cell.Flags.HasFlag(Cell.Flag.IsInteriorCell)) { data.ExteriorCellCount++; continue; }
            string origin = cell.FormKey.ModKey.FileName;
            data.Spaces.Add(new SpaceRecord
            {
                Id = $"{SpaceKind.Interior}:{Id(cell.FormKey)}",
                Kind = SpaceKind.Interior,
                EditorId = cell.EditorID,
                Name = cell.Name?.String,
                Plugin = ctx.ModKey.FileName,
                Origin = origin,
                OriginKind = KindOf(origin),
                Flags = FlagNames(cell.Flags),
            });
        }
        foreach (var wsWinner in loadOrder.PriorityOrder.Worldspace().WinningOverrides())
        {
            var ws = wsWinner;
            var lastEditor = cache.TryResolveContext<IWorldspace, IWorldspaceGetter>(ws.FormKey, out var wsCtx) ? wsCtx.ModKey : ws.FormKey.ModKey;
            string origin = ws.FormKey.ModKey.FileName;
            var sp = new SpaceRecord
            {
                Id = $"{SpaceKind.Worldspace}:{Id(ws.FormKey)}",
                Kind = SpaceKind.Worldspace,
                EditorId = ws.EditorID,
                Name = ws.Name?.String,
                Plugin = lastEditor.FileName,
                Origin = origin,
                OriginKind = KindOf(origin),
                Flags = FlagNames(ws.Flags),
            };
            if (ws.Parent is { } par && !par.Worldspace.IsNull)
            {
                sp.ParentId = $"{SpaceKind.Worldspace}:{Id(par.Worldspace.FormKey)}";
                sp.ParentFlags = FlagNames(par.Flags);
            }
            data.Spaces.Add(sp);
        }
        log($"spaces: {data.Spaces.Count(s => s.Kind == SpaceKind.Interior):N0} interior cells, " +
            $"{data.Spaces.Count(s => s.Kind == SpaceKind.Worldspace)} worldspaces, {data.ExteriorCellCount:N0} exterior cells");
        return data;
    }

    // ---- helpers

    private static string Id(FormKey key) => $"{key.ModKey.FileName}:{key.ID:X6}";

    private static FormKey? ParseId(string id)
    {
        int i = id.LastIndexOf(':');
        if (i <= 0) return null;
        if (!ModKey.TryFromFileName(id[..i], out var mk)) return null;
        if (!uint.TryParse(id[(i + 1)..], System.Globalization.NumberStyles.HexNumber, null, out var raw)) return null;
        return new FormKey(mk, raw);
    }

    private static string LockLevelName(int level) => level switch
    {
        0 => "VeryEasy", 1 => "Novice", 25 => "Apprentice", 50 => "Adept", 75 => "Expert", 100 => "Master",
        255 => "RequiresKey", _ => level.ToString(),
    };

    private static string[] LockFlagNames(int flags)
    {
        var names = new List<string>();
        if ((flags & 4) != 0) names.Add("LeveledLock");
        if ((flags & ~4) != 0) names.Add($"0x{flags & ~4:X}");
        return names.ToArray();
    }

    private static float[] Vec(P3Float? v) => v is { } p ? new[] { p.X, p.Y, p.Z } : new float[3];

    private static string[] ScriptNames(IVirtualMachineAdapterGetter? vmad) =>
        vmad is null ? Array.Empty<string>() : vmad.Scripts.Select(s => s.Name).ToArray();

    private static string[] FlagNames<TEnum>(TEnum value) where TEnum : struct, Enum =>
        Convert.ToInt64(value) == 0 ? Array.Empty<string>()
            : value.ToString().Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);

    /// <summary>Creation Club content: names listed in Skyrim.ccc, the MO2 "Creation Club:" entries, and the cc*sse### pattern.</summary>
    private static HashSet<string> LoadCreationNames(Mo2Instance mo2)
    {
        var set = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var ccc = Path.Combine(Path.GetDirectoryName(mo2.GameDataDir)!, "Skyrim.ccc");
        if (File.Exists(ccc))
            foreach (var l in File.ReadAllLines(ccc)) if (l.Trim().Length > 0) set.Add(l.Trim());
        foreach (var e in mo2.UnmanagedEntries)
        {
            const string prefix = "Creation Club:";
            if (!e.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) continue;
            var stem = e[prefix.Length..].Trim();
            foreach (var ext in new[] { ".esm", ".esl", ".esp" }) set.Add(stem + ext);
        }
        foreach (var name in mo2.LoadOrder)
            if (System.Text.RegularExpressions.Regex.IsMatch(name, @"^cc[a-z]{2,}sse\d{3}-", System.Text.RegularExpressions.RegexOptions.IgnoreCase))
                set.Add(name);
        return set;
    }
}
