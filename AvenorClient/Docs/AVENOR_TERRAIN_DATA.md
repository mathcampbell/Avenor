# Avenor baked terrain data

The authoritative Avenor geography is stored in a `UAvenorTerrainData`
primary data asset. Mesh Terrain sections, Water Body actors and refinement
modifiers are derived editor output.

## First bake

1. Select `AvenorStripTerrainGenerator` and assign its Mesh Partition actor.
2. Set the world, terrain, erosion, hydrology and water options.
3. Click **Generate and Bake Geography**.
4. If `Baked Terrain Data` is empty, the generator creates and saves
   `/Game/Avenor/Generated/DA_AvenorTerrainData_<actor name>` and assigns it.
5. Save the level so the generator's soft asset reference is retained.

This is the only production command that runs the full procedural landform,
erosion and hydrology calculation. It replaces the contents of the assigned
asset after generation succeeds.

## Later operations

- **Rebuild World from Baked Data** decompresses the asset and rebuilds Mesh
  Terrain, refinement modifiers and Water Body actors. It does not recalculate
  geography.
- **Regenerate Water from Baked Data** recreates the refinement and water
  actors from the saved river/lake topology without recalculating geography.
- **Generate Fast Preview** is an explicit temporary calculation. It neither
  reads nor overwrites the baked asset.
- **Clear Generated World** removes derived water, refinement and preview
  objects. It does not delete or clear the baked asset.

Mesh Terrain background operations never generate geography implicitly. If no
valid asset is assigned, they report that baked data is missing and do no work.

## Stored data

The asset records the format and generator versions, UTC generation time,
settings snapshot and hash, seed, bounds, cell spacing, generation statistics,
river topology, lake basins and the ocean boundary.

Raster fields are divided into independently compressed 128 by 128-cell chunks
by default. Each payload contains final and filled heights, erosion resistance,
landform masks, accumulation, slope, flow receivers and weights, fill parents
and lake membership. The first implementation decompresses all chunks for a
Mesh Terrain build. The chunk boundary permits later section-local/asynchronous
loading without changing the asset format or regenerating established terrain.

Changing generator settings marks the saved settings hash as different but does
not silently regenerate or invalidate established geography. Run **Generate and
Bake Geography** deliberately when the changed settings should replace it.
