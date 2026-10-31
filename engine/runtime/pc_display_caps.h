// What a monitor can do, as Windows, its driver and its EDID say: the first start's display defaults (pc_settings.cpp:
// frame rate, HDR, VRR) and the "Display" line of F10 and the Welcome screen (pc_overlay.cpp). pcd_edid_parse is pure
// C, no Windows: tools/engine/test_display_caps.py tests it.
#pragma once
#include <stddef.h>
#include <stdint.h>

// VRR as far as it can be known before a frame is shown
enum {
    PCD_VRR_NONE = 0,   // not offered, or not known (no NVIDIA answer and nothing in the EDID)
    PCD_VRR_OFF,        // the monitor can, the driver has it off (NVIDIA Control Panel: G-SYNC off for it)
    PCD_VRR_ON,         // the driver has it on (NVIDIA: NvAPI_Disp_GetVRRInfo)
    PCD_VRR_MONITOR,    // the monitor declares it (EDID) on an AMD / Intel GPU; not confirmed (the monitor's OSD, the
                        // driver or a borderless window can have it off): suggested, never set
};

typedef struct {
    char name[96];            // Windows' name for it (the EDID's: "LG TV SSCR2"), else its GDI device (\\.\DISPLAY1)
    int w, h, hz;             // the desktop mode
    int top_hz;               // the fastest refresh at that resolution
    int native_w, native_h;   // the EDID's preferred mode (0: not known)
    int hdr_capable, hdr_on;  // Windows HDR for it: offered / on
    int peak_nits;            // its peak brightness for HDR (DXGI's MaxLuminance; 0: not known)
    int vrr;                  // PCD_VRR_*
    const char *vrr_kind;     // "G-SYNC", "FreeSync", "Adaptive-Sync" (nullptr: none)
    int vrr_lo, vrr_hi;       // its range in Hz (0: not known)
    int nv_bits;              // NvAPI_Disp_GetVRRInfo's bits (-1: not an NVIDIA display), for the log
} pcd_caps;

// What an EDID (the base block and its extensions) declares
typedef struct {
    int native_w, native_h;   // the first detailed timing: the preferred mode
    int range_lo, range_hi;   // the range limits descriptor's vertical rates (0: none)
    int freesync;             // an AMD vendor block (CTA-861) with its FreeSync bit
    int freesync_lo, freesync_hi;
    int adaptive_sync;        // a DisplayID 2.x Adaptive-Sync block (VESA)
} pcd_edid;

static inline int pcd_edid_parse(const uint8_t *e, size_t n, pcd_edid *out) {
    static const uint8_t magic[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    pcd_edid r = {0};
    if (n < 128) return 0;
    for (int i = 0; i < 8; i++)
        if (e[i] != magic[i]) return 0;
    for (int d = 54; d <= 108; d += 18) {
        const uint8_t *t = e + d;
        if (t[0] | t[1]) {   // a detailed timing: the first is the preferred mode
            if (!r.native_w) {
                r.native_w = t[2] | (t[4] >> 4) << 8;
                r.native_h = t[5] | (t[7] >> 4) << 8;
            }
        } else if (t[3] == 0xfd) {   // range limits; EDID 1.4: byte 4 bits 0 / 1 add 255 to the min / max rate
            r.range_lo = t[5] + (t[4] & 1 ? 255 : 0);
            r.range_hi = t[6] + (t[4] & 2 ? 255 : 0);
        }
    }
    const size_t blocks = 1 + (size_t)e[126];
    for (size_t b = 1; b < blocks && (b + 1) * 128 <= n; b++) {
        const uint8_t *x = e + b * 128;
        if (x[0] == 0x02) {   // CTA-861: data blocks from byte 4 up to the detailed timings (byte 2)
            const int end = x[2] >= 4 && x[2] <= 127 ? x[2] : 4;
            for (int p = 4; p < end;) {
                const int tag = x[p] >> 5, len = x[p] & 31;
                if (p + 1 + len > end) break;
                // AMD's vendor block (OUI 00-00-1A, little-endian): after the OUI the version, then the feature
                // caps (bit 0: FreeSync), then the min / max refresh (amdgpu's former parse_hdmi_amd_vsdb)
                if (tag == 3 && len >= 5 && x[p + 1] == 0x1a && x[p + 2] == 0x00 && x[p + 3] == 0x00 && (x[p + 5] & 1)) {
                    r.freesync = 1;
                    if (len >= 7 && x[p + 6] && x[p + 7] > x[p + 6]) { r.freesync_lo = x[p + 6]; r.freesync_hi = x[p + 7]; }
                }
                p += 1 + len;
            }
        } else if (x[0] == 0x70 && x[1] >= 0x20) {   // DisplayID 2.x: version, section length, product type,
                                                      // extension count, then blocks (tag, revision, length)
            const int end = 5 + x[2] < 127 ? 5 + x[2] : 127;
            for (int p = 5; p + 3 <= end;) {
                const int tag = x[p], len = x[p + 2];
                if (p + 3 + len > end) break;
                if (tag == 0x2b) r.adaptive_sync = 1;   // DisplayID 2.x Adaptive-Sync data block
                p += 3 + len;
            }
        }
    }
    *out = r;
    return 1;
}

#ifdef __cplusplus
// The monitor (an HMONITOR; nullptr: the primary, where a first start opens). 0: Windows knows no such monitor.
// A full read, a few ms (the EDID only once per monitor): the first start's, and test_display_caps.py.
int pcd_caps_of(void *monitor, pcd_caps *out);
// For a thread that must not wait (F10 on the present thread): the last full read of that monitor, 0 while there is
// none yet; a stale one (another monitor, a display change, 10 s) is read again on a thread of its own.
int pcd_caps_cached(void *monitor, pcd_caps *out);
void pcd_caps_display_changed();   // WM_DISPLAYCHANGE (pc_overlay.cpp pc_wndproc): the next cached call reads again
// "LG TV SSCR2, 3840 x 2160 at 120 Hz (fastest 144). HDR on, 800 nits. VRR on (G-SYNC)."
void pcd_caps_text(const pcd_caps &c, char *buf, size_t n);
#endif
