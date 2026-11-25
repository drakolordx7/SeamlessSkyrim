using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Census.Analysis;
using Census.Model;

namespace Census.Output;

public sealed class Meta
{
    public string Schema { get; set; } = "door-census/1";
    public string Game { get; set; } = "";
    public string Generated { get; set; } = "";
    public string Instance { get; set; } = "";
    public string Profile { get; set; } = "";
    public string GameData { get; set; } = "";
}

public sealed class CensusDocument
{
    public Meta Meta { get; set; } = new();
    public List<PluginRecord> Plugins { get; set; } = new();
    public List<string> Issues { get; set; } = new();
    public List<SpaceRecord> Spaces { get; set; } = new();
    public List<DoorRecord> Doors { get; set; } = new();
    /// <summary>Destination references that are not teleport doors (or do not exist).</summary>
    public List<Endpoint> Endpoints { get; set; } = new();
    public GraphReport Graph { get; set; } = new();
}

public static class ReportWriter
{
    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DictionaryKeyPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        WriteIndented = false,
    };

    public static void Write(string outDir, CensusDocument doc)
    {
        Directory.CreateDirectory(outDir);
        File.WriteAllText(Path.Combine(outDir, "census.json"), JsonSerializer.Serialize(doc, Options));
        File.WriteAllText(Path.Combine(outDir, "summary.md"), Summary(doc));
    }

    public static string Summary(CensusDocument doc)
    {
        var g = doc.Graph;
        var sb = new StringBuilder();
        sb.AppendLine($"# Door census: {doc.Meta.Game}");
        sb.AppendLine();
        sb.AppendLine($"Generated {doc.Meta.Generated}. Profile `{doc.Meta.Profile}` of `{doc.Meta.Instance}`.");
        int loaded = doc.Plugins.Count(p => p.Loaded);
        sb.AppendLine($"{loaded} plugins loaded read-only ({doc.Plugins.Count(p => !p.Enabled)} listed but disabled).");
        sb.AppendLine();

        sb.AppendLine("## Spaces");
        sb.AppendLine();
        sb.AppendLine($"- Interior cells: {g.Spaces.Interiors:N0} ({g.Spaces.InteriorsWithDoors:N0} with a teleport door)");
        sb.AppendLine($"- Worldspaces: {g.Spaces.Worldspaces:N0} ({g.Spaces.WorldspacesWithDoors:N0} with a teleport door), " +
                      $"{g.Spaces.ChildWorldspaces} with a parent worldspace");
        sb.AppendLine($"- Exterior cells: {g.Spaces.ExteriorCells:N0}");
        if (g.Spaces.UnlistedSpaces > 0) sb.AppendLine($"- Space ids used by doors but missing from the spaces list: {g.Spaces.UnlistedSpaces}");
        sb.AppendLine();

        sb.AppendLine("## Doors");
        sb.AppendLine();
        sb.AppendLine($"{g.Doors.Total:N0} placed teleport doors. Locked: {g.Doors.Locked:N0}. Reference scripts: {g.Doors.WithScripts:N0}. " +
                      $"Base door scripts: {g.Doors.BaseWithScripts:N0}. Minimal-use: {g.Doors.MinimalUse:N0}. Automatic: {g.Doors.Automatic:N0}.");
        sb.AppendLine();
        sb.AppendLine("| Kind | Door refs | Links |");
        sb.AppendLine("|---|---:|---:|");
        foreach (var k in g.Doors.ByKind.Keys.Union(g.Doors.LinksByKind.Keys).Order())
            sb.AppendLine($"| {k} | {g.Doors.ByKind.GetValueOrDefault(k):N0} | {g.Doors.LinksByKind.GetValueOrDefault(k):N0} |");
        sb.AppendLine();
        sb.AppendLine("Exterior-exterior doors are gates between worldspaces (or two parts of one). By origin: " +
                      string.Join(", ", g.Doors.ByOrigin.OrderBy(kv => kv.Key).Select(kv => $"{kv.Key} {kv.Value:N0}")) + ".");
        sb.AppendLine();

        sb.AppendLine("## Link health");
        sb.AppendLine();
        foreach (var kv in g.Doors.ByStatus.OrderByDescending(kv => kv.Value))
            sb.AppendLine($"- {kv.Key}: {kv.Value:N0}");
        var worst = g.BrokenLinks.GroupBy(b => b.Plugin).OrderByDescending(x => x.Count()).Take(10).ToList();
        if (worst.Count > 0)
        {
            sb.AppendLine();
            sb.AppendLine("Plugins with most one-way/broken doors: " + string.Join(", ", worst.Select(x => $"{x.Key} ({x.Count()})")) + ".");
        }
        sb.AppendLine();

        sb.AppendLine("## Child worldspaces");
        sb.AppendLine();
        if (g.ChildWorldspaces.Count == 0) sb.AppendLine("None.");
        else
        {
            sb.AppendLine($"Gate pair counts as coordinate-shared when the two door positions are within {g.SharedThreshold:0} units (horizontal).");
            sb.AppendLine();
            sb.AppendLine("| Worldspace | Parent | Gate pairs | Shared | Coordinates | Median / max dist |");
            sb.AppendLine("|---|---|---:|---:|---|---|");
            foreach (var w in g.ChildWorldspaces)
                sb.AppendLine($"| {w.EditorId ?? w.Id} | {w.ParentId} | {w.GatePairs} | {w.SharedPairs} | {w.Coordinates} | " +
                              (w.MedianDistance is null ? "-" : $"{w.MedianDistance:0} / {w.MaxDistance:0}") + " |");
        }
        sb.AppendLine();

        sb.AppendLine("## Top plugins by teleport doors added or edited");
        sb.AppendLine();
        sb.AppendLine("| # | Plugin | Added | Edited | Winning | Source |");
        sb.AppendLine("|--:|---|--:|--:|--:|---|");
        int i = 1;
        foreach (var p in g.TopPlugins)
            sb.AppendLine($"| {i++} | {p.Plugin} | {p.Added:N0} | {p.Edited:N0} | {p.Winning:N0} | {p.Source} |");
        sb.AppendLine();

        sb.AppendLine("## Unresolved");
        sb.AppendLine();
        if (doc.Issues.Count == 0) sb.AppendLine("Nothing.");
        else foreach (var s in doc.Issues) sb.AppendLine($"- {s}");
        return sb.ToString();
    }
}
