# Avenor Spine PCG setup

## What C++ now supplies

`ASpineGenerator` no longer constructs a cube motorway and parcel field with
instanced-mesh components. It owns the editable alignment and supplies PCG with:

- `GuideSpline`, tagged `Avenor.Spine.Center`;
- `HighwayPositiveSpline`, tagged `Avenor.Highway.Positive`;
- `HighwayNegativeSpline`, tagged `Avenor.Highway.Negative`;
- `MonorailPositiveSpline`, tagged `Avenor.Monorail.Positive`;
- `MonorailNegativeSpline`, tagged `Avenor.Monorail.Negative`;
- `StationRecords`;
- `BlockRecords`;
- `GreyboxSegments`;
- `GuidewayPlacements`;
- `MonorailPierPlacements`;
- `MonorailSupportPlacements`.

The station and block arrays contain stable IDs and world transforms. The
greybox array contains disposable box transforms for the complete highway,
twin guideways and local street grid. These arrays are flat reflected structs,
so PCG 5.8 can read them with stock `Get Actor Property` nodes.

Generated arrays are transient and rebuilding is deliberately button-driven.
Moving, selecting or editing the actor does not rebuild or serialize the
derived records into the level. The safe prototype defaults are one district,
one development row on each side and non-partitioned generation.

The actor also owns `SpineTerrainCorridor`, a narrow Mesh Terrain modifier. It
does not create another terrain mesh. **Rebuild Terrain Alignment** samples the
existing Mesh Terrain every 25 m, smooths the result over 250 m, enforces the
configured maximum grade, and blends the existing terrain to that shared road
datum. The same solved profile drives roads, stations, guideways and piers. The
small solved profile is saved with the level; the much larger disposable PCG
placement arrays remain transient.

The binding district arithmetic is:

| Element | Length |
|---|---:|
| Station-to-station chainage | 102,400 cm / 1,024 m |
| Nine clear block bays | 90,000 cm / 900 m |
| One station cross-street | 2,000 cm / 20 m |
| Eight resolved local streets | 8 × 1,300 cm / 13 m |
| Total | 102,400 cm / 1,024 m |

Roads are outside the guaranteed 100 × 100 m block dimensions. The configured
public-realm bay is tagged `PublicRealm`; the other eight bays are tagged
`Development`.

## Create the first functional graph

Create `/Game/Avenor/PCG/Spine/PCG_Spine_Master`.

### 1. Greybox infrastructure branch

1. Add **Get Actor Property**.
2. Set Actor Selection to **Self** and Property Name to
   `GreyboxSegments`. Self is the `SpineGenerator` that owns this PCG
   component.
3. Add **Attribute Set To Point** and map the struct's `Transform` attribute to
   the point property `$Transform`.
4. Add a **Static Mesh Spawner**, add one Mesh Entry, and assign
   `/Engine/BasicShapes/Cube`. The Debug Point Mesh does not count as a mesh
   entry.
5. Preserve the incoming point transform and scale. Do not randomise rotation
   or scale: each point already represents an exact road or guideway span.
6. For clearer colours, split on the `Kind` attribute before the spawner:
   - `Highway`;
   - `LocalStreet`;
   - `StationStreet`.
7. Use one material per branch, then gather the outputs.

This branch is the replacement functional greybox. It is intentionally crude
in appearance, but its station cadence, independent guideways, clear block
dimensions and road reservations are real rather than illustrative.

### 2. Block branch

1. Add **Get Actor Property** for `BlockRecords`, selecting the same
   **Self** actor.
2. Convert the Attribute Set to points by mapping `Transform` to `$Transform`.
3. Split or filter on `ZoneRole` (`Development` or `PublicRealm`).
4. For the initial visual test, spawn a 100 × 100 × 0.2 m cube pad at each
   point. Set scale explicitly here because block transforms intentionally do
   not contain display scale.
5. Give public-realm pads a different material.

Do not turn these PCG points into live ownership records. PCG proposes the
layout; publishing a validated layout manifest to the world registry is a
separate future operation. After publication, the registry snapshot will be
the parcel authority and the graph will consume that versioned snapshot.

### 3. Station branch

1. Add **Get Actor Property** for `StationRecords`, selecting the same
   **Self** actor.
2. Convert the Attribute Set to points by mapping `Transform` to `$Transform`.
3. Initially use **Spawn Actor** to place a reusable
   `BP_Station_Standard_Greybox` made from the supplied station blockout.
4. Keep the station actor origin at road datum. The record's `PlatformDatum`
   is 930 cm and the v01 greybox already uses that datum.
5. Map `StationId` and `StationIndex` to variables on the station controller
   when that Blueprint exists.

The production station may later wrap a regular Level Instance, or use a
Packed Level Actor for a static shell plus a separate controller. PCG places
the station during editor generation; World Partition streams the baked actor.

### 4. Output and generation settings

1. Gather the three branches and connect them to **Output**.
2. Keep the Spine PCG component non-partitioned for the prototype.
3. Do not enable runtime generation.
4. Assign `PCG_Spine_Master` to `Infrastructure Graph` on `SpineGenerator`.
5. Click **Regenerate Infrastructure**. It rebuilds the transient layout data
   before running the graph; a separate rebuild is unnecessary.

## Terrain alignment workflow

1. Assign the level's existing Mesh Partition actor to **Mesh Partition
   Actor** on `SpineGenerator`.
2. Keep the initial terrain values at:
   - sampling: 2,500 cm / 25 m;
   - smoothing: 25,000 cm / 250 m;
   - maximum grade: 0.04 / 4%;
   - flat half-width: 2,700 cm / 27 m;
   - transition half-width: 12,000 cm / 120 m.
3. Use **Regenerate Complete Spine** after moving the guide spline or changing
   terrain. This solves the terrain corridor first and regenerates PCG from
   the same profile.
4. Use **Regenerate Infrastructure** for quick mesh/material/PCG iteration
   when neither the route nor terrain has changed.

The Spine modifier uses the last Mesh Partition priority layer at priority 5.
Native Avenor water modifiers use priority 10, so rivers remain able to cut
through the corridor and identify future bridge crossings rather than being
silently blocked by an embankment.

Partitioned generation and spatial culling belong to the later production
graph, after its bounds and World Partition behaviour have been tested. Do not
enable them for this first one-district verification graph.

## Production replacement graph

The box transforms are only the first verification layer. Production subgraphs
should read the tagged derived splines:

- sample the highway splines for modular carriageway meshes, markings and
  barriers;
- sample both monorail splines independently for guideway meshes;
- sample support points, then difference them against station, road and water
  footprints;
- use `BlockRecords` for block surfaces and the authoritative registry snapshot
  for parcel contents;
- use `StationRecords` to place authored station variants;
- run separate subgraphs for pavements, crossings, lights, signs, trees and
  other dressing.

Keep these as subgraphs under `PCG_Spine_Master`; do not build one unmaintainable
wall of nodes.

## Why the graph is not created in source code

PCG graph assets are editable Unreal `.uasset` files. Unreal editor scripting
and C++ can technically create and modify graph objects, but doing so depends
on editor-only, version-sensitive APIs and produces a binary asset anyway. It
would make a simple graph harder to inspect and maintain.

The durable division is therefore:

- C++ creates deterministic, testable spatial data and any genuinely custom
  PCG elements;
- the PCG editor owns graph topology and asset choices;
- the resulting `.uasset` is committed to source control.

Stock PCG 5.8 nodes are sufficient for this initial graph, so a custom PCG
element is unnecessary.
