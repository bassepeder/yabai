/*
 * Dock swipe payload adapted from InstantSpaceSwitcher:
 * https://github.com/geesawra/InstantSpaceSwitcher/commit/79a17c4dd041639751a3d6087009864a2d6dcf1b
 * https://github.com/jurplel/InstantSpaceSwitcher/tree/c64e0fd09857330422084387cb98e8d1f4c3e2d1
 * Natural scrolling handling from PR #100 (e240e88b30c4f882282749b5364b40ef51671903).
 *
 * Copyright (c) 2026 jurplel
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// These private IOHID records are embedded in CGEvent serialized field 4205.
// CGEvent's field headers are big-endian; the IOHID payload uses native byte order.
#pragma pack(push, 1)
struct space_gesture_hid_base
{
    uint32_t size;
    uint32_t type;
    uint32_t options;
    uint8_t depth;
    uint8_t reserved[3];
};

struct space_gesture_hid_header
{
    uint64_t timestamp;
    uint64_t sender_id;
    uint32_t options;
    uint32_t attribute_length;
    uint32_t event_count;
};

struct space_gesture_hid_fluid
{
    struct space_gesture_hid_base base;
    int32_t position_x;
    int32_t position_y;
    int32_t position_z;
    uint32_t swipe_mask;
    uint16_t motion;
    uint16_t flavor;
    int32_t progress;
};

struct space_gesture_hid_velocity
{
    struct space_gesture_hid_base base;
    int32_t x;
    int32_t y;
    int32_t z;
};

struct space_gesture_payload
{
    struct space_gesture_hid_header header;
    struct space_gesture_hid_fluid fluid;
    struct space_gesture_hid_velocity velocity;
};
#pragma pack(pop)

_Static_assert(sizeof(struct space_gesture_hid_header) == 28, "IOHID header layout");
_Static_assert(sizeof(struct space_gesture_hid_fluid) == 40, "IOHID gesture layout");
_Static_assert(sizeof(struct space_gesture_hid_velocity) == 28, "IOHID velocity layout");
_Static_assert(sizeof(struct space_gesture_payload) == 96, "IOHID payload layout");

bool space_gesture_requires_payload(void)
{
    return [[NSProcessInfo processInfo] operatingSystemVersion].majorVersion >= 27;
}

static bool space_gesture_natural_scrolling(void)
{
    // Read once per request, so changes in System Settings need no daemon restart.
    CFPreferencesAppSynchronize(kCFPreferencesAnyApplication);
    Boolean valid;
    Boolean enabled = CFPreferencesGetAppBooleanValue(CFSTR("com.apple.swipescrolldirection"),
                                                     kCFPreferencesAnyApplication, &valid);
    return valid ? enabled : true;
}

static CGEventRef space_gesture_create_event(int direction, bool natural_scrolling, uint32_t phase)
{
    if ((direction != -1 && direction != 1) || (phase != 1 && phase != 2 && phase != 4)) return NULL;

    int sign = natural_scrolling ? -direction : direction;
    bool ended = phase == 4;
    CGEventRef event = CGEventCreate(NULL);
    if (!event) return NULL;

    CGEventSetIntegerValueField(event, /* event type     */  55, 30);
    CGEventSetIntegerValueField(event, /* HID type       */ 110, 23);
    CGEventSetIntegerValueField(event, /* motion         */ 123, 1);
    CGEventSetIntegerValueField(event, /* phase          */ 132, phase);
    CGEventSetIntegerValueField(event, /* phase alias    */ 134, phase);
    CGEventSetDoubleValueField(event,  /* progress       */ 124, sign * 0.000016);
    CGEventSetDoubleValueField(event,  /* position X     */ 125, 0.1);
    CGEventSetDoubleValueField(event,  /* gesture flavor */ 138, 3.0);
    CGEventSetDoubleValueField(event,  /* timestamp alias*/ 169, (double) mach_absolute_time());
    // Velocity during Began/Changed can make the Dock reverse the transition.
    if (ended) CGEventSetDoubleValueField(event, /* velocity X */ 129, sign * 9999.0);

    uint64_t timestamp = CGEventGetTimestamp(event);
    struct space_gesture_payload payload = {
        .header = {
            .timestamp = timestamp ? timestamp : mach_absolute_time(),
            .event_count = ended ? 2 : 1,
        },
        .fluid = {
            .base = { .size = 40, .type = 23, .options = phase << 24 },
            .position_x = 6553, // 0.1 in 16.16 fixed point
            .motion = 1,
            .flavor = 3,
            .progress = sign, // +/-0.000016 rounds toward zero to +/-1 in 16.16
        },
        .velocity = {
            .base = { .size = 28, .type = 9, .depth = 1 },
            .x = sign * 9999 * 65536,
        },
    };

    CFDataRef data = CGEventCreateData(kCFAllocatorDefault, event);
    CFRelease(event);
    if (!data) return NULL;

    // Each event is fresh: append one payload, never augment an already augmented event.
    const uint8_t version[] = { 0, 0, 0, 2 };
    if (CFDataGetLength(data) < 4 || memcmp(CFDataGetBytePtr(data), version, sizeof(version)) != 0) {
        CFRelease(data);
        return NULL;
    }

    CFMutableDataRef augmented = CFDataCreateMutableCopy(kCFAllocatorDefault, 0, data);
    CFRelease(data);
    if (!augmented) return NULL;

    uint16_t payload_size = ended ? sizeof(payload) : offsetof(struct space_gesture_payload, velocity);
    uint16_t field_header[] = { CFSwapInt16HostToBig(payload_size), CFSwapInt16HostToBig(4205) };
    CFDataAppendBytes(augmented, (const UInt8 *) field_header, sizeof(field_header));
    CFDataAppendBytes(augmented, (const UInt8 *) &payload, payload_size);
    CGEventRef result = CGEventCreateFromData(kCFAllocatorDefault, augmented);
    CFRelease(augmented);
    return result;
}

bool space_gesture_perform(int direction, int count)
{
    if ((direction != -1 && direction != 1) || count < 1) return false;
    bool natural_scrolling = space_gesture_natural_scrolling();
    const uint32_t phases[] = { 1, 2, 4 }; // Began, Changed, Ended

    for (int step = 0; step < count; ++step) {
        // Prepare the complete sequence before posting Began, so allocation failure
        // cannot leave a gesture half-open.
        CGEventRef events[3] = {0};
        for (int i = 0; i < 3; ++i) {
            events[i] = space_gesture_create_event(direction, natural_scrolling, phases[i]);
            if (!events[i]) {
                for (int j = 0; j < i; ++j) CFRelease(events[j]);
                return false;
            }
        }

        for (int i = 0; i < 3; ++i) {
            CGEventPost(kCGSessionEventTap, events[i]);
            CFRelease(events[i]);
        }
    }

    // Posting is asynchronous; this reports delivery attempts, not Dock acknowledgement.
    return true;
}
