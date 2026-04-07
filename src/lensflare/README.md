# LensFlare — Specular Lens Flare for LightWave 3D

Post-render image filter that detects bright specular highlights and
composites glow and star streaks over the rendered image. Applied as
an Image Filter in Layout's Effects panel.

## How It Works

1. Scans the specular buffer for pixels above a brightness threshold
2. Identifies the 8 brightest specular hotspots
3. For each hotspot, renders:
   - Circular glow with quadratic falloff
   - 6-point hexagonal star streaks with fade
4. Additively blends the flare onto the final rendered image

The flare has a warm color tint (white center fading to amber) for a
natural camera lens look.

## Installation

1. Copy `lensflare.p` to your LightWave plugins directory
2. Add this line to your LW config file:

```
Plugin ImageFilterHandler LensFlare lensflare.p LensFlare
```

3. Restart LightWave

## Usage

1. In Layout, go to **Effects** panel (or Windows → Image Processing)
2. Add **LensFlare** to the image filter list
3. Ensure your scene has objects with visible specular highlights
4. Render — flares appear automatically on bright specular areas

### Default Settings

| Setting | Default | Description |
|---|---|---|
| Threshold | 200 | Minimum specular brightness (0-255) to trigger flare |
| Glow Radius | 40 | Radius of circular glow in pixels |
| Streak Length | 80 | Length of star streaks in pixels |
| Intensity | 60 | Overall flare brightness (0-100) |
| Streak Count | 6 | Number of star streak arms (max 6) |

### Tips

- **Lower the threshold** (150-180) for more flares on dimmer highlights
- **Increase glow radius** (60-100) for larger, softer bloom effects
- **Increase streak length** (120-200) for dramatic star patterns
- **Reduce intensity** (30-40) for subtle glints, increase (80-100) for dramatic flares
- Works best with high specular surfaces (metals, glass, wet surfaces)
- Multiple flare sources are supported (up to 8 brightest)

### Performance Notes

- Uses bounding-box optimization: only processes pixels near each flare source
- Pass 1 (specular scan): O(width x height) — fast
- Pass 2 (flare render): processes only the bounding box around each source
- On JIT emulators: very fast. On real 68k: depends on number of flare sources and radii
