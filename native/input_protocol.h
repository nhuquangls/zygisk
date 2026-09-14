#ifndef CF_INPUT_PROTOCOL_H
#define CF_INPUT_PROTOCOL_H
#include <stdint.h>
#include <stdbool.h>

// Channel contract between the in-app runtime and the root companion.
// The companion streams a protocol-B multitouch contact into the real touch
// panel's evdev node, so no extra input device ever appears on the system and
// the injected events are indistinguishable from firmware contacts.

#define CF_TOUCH_MAGIC 0x43464934u
#define CF_TOUCH_SLOT 9
#define CF_TOUCH_TRACKING 0x5A17u // 23063; panel tracking range is 0..65535

#define CF_TOUCH_ACTION_DOWN 1
#define CF_TOUCH_ACTION_MOVE 2
#define CF_TOUCH_ACTION_UP 3
#define CF_TOUCH_ACTION_RESET 4

// Panel raw ranges, portrait native: raw = display px * ~10.
#define CF_TOUCH_ABS_X_MAX 18400
#define CF_TOUCH_ABS_Y_MAX 29440
// Landscape HUD viewport the controller is pinned to.
#define CF_TOUCH_DISP_W 2944
#define CF_TOUCH_DISP_H 1840
// Look-area anchor; DOWN must land exactly here.
#define CF_TOUCH_ANCHOR_X 1943
#define CF_TOUCH_ANCHOR_Y 736
// MOVE commands must stay inside this square around the anchor.
#define CF_TOUCH_DRIFT 420

typedef struct CfTouchCommand {
    uint32_t magic;
    uint8_t action;    // CF_TOUCH_ACTION_*
    uint8_t slot;      // CF_TOUCH_SLOT
    uint16_t tracking; // CF_TOUCH_TRACKING
    int32_t raw_x, raw_y;
} CfTouchCommand;

// Companion-side check: structure, fixed identifiers and raw bounds. Not
// static for the same reason as cf_touch_pack below.
bool cf_touch_valid(const CfTouchCommand *c) {
    if (!c || c->magic != CF_TOUCH_MAGIC || c->slot != CF_TOUCH_SLOT ||
        c->tracking != CF_TOUCH_TRACKING) return false;
    if (c->action < CF_TOUCH_ACTION_DOWN || c->action > CF_TOUCH_ACTION_RESET) return false;
    if (c->action == CF_TOUCH_ACTION_UP || c->action == CF_TOUCH_ACTION_RESET)
        return c->raw_x == 0 && c->raw_y == 0;
    return c->raw_x >= 0 && c->raw_x <= CF_TOUCH_ABS_X_MAX &&
           c->raw_y >= 0 && c->raw_y <= CF_TOUCH_ABS_Y_MAX;
}

// Runtime-side check plus display-to-raw conversion. Not static: the host
// test build exercises it through input_companion.c, which is the only other
// translation unit including this header in that binary.
bool cf_touch_pack(uint8_t action, int rotation, int layout, int x, int y,
                   CfTouchCommand *out) {
    if (!out || (rotation != 1 && rotation != 3) || (layout & ~1)) return false;
    out->magic = CF_TOUCH_MAGIC;
    out->action = action;
    out->slot = CF_TOUCH_SLOT;
    out->tracking = CF_TOUCH_TRACKING;
    out->raw_x = 0;
    out->raw_y = 0;
    if (action == CF_TOUCH_ACTION_UP || action == CF_TOUCH_ACTION_RESET) return true;
    if (action != CF_TOUCH_ACTION_DOWN && action != CF_TOUCH_ACTION_MOVE) return false;
    if (x < CF_TOUCH_ANCHOR_X - CF_TOUCH_DRIFT || x > CF_TOUCH_ANCHOR_X + CF_TOUCH_DRIFT ||
        y < CF_TOUCH_ANCHOR_Y - CF_TOUCH_DRIFT || y > CF_TOUCH_ANCHOR_Y + CF_TOUCH_DRIFT)
        return false;
    if (action == CF_TOUCH_ACTION_DOWN && (x != CF_TOUCH_ANCHOR_X || y != CF_TOUCH_ANCHOR_Y))
        return false;
    // The panel's raw X spans the short (1840px) side and raw Y the long
    // (2944px) side. Layout selects the vendor's mounting orientation; the
    // runtime verifies the choice against the first observed contact.
    int variant = (rotation == 1) ? layout : 1 - layout;
    int raw_x, raw_y;
    if (variant == 0) {
        raw_x = (CF_TOUCH_DISP_H - 1 - y) * CF_TOUCH_ABS_X_MAX / (CF_TOUCH_DISP_H - 1);
        raw_y = x * CF_TOUCH_ABS_Y_MAX / (CF_TOUCH_DISP_W - 1);
    } else {
        raw_x = y * CF_TOUCH_ABS_X_MAX / (CF_TOUCH_DISP_H - 1);
        raw_y = (CF_TOUCH_DISP_W - 1 - x) * CF_TOUCH_ABS_Y_MAX / (CF_TOUCH_DISP_W - 1);
    }
    if (raw_x < 0 || raw_x > CF_TOUCH_ABS_X_MAX || raw_y < 0 || raw_y > CF_TOUCH_ABS_Y_MAX)
        return false;
    out->raw_x = raw_x;
    out->raw_y = raw_y;
    return true;
}
#endif
