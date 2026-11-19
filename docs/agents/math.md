# math - vectors, matrices, quaternions, shapes, intersections, colour, easing, random, noise, splines

Headers: `engine/include/braty/br_math.h` (core), `br_ease.h`, `br_random.h`, `br_noise.h`,
`br_spline.h`. Source: `engine/src/math/`. Test: `tests/test_math.c` (reference values that are not
hand-computed come from `tests/tools/math_reference.py`, stdlib Python).

All value types, all functions take and return by value. Nothing allocates (the arc-length
table uses caller storage). Plain scalar C; SIMD would be a later, measured change behind the
same functions.

## Conventions (read these first)

- 2D is screen space: x right, **y down**, radians, positive rotation = clockwise on screen.
- `br_mat3` is PixiJS `Matrix` (`a b c d tx ty`). `br_mat3_from_transform` is Pixi
  `setTransform` (position, pivot, scale, rotation, skew). `br_mat3_mul(m, o)` applies `o` first.
- `br_mat4` is column-major (`m[col * 4 + row]`), `v' = M * v`, upload as-is to HLSL and use
  `mul(matrix, vector)`. Clip space is SDL_GPU's: y up, depth 0..1. 3D is right-handed, cameras
  look down -Z (`br_mat4_look_at`, `br_quat_look_rotation`).
- For 2D drawing in window units: `br_mat4_ortho(0, width, height, 0, -1, 1)`.
- `br_quat_from_euler(x, y, z)` applies X, then Y, then Z; `br_quat_to_euler` is its inverse.
- Degenerate input never produces NaN: zero vectors normalise to zero, degenerate triangles
  miss, parallel rays miss, singular matrices invert to identity with `ok = false`.

## Functions you actually need

| Function | What for |
|---|---|
| `br_mat3_from_transform(x, y, px, py, sx, sy, rot, skx, sky)` | Node local transform. |
| `br_mat3_mul`, `br_mat3_invert`, `br_mat3_apply` | Parent * local, to-local, point transform. |
| `br_mat4_ortho`, `br_mat4_perspective`, `br_mat4_look_at` / `_look_to` | Projection / view. |
| `br_mat4_perspective_infinite`, `_reversed`, `_reversed_infinite`, `br_mat4_frustum` | Projection variants (reversed-Z: compare GREATER, clear depth to 0). |
| `br_mat4_from_trs`, `br_mat4_decompose`, `br_mat4_invert_affine` / `_rigid`, `_normal_matrix` | 3D transforms. |
| `br_quat_from_axis_angle`, `_slerp`, `_nlerp`, `_from_to`, `_look_rotation`, `_rotate_towards` | Rotations. |
| `br_lerpf`, `br_inverse_lerpf`, `br_remapf`, `br_smoothstepf`, `br_clamp01f`, `br_wrapf` | Scalars. |
| `br_smooth_dampf`, `br_move_towardsf`, `br_spring_step(_vec2/_vec3)` | Follow / juice (frame-rate independent). |
| `br_angle_wrap`, `br_angle_delta`, `br_angle_lerp`, `br_angle_move_towards` | Angles the short way round. |
| `br_vec2/3_reflect`, `_project`, `_angle`, `_clamp_length`, `_move_towards`, `br_vec3_slerp` | Vector helpers. |
| `br_rect_contains`, `br_polygon_contains`, ... `_bounds` | Hit areas and bounds (Pixi shapes). |
| `br_sat_polygon_polygon`, `br_sat_circle_polygon`, `br_obb2d_corners` | 2D convex overlap + push-out vector. |
| `br_ray_plane / _sphere / _box / _obb / _triangle` | 3D ray casts (t = ray parameter). |
| `br_sphere_sphere`, `br_sphere_box`, `br_box_box`, `br_obb_obb` | 3D overlap tests. |
| `br_frustum_planes`, `br_frustum_planes_sphere / _box` | Culling with your own view-projection. |
| `br_closest_point_segment / _ray / _plane / _box / _obb / _triangle`, `br_closest_points_segments`, `br_barycentric` | Closest points. |
| `br_color_parse("#ff8800")`, `br_color_from_hex(0xff8800, 1)` | Colours from data files. |
| `br_color_to_linear` / `_to_srgb`, `br_color_from_hsv` / `_to_hsv`, `br_color_lerp_oklab` | Colour spaces; OKLab lerp avoids muddy midpoints. |
| `br_ease_out_cubic(t)`, `br_ease_apply(type, t)`, `br_ease_from_name("in_out_back", &ok)` | Easing (tweens, UI). |
| `br_random_seed`, `_float`, `_range`, `_range_int`, `_on_sphere`, `_shuffle`, `_weighted` | Seeded PCG32. `br_random_engine(e)` = the engine's instance. |
| `br_noise_perlin2/3`, `_simplex2/3`, `_value2/3`, `br_noise_fbm2/3` | Procedural noise. |
| `br_spline_bezier_cubic3`, `_catmull_rom3`, `_hermite3`, `br_spline3_point`, `br_spline_arc_param` | Curves and constant-speed paths. |

## Snippet (from tests/test_math.c)

```c
br_mat3 pivoted = br_mat3_from_transform(100.0f, 50.0f, 8.0f, 4.0f, 2.0f, 3.0f, 0.7f, 0.0f, 0.0f);
BR_CHECK(vec2_near(br_mat3_apply(pivoted, br_vec2_make(8.0f, 4.0f)), 100.0f, 50.0f));   /* pivot lands on position */

bool ok = false;
BR_CHECK(br_ease_from_name("ease_OUT_Cubic", &ok) == BR_EASE_OUT_CUBIC && ok);

br_random r;
br_random_seed(&r, 42u, 54u);

/* constant-speed motion along a path */
float lengths[256];
br_spline_arc_table table;
BR_CHECK(br_spline_arc_table_build(&table, bez, lengths, 256) && br_nearly_equal(table.total, 10.0f, 1e-3f));
float t = br_spline_arc_param(&table, 5.0f);
```

## Easing names (data files)

`linear`, then `in_` / `out_` / `in_out_` + `quad cubic quart quint sine expo circ back elastic
bounce` (e.g. `"out_back"`). Case-insensitive, an `ease_` prefix is accepted. Unknown names give
`BR_EASE_LINEAR` with `ok = false` - log it. Every easing clamps t to 0..1 and returns exactly 0
and 1 at the ends; `back` and `elastic` overshoot in between.

## Random

PCG32 (O'Neill, XSH RR): 16 bytes of state, `br_random_seed(&r, seed, stream)`; the same seed and
stream give the same numbers on every platform (verified against the pcg32 reference sequence).
No hidden global: pass a `br_random *` around, or use `br_random_engine(e)` (seeded with a fixed
value at startup, so `--frames` runs repeat). `br_random_bounded` is unbiased;
`br_random_weighted` returns -1 when no weight is positive.

## Noise

Stateless, deterministic, roughly [-1, 1], period 256. Seed 0 is Ken Perlin's reference
permutation; other seeds shift the lattice by a hashed offset. Perlin is exactly 0 on integer
lattice points (sample at non-integer coordinates). fBm normalises by the sum of amplitudes;
each octave uses `seed + octave`.

## Gotchas

- `br_rect_contains` is right/bottom exclusive (Pixi).
- `br_polygon` and `br_spline3` borrow their points; they do not own or copy them.
- `br_color` is straight alpha; premultiply before blending with the premultiplied blend state.
  `br_color` values are sRGB: convert with `br_color_to_linear` before lighting maths.
- Singular matrices invert to identity and report `ok = false`.
- Ray functions return the ray parameter t: a distance only if the direction has unit length.
  Rays starting inside a solid report t = 0.
- 3D boxes and rays are passed as `(min, max)` / `(origin, dir)` so they work with `br_aabb` /
  `br_ray3d` from `br_view3d.h`. `br_camera3d.h` keeps its own `br_frustum_test_aabb` and
  `br_ray3d_*`; the `br_math.h` versions are the general ones.
- `br_sat_*` need convex polygons. The MTV pushes the first shape out of the second.
- `br_spring_step` is implicit Euler: stable at any dt, slightly over-damped at very large dt.
- `br_smooth_dampf` keeps state in `*velocity`; start it at 0 and keep it per follower.
- Arc-length tables are chord sums: a little short on tight bends; more samples = closer.

## Audit (Task 14c)

Compared against the public function lists of HandmadeMath 2.0 (`HandmadeMath.h`) and cglm
(`vec2/3/4`, `mat3/4`, `affine`, `cam`, `frustum`, `quat`, `euler`, `plane`, `ray`, `sphere`,
`box`, `aabb2d`, `ease`, `bezier`, `curve`, `noise`, `color`, `util`). Everything a game uses from
them is now here, written in the engine's plain style (no code copied).

Deliberately left out:
- SIMD and double precision (`dvec`, `ivec` types): measure first; integer vectors are not used
  by any module yet.
- `mat2`, `mat2x3`/`mat3x4`-style non-square matrices and component-wise matrix add/sub/scale:
  no engine use; `br_mat3` is the 2D affine type.
- Left-handed and [-1, 1]-depth projection variants: the engine is right-handed with SDL_GPU's
  0..1 depth; reversed-Z covers the precision use case.
- Euler orders other than X-Y-Z, `unproject` (use `br_mat4_invert` + `br_mat4_transform_point`,
  or `br_ray3d_from_ndc`), cglm's `io` printing.
- Capsules: `br_closest_points_segments` gives capsule-capsule and capsule-sphere in two lines.
- Centripetal Catmull-Rom: uniform only; add if paths with very uneven point spacing loop.
