# GopFramebuffer

UEFI GOP framebuffer and MobileFramebuffer integration.

CPU and GPU scanout are supported. With `sheng-vsync=<Hz>`, a work-loop timer sends the MobileFramebuffer vsync message to the compositor.

Build and interface details: [documentation](https://unixnu.com/drivers/gop-framebuffer). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).

## Frame diagnostics

`sheng-gpu-trace=1` records frame identities, synchronization and display decisions. `sheng-gpu-pixels=1` retains bounded raw samples after GPU completion. The sheng log exporter carries both streams. See [GPU frame diagnostics](https://unixnu.com/porting/gpu-diagnostics) for fields and decoding.

Display traces obtain physical addresses from prepared direct-display slots. Validation records use address zero until a slot is mapped; the flip record supplies the mapped address. Observers do not prepare memory or query unwired descriptors.
