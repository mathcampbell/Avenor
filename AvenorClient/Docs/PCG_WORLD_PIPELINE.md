# Avenor PCG World Pipeline

## Ownership

- `AProceduralTerrainGenerator` is the deterministic world definition. The
  legacy class name remains so existing level references continue to load.
- One native `ALandscape`, created and saved in the level, owns ground,
  collision, Landscape materials, LOD and navigation.
- Native Water Body actors own visible rivers and lakes. They use the
  Landscape `Water` edit layer.
- PCG graphs read the world definition and create/bake terrain patches, water
  actors, vegetation and infrastructure.
- `ASpineGenerator::GuideSpline` is the authoritative Spine route.
- `AParcelGenerator` owns parcel policy/availability data.

Regeneration must never create a second Landscape.

## One-time level migration

1. Delete every actor labelled `Avenor Generated Landscape`,
   `Avenor Generated Water Zone`, `Avenor River ...` or `Avenor Lake ...`
   left by the legacy generator.
2. Keep one `ProceduralTerrainGenerator` and one `SpineGenerator`.
3. Create one native Landscape in Landscape mode, centred on the level datum.
   Enable Edit Layers and name the base layer `Terrain`.
4. Add a layer named `Water` above `Terrain`.
5. Assign that Landscape to `Native Landscape` on the world definition.
6. Add one Water Zone. Water Bodies may be authored manually initially or
   emitted by the assigned Water PCG graph.
7. Assign PCG graphs to the world definition:
   - `Terrain Graph`: landscape patch/stamp orchestration.
   - `Water Graph`: rivers/lakes derived from `Watercourses`.
   - `Vegetation Graph`: biome and exclusion-rule scattering.
   - `Infrastructure Graph`: world-level roads and development.
8. Assign the Spine-specific graph to
   `SpineGenerator.Infrastructure Graph`.

## Graph contract

All graphs use the same fixed `WorldSeed`. Generation is an editor/build step,
not per-player runtime randomness.

### Terrain

- Read the Spine spline and distance from it.
- Keep the nearest roughly 1 km on either side broadly rolling and suitable
  for development, not mathematically flat.
- Blend progressively into hills.
- Allow rare mountain masks only at distant ranges.
- Write non-destructively through Landscape patches/stamps to the `Terrain`
  layer.

### Water

- Read `Watercourses` from the world-definition actor.
- Each entry contains a source lake, connected river points, surface heights
  and width.
- Create or update native Water Body Lake/River actors.
- Let Water own channel deformation on the `Water` edit layer.
- Do not pre-carve the same channel into the base Terrain layer.

### Vegetation

- Sample the native Landscape.
- Exclude the Spine development corridor, roads, parcel development masks and
  water surfaces.
- Select biome by slope, height, moisture/distance-to-water and deterministic
  regional masks.
- Use partitioned PCG and hierarchical instancing.

### Spine infrastructure

- Sample `GuideSpline`.
- Generate guideway meshes along the spline.
- Place supports by distance with exclusions for rivers, roads and stations.
- Place stations at the configured station interval.
- Emit signs, lighting and street furniture as separate density/LOD groups.

## Editor buttons

- `Regenerate` rebuilds deterministic rule data and runs every assigned graph.
- Individual PCG categories can be regenerated independently.
- `Clear Generated Terrain` is retained for old level instances, but now only
  clears PCG output and cached rules. It cannot delete the Landscape or Water
  actors.
