# Exporting the original levels

Build and run the `export_levels` utility from the repository root:

```sh
cmake -S . -B build
cmake --build build --target export_levels
./build/export_levels Data build/level-data
```

The output directory contains one JSON file per level, the uncompressed pack
binary, and the original level and mark entries as binary files. The JSON
decodes the level metadata, road physics and texture IDs, both AI driving
tracks, object placements, road cross-sections, and road marks. Levels 4–10
are decrypted with the same key and byte transform used by the game before
decompression.

## JSON coordinates and records

- `roadSegments` is an ordered list of `[leftOuter, leftInner, rightInner,
  rightOuter]` signed 16-bit X coordinates. Each record represents one
  horizontal road slice in the game's road bitmap.
- `trackUp` and `trackDown` are the two AI traffic paths. Each point has flags,
  X, Y, and speed.
- `objects` lists authored object positions in level coordinates, including
  the resource ID that identifies the object type.
- `marks` contains pairs of floating-point endpoints for road marks.
- `metadata` and `roadInfo` contain level timing, object groups, spawn
  position, surface/physics values, and texture/resource references.

The export is intentionally read-only: the game loads its binary resource
packs, not JSON, so changing a JSON file alone does not alter gameplay. The
`.bin` files preserve the original unpacked bytes for investigation and for a
future pack builder. Keep a backup of the original `Data` file before
experimenting with resource data.

## Graphical editor

Open [`level-editor/index.html`](./level-editor/index.html) in a modern browser
and load a `level-XX.json` file from `build/level-data`. The editor can change
level metadata and selected surface values; select, move, and delete placed
objects or AI waypoints; add objects and waypoints; and reshape road sections
by dragging with a soft brush or editing their four X coordinates.

Switch between **Karte** and **Fahransicht**. The driving view uses the game's
640×480 road projection and the selected level's road boundaries, camera zoom,
marks, and original repeating road textures. Adjust **Kamera-X-Versatz** to
pan horizontally and bring roadside objects into view. To load those textures,
first export them from the original resource file:

```sh
cmake --build build --target hq_scene_assets
mkdir -p build/level-editor-assets/textures build/level-editor-assets/rle build/level-editor-assets/pics
./build/hq_scene_assets export Data build/level-editor-assets
```

Then export the original sprites too:

```sh
mkdir -p build/level-editor-assets/sprites
cmake --build build --target ai_sprite_assets
./build/ai_sprite_assets export Data build/level-editor-assets/sprites
```

Choose **Spieltexturen laden** and **Objektsprites laden**, selecting
`build/level-editor-assets` each time. The editor uses the exported `textures/`
and `sprites/` images. Without textures the road falls back to plain colors;
without sprites objects appear as position markers. The driving view shows
placed level objects with their original sprites, but does not simulate moving
traffic, cars driven by the player, or the HUD.

In **Karte**, use the mouse wheel to zoom, the middle mouse button to pan, and
drag objects, AI points, or the road to edit them. **JSON speichern** downloads
the edited level. The editor does not modify the original file or `Data`.
Edited JSON is not directly playable until converted back into the game's
resource-pack format.
