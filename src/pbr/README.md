# PBR Shader — Physically-Based Rendering for LightWave 3D

A combined PBR-lite shader that brings modern material concepts to LightWave 5.x
on AmigaOS. Includes variable metallic intensity, roughness, ambient occlusion,
blurred reflections, and environment sampling in a single plugin. For
angle-dependent Fresnel effects, stack with the standalone Fresnel plugin.

## Features

### Roughness
Perturbs surface normals based on object-space position using a deterministic
hash function. This breaks up perfect mirror reflections and sharp specular
highlights, simulating micro-surface detail without requiring bump maps.

### Ambient Occlusion
Casts rays from each surface point into the surrounding hemisphere to detect
nearby geometry. Areas where rays hit nearby surfaces (corners, crevices) are
darkened, adding depth and contact shadows. Configurable sample count (4/8/16),
radius, and strength.

### Metallic
Variable intensity (0-100) blending between dielectric and metallic behavior.
Higher values increase reflectivity, reduce diffuse, and boost specular using
an IOR-based Fresnel curve. At 100%, the surface is fully metallic with
near-zero diffuse — appearance dominated by reflections.

### Blurred Reflections
Replaces LightWave's single-ray mirror reflections with multi-sample cone
tracing. Casts 4/8/16 rays around the reflection direction, with cone spread
controlled by the Blur Spread setting (independent of roughness). The averaged
result is blended into the surface color, producing soft, spread-out
reflections for frosted, rough, or brushed materials.

### Environment Sampling
Approximates indirect lighting by casting 4/8/16 rays into the hemisphere
around the surface normal. Each sample is cosine-weighted for physically
correct importance sampling. The gathered light is added to the surface
color and slightly boosts luminosity, giving objects a sense of being lit
by their surroundings rather than just direct lights.

## Installation

1. Copy `pbr.p` to your LightWave plugins directory
2. Add these lines to your LW config file:

```
Plugin ShaderHandler PBR pbr.p PBR Shader
Plugin ShaderInterface PBR pbr.p PBR Shader
```

3. Restart LightWave

## Usage

1. Open the **Surfaces** panel in Layout
2. Select a surface and go to the **Shaders** section
3. Add **PBR Shader** from the shader list
4. Click **Options** to adjust settings
5. Render to see the effect

### Settings

| Setting | Range | Default | Description |
|---|---|---|---|
| IOR | 1.0 – 5.0 | 1.5 | Index of Refraction for metallic F0 |
| Metallic | 0 – 100 | 0 | Metallic intensity (0=dielectric, 100=full metal) |
| Roughness | on/off | off | Perturb normals for rough surfaces |
| Roughness Amount | 0 – 100 | 20 | Intensity of normal perturbation |
| AO | Off/2/4/8 | Off | Ambient occlusion ray samples |
| AO Radius | float | 1.0m | Maximum occlusion distance |
| AO Strength | 0 – 100 | 50 | Occlusion darkening intensity |
| Blur Refl | Off/2/4/8 | Off | Blurred reflection ray samples |
| Blur Spread | 0 – 100 | 30 | Cone spread angle |
| Env Light | Off/2/4/8 | Off | Environment sampling ray samples |
| Env Strength | 0 – 100 | 50 | Indirect lighting intensity |

For angle-dependent Fresnel effects (reflection, transparency, diffuse, specular),
stack the **Fresnel** plugin on the same surface.

### Material Presets

**Polished metal**: Metallic 100, IOR 2.0+, Roughness off + Fresnel plugin
- Set surface color to metal tint, high mirror

**Brushed metal**: Metallic 80, IOR 2.0+, Roughness on (30-50), Blur Refl 2-4

**Plastic**: Metallic 0, low Roughness (10-20) + Fresnel plugin (IOR 1.45)

**Concrete/stone**: Metallic 0, high Roughness (60-80), AO on, Env Light on (30-50)

### Performance Notes

- **Fresnel, Roughness, Metallic**: Very fast — no extra rays, pure math
- **Ambient Occlusion**: Slower — casts 4-16 rays per surface point. Use
  4 samples for previews, 8-16 for final renders
- **Blurred Reflections**: Similar cost to AO — casts 4-16 rays per reflective
  surface point. Only active on surfaces with mirror > 0
- **Environment Sampling**: Casts 4-16 rays per surface point for indirect
  lighting. Combine with AO sparingly — both cast rays and costs stack
- On JIT-equipped emulators ray-based features are quite manageable; on
  real 68k hardware, limit ray-casting features to 4 samples each
- All features can be independently enabled/disabled

## Scene Persistence

All settings are saved with the scene file and restored on reload.
