# Deferred resolve readback

The experimental `d3d12_readback_resolve_defer_host_copy` option coalesces
waits for direct resolve copies into guest RAM. It defaults to false. The
`d3d12_readback_resolve_defer_submit_early` option submits each copy early
and also defaults to false. These options do not establish visual parity.

Before a `WAIT_REG_MEM` packet first reads guest memory, the command processor
asks the backend to complete pending copies. This happens independently of
the packet's sleep duration and before checking for a matching value. It
prevents short waits from spinning on an unsubmitted copy and prevents stale
memory from falsely satisfying a wait. Register-only waits keep their prior
behavior. If completion fails, packet execution returns failure and pending
copy state remains available for the next completion attempt.

Existing wait, interrupt, primary-buffer-end, and configured guest-signal
boundaries continue to flush pending copies. The new memory-poll boundary
does not prove that every guest-memory ordering case is covered.

With tests enabled, `ctest -C Release -R gpu.wait_reg_mem --output-on-failure`
in the build directory runs a headless regression. It compiles the actual
`WAIT_REG_MEM` handler with a bounded synthetic backend and checks short and
long waits, stale matches, completion failure, and register waits. It needs
Python and the configured Clang compiler; it creates no graphics device.

Build and test instructions: [Windows validation](WINDOWS_VALIDATION.md).
An exact-binary game-route run is still required before promoting these
experimental binaries to a validated gameplay baseline.
