using Census.Model;

namespace Census.Analysis;

public sealed class GraphReport
{
    public SpaceCounts Spaces { get; set; } = new();
    public DoorCounts Doors { get; set; } = new();
    public List<Edge> Edges { get; set; } = new();
    public List<ChildWorldspace> ChildWorldspaces { get; set; } = new();
    public List<GatePair> GatePairs { get; set; } = new();
    public List<BrokenLink> BrokenLinks { get; set; } = new();
    public List<PluginRank> TopPlugins { get; set; } = new();
    public float SharedThreshold { get; set; }
}

public sealed class SpaceCounts
{
    public int Interiors { get; set; }
    public int InteriorsWithDoors { get; set; }
    public int Worldspaces { get; set; }
    public int WorldspacesWithDoors { get; set; }
    public int ChildWorldspaces { get; set; }
    public int ExteriorCells { get; set; }
    /// <summary>Space ids doors point into that are not in the spaces list.</summary>
    public int UnlistedSpaces { get; set; }
}

public sealed class DoorCounts
{
    public int Total { get; set; }
    /// <summary>Door references by kind of (this space, destination space).</summary>
    public Dictionary<string, int> ByKind { get; set; } = new();
    /// <summary>Unordered links by kind: a reciprocal pair counts once, a one-way or broken door counts once.</summary>
    public Dictionary<string, int> LinksByKind { get; set; } = new();
    public Dictionary<string, int> ByStatus { get; set; } = new();
    public int WithScripts { get; set; }
    public int BaseWithScripts { get; set; }
    public int Locked { get; set; }
    public int MinimalUse { get; set; }
    public int Automatic { get; set; }
    public Dictionary<string, int> ByOrigin { get; set; } = new();
}

public sealed class Edge
{
    public string A { get; set; } = "";
    public string B { get; set; } = "";
    public string Kind { get; set; } = "";
    /// <summary>Door references on either side of this space pair.</summary>
    public int Doors { get; set; }
}

public sealed class ChildWorldspace
{
    public string Id { get; set; } = "";
    public string? EditorId { get; set; }
    public string? Name { get; set; }
    public string Plugin { get; set; } = "";
    public string ParentId { get; set; } = "";
    public string[] ParentFlags { get; set; } = Array.Empty<string>();
    public int GatePairs { get; set; }
    public int SharedPairs { get; set; }
    /// <summary>shared (all gate pairs within threshold) | partial | separate | no-gates</summary>
    public string Coordinates { get; set; } = "no-gates";
    public float? MedianDistance { get; set; }
    public float? MaxDistance { get; set; }
}

public sealed class GatePair
{
    public string DoorA { get; set; } = "";
    public string DoorB { get; set; } = "";
    public string SpaceA { get; set; } = "";
    public string SpaceB { get; set; } = "";
    public bool Reciprocal { get; set; }
    /// <summary>Horizontal distance between the two door positions, game units.</summary>
    public float Distance { get; set; }
    public float DeltaZ { get; set; }
    public bool Shared { get; set; }
}

public sealed class BrokenLink
{
    public string Door { get; set; } = "";
    public string Status { get; set; } = "";
    public string Plugin { get; set; } = "";
    public string Space { get; set; } = "";
    public string? Destination { get; set; }
    public string? DestinationSpace { get; set; }
}

public sealed class PluginRank
{
    public string Plugin { get; set; } = "";
    public string? Source { get; set; }
    public int Added { get; set; }
    public int Edited { get; set; }
    /// <summary>Teleport doors whose winning record is in this plugin.</summary>
    public int Winning { get; set; }
}

/// <summary>Engine-agnostic analysis over <see cref="CensusData"/>: link resolution, kinds, graph, gates.</summary>
public static class SpaceGraph
{
    public static GraphReport Build(CensusData data, float sharedThreshold, int topPlugins = 20)
    {
        var report = new GraphReport { SharedThreshold = sharedThreshold };
        var doors = data.Doors.ToDictionary(d => d.Id);
        var endpoints = data.Endpoints.ToDictionary(e => e.Id);
        var spaces = data.Spaces.ToDictionary(s => s.Id);

        // ---- resolve destinations, status and kind
        foreach (var d in data.Doors)
        {
            var destId = d.Dest.DoorId;
            if (destId is null) d.Link = LinkStatus.DestNull;
            else if (doors.TryGetValue(destId, out var other))
            {
                d.Dest.At = other.At;
                d.Dest.DoorPos = other.Pos;
                d.Dest.DoorRot = other.Rot;
                d.Dest.DoorPlugin = other.Plugin;
                d.Link = other.Dest.DoorId == d.Id ? LinkStatus.Ok : LinkStatus.OneWay;
            }
            else if (endpoints.TryGetValue(destId, out var ep) && ep.Exists)
            {
                d.Dest.At = ep.At;
                d.Dest.DoorPos = ep.Pos;
                d.Dest.DoorRot = ep.Rot;
                d.Dest.DoorPlugin = ep.Plugin;
                d.Link = LinkStatus.DestNotTeleport;
            }
            else d.Link = LinkStatus.DestMissing;

            d.Kind = KindOf(d.At.Kind, d.Dest.At?.Kind);
        }

        // ---- door counts
        var c = report.Doors;
        c.Total = data.Doors.Count;
        foreach (var d in data.Doors)
        {
            Inc(c.ByKind, d.Kind);
            Inc(c.ByStatus, d.Link);
            Inc(c.ByOrigin, d.OriginKind);
            if (d.HasScripts) c.WithScripts++;
            if (d.BaseHasScripts) c.BaseWithScripts++;
            if (d.Lock is not null) c.Locked++;
            if (d.MinimalUse) c.MinimalUse++;
            if (d.Automatic) c.Automatic++;
        }

        // ---- links: unordered, reciprocal pairs once
        var linkKeys = new HashSet<string>();
        foreach (var d in data.Doors)
        {
            string key = d.Link == LinkStatus.Ok && d.Dest.DoorId is not null
                ? string.CompareOrdinal(d.Id, d.Dest.DoorId) < 0 ? d.Id + "|" + d.Dest.DoorId : d.Dest.DoorId + "|" + d.Id
                : d.Id + "|" + d.Dest.DoorId;
            if (linkKeys.Add(key)) Inc(c.LinksByKind, d.Kind);
        }

        // ---- spaces
        var doorsPerSpace = data.Doors.GroupBy(d => d.At.SpaceId).ToDictionary(g => g.Key, g => g.Count());
        foreach (var s in data.Spaces) s.Doors = doorsPerSpace.GetValueOrDefault(s.Id);
        var sc = report.Spaces;
        sc.Interiors = data.Spaces.Count(s => s.Kind == SpaceKind.Interior);
        sc.InteriorsWithDoors = data.Spaces.Count(s => s.Kind == SpaceKind.Interior && s.Doors > 0);
        sc.Worldspaces = data.Spaces.Count(s => s.Kind == SpaceKind.Worldspace);
        sc.WorldspacesWithDoors = data.Spaces.Count(s => s.Kind == SpaceKind.Worldspace && s.Doors > 0);
        sc.ChildWorldspaces = data.Spaces.Count(s => s.ParentId is not null);
        sc.ExteriorCells = data.ExteriorCellCount;
        var referenced = new HashSet<string>(data.Doors.Select(d => d.At.SpaceId));
        foreach (var d in data.Doors) if (d.Dest.At is { } at) referenced.Add(at.SpaceId);
        sc.UnlistedSpaces = referenced.Count(id => !spaces.ContainsKey(id));

        // ---- space-pair edges
        var edgeMap = new Dictionary<string, Edge>();
        foreach (var d in data.Doors)
        {
            if (d.Dest.At is not { } at) continue;
            var (a, b) = string.CompareOrdinal(d.At.SpaceId, at.SpaceId) <= 0 ? (d.At.SpaceId, at.SpaceId) : (at.SpaceId, d.At.SpaceId);
            string key = a + "|" + b;
            if (!edgeMap.TryGetValue(key, out var e))
                edgeMap[key] = e = new Edge { A = a, B = b, Kind = d.Kind };
            e.Doors++;
        }
        report.Edges = edgeMap.Values.OrderByDescending(e => e.Doors).ThenBy(e => e.A).ThenBy(e => e.B).ToList();

        // ---- gates (exterior <-> exterior)
        var gateSeen = new HashSet<string>();
        foreach (var d in data.Doors.Where(d => d.Kind == DoorKind.ExteriorExterior && d.Dest.DoorPos is not null))
        {
            string key = string.CompareOrdinal(d.Id, d.Dest.DoorId) < 0 ? d.Id + "|" + d.Dest.DoorId : d.Dest.DoorId + "|" + d.Id;
            if (!gateSeen.Add(key)) continue;
            var p = d.Pos; var q = d.Dest.DoorPos!;
            float dist = MathF.Sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]));
            report.GatePairs.Add(new GatePair
            {
                DoorA = d.Id, DoorB = d.Dest.DoorId!,
                SpaceA = d.At.SpaceId, SpaceB = d.Dest.At!.SpaceId,
                Reciprocal = d.Link == LinkStatus.Ok,
                Distance = dist, DeltaZ = q[2] - p[2], Shared = dist <= sharedThreshold,
            });
        }

        // ---- child worldspaces and coordinate sharing
        foreach (var ws in data.Spaces.Where(s => s.Kind == SpaceKind.Worldspace && s.ParentId is not null))
        {
            var pairs = report.GatePairs.Where(g => g.SpaceA == ws.Id || g.SpaceB == ws.Id).ToList();
            var dists = pairs.Select(g => g.Distance).Order().ToList();
            var cw = new ChildWorldspace
            {
                Id = ws.Id, EditorId = ws.EditorId, Name = ws.Name, Plugin = ws.Plugin,
                ParentId = ws.ParentId!, ParentFlags = ws.ParentFlags ?? Array.Empty<string>(),
                GatePairs = pairs.Count, SharedPairs = pairs.Count(g => g.Shared),
            };
            if (pairs.Count > 0)
            {
                cw.Coordinates = cw.SharedPairs == pairs.Count ? "shared" : cw.SharedPairs == 0 ? "separate" : "partial";
                cw.MedianDistance = dists[dists.Count / 2];
                cw.MaxDistance = dists[^1];
            }
            report.ChildWorldspaces.Add(cw);
        }
        report.ChildWorldspaces = report.ChildWorldspaces
            .OrderByDescending(w => w.GatePairs).ThenBy(w => w.Id).ToList();

        // ---- broken / one-way
        foreach (var d in data.Doors.Where(d => d.Link != LinkStatus.Ok))
            report.BrokenLinks.Add(new BrokenLink
            {
                Door = d.Id, Status = d.Link, Plugin = d.Plugin, Space = d.At.SpaceId,
                Destination = d.Dest.DoorId, DestinationSpace = d.Dest.At?.SpaceId,
            });

        // ---- plugins by doors added or edited
        var winning = data.Doors.GroupBy(d => d.Plugin, StringComparer.OrdinalIgnoreCase).ToDictionary(g => g.Key, g => g.Count(), StringComparer.OrdinalIgnoreCase);
        var sources = data.Plugins.ToDictionary(p => p.Name, p => p.Source, StringComparer.OrdinalIgnoreCase);
        report.TopPlugins = data.PluginTallies
            .Where(kv => kv.Value.Added + kv.Value.Edited > 0)
            .OrderByDescending(kv => kv.Value.Added + kv.Value.Edited).ThenBy(kv => kv.Key)
            .Take(topPlugins)
            .Select(kv => new PluginRank
            {
                Plugin = kv.Key, Source = sources.GetValueOrDefault(kv.Key),
                Added = kv.Value.Added, Edited = kv.Value.Edited, Winning = winning.GetValueOrDefault(kv.Key),
            }).ToList();

        return report;
    }

    private static string KindOf(string from, string? to)
    {
        if (to is null) return DoorKind.Unknown;
        bool fi = from == SpaceKind.Interior, ti = to == SpaceKind.Interior;
        return fi && ti ? DoorKind.InteriorInterior : fi || ti ? DoorKind.InteriorExterior : DoorKind.ExteriorExterior;
    }

    private static void Inc(Dictionary<string, int> map, string key) => map[key] = map.GetValueOrDefault(key) + 1;
}
