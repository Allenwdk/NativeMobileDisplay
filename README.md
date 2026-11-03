# GopFramebuffer

UEFI GOP framebuffer and MobileFramebuffer integration.

CPU and GPU scanout are supported. With `sheng-vsync=<Hz>`, a work-loop timer sends the MobileFramebuffer vsync message to the compositor.

Direct display keeps the first successful GPU fence snapshot for each swap. Busy or unavailable queries retry after 200 microseconds. Reaching 250 milliseconds records a slow wait; the current frame stays on screen until the target completes or the display family cancels the swap. Cancellation clears the pending fence timer and snapshot.

Run the fence-wait regression check with `python3 Test/check-fence-wait.py`. It compiles the display's fence-wait and cancellation methods against a simulated GPU with AddressSanitizer and UndefinedBehaviorSanitizer.

Build and interface details: [documentation](https://unixnu.com/drivers/gop-framebuffer). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).

## Frame diagnostics

`sheng-gpu-trace=1` records display arrival, fence and latch in the kernel GPU ring. Repeated pending display queries are coalesced. See [GPU frame diagnostics](https://unixnu.com/porting/gpu-diagnostics) for fields and decoding.

Display traces obtain physical addresses from prepared direct-display slots. Validation records use address zero until a slot is mapped; the flip record supplies the mapped address. Observers do not prepare memory or query unwired descriptors.


## Uke 24A446 integration (2026-11-03)

The requested-scanout initialization correction and upstream fence/cancellation lifecycle are included in the user-accepted uke GPU baseline (kernel CRC `90B6BC701210D9DB`). Boot, icon/widget loading and Settings/Dock transitions are reported working after the corresponding Metal library fix. No isolated display-throughput or long-run timing result is inferred.
