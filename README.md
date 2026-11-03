# GopFramebuffer

UEFI GOP framebuffer and MobileFramebuffer integration.

CPU and GPU scanout are supported. With `sheng-vsync=<Hz>`, a work-loop timer sends the MobileFramebuffer vsync message to the compositor.

The qualified 24A446 ABI can define `NATIVE_LEGACY_BOOT_SCANOUT_REQUESTED` to publish both the initial logical scanout request and firmware-powered state through the original family setter, after validating the primary GOP mapping. Its original submit path discards requests while the requested state is zero. This initialization does not implement physical panel power control or change user-client return values; hardware presentation still requires device validation.

Build and interface details: [documentation](https://unixnu.com/drivers/gop-framebuffer). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).
