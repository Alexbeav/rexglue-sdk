# Render-target lifecycle tracing

The D3D12 GPU plugin can write a bounded sequence of render-target cache,
resolve, and swap events to the normal runtime log. Use this trace to compare
the host render-target (`rtv`) and rasterizer-ordered-view (`rov`) paths without
changing either path's behavior.

Add these options to the title's runtime TOML file:

```toml
render_target_lifecycle_trace = true
render_target_lifecycle_trace_limit = 50000
# Optional and very high-volume:
```

The default limit is 8,192 events. Set the limit to `0` only when an unlimited
trace is intentional. Reuse events can consume the event budget quickly, so
raise the limit when a title performs extensive render-target reuse before the
scene under investigation. The options support hot reload, but the sequence
number is process-wide and does not reset when tracing is toggled.

Each line starts with `[RT_TRACE #N]`. Events include:

- `create`, `reuse`, and `evict`: render-target key, EDRAM base and pitch,
  multisampling, depth/color role, and format;
- `resolve-begin`: selected path, source and destination extents, copy rectangle,
  and clear flags;
- `resolve-end`: direct, EDRAM fallback, ROV, or no-copy route and final success;
- `swap`: selected path, guest front-buffer address and dimensions, active output
  dimensions, and texture formats.

For an A/B comparison, use separate working and user-data directories, keep all
settings except `render_target_path_d3d12` identical, and execute the same input
route. Compare ordered events and visible milestones rather than total log
volume. A successful resolve only establishes that the GPU path accepted the
operation; it does not prove that later guest CPU control flow is correct.
