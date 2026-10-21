/* SPDX-License-Identifier: MPL-2.0
 * Our own functions; demonstrates GNU ELF binding, not Horizon rtld policy. */
__attribute__((noinline, visibility("default"))) int original_entry(void) { return 11; }
__attribute__((noinline, visibility("default"))) int observer_proxy_control(void) { return 22; }
extern int original_direct(void) __attribute__((alias("original_entry"), visibility("hidden")));
__attribute__((noinline, visibility("default"))) int call_internal_plt(void) { return original_entry(); }
__attribute__((noinline, visibility("default"))) int call_internal_direct(void) { return original_direct(); }
