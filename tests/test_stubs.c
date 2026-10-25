/* Stand-ins for the host program and the display/audio layer, so runtime
 * unit tests can link against runtime/ + runtime/hle/ alone. */
#include "ac3_runtime.h"
#include <stdint.h>
void ac3_render_screenshot(const char *path) { (void)path; }
void ac3_render_screenshot_when_drawing(const char *path, uint32_t n) { (void)path; (void)n; }
void ac3_boot_frame_hook(void) { }
void ac3_audio_queue_stats(u32 *under, u32 *over) { if (under) *under = 0; if (over) *over = 0; }
