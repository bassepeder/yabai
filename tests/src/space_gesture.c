// Decode the serialized event independently of the production payload structs.
// None of these tests post events or change desktops.
static uint32_t test_gesture_le32(const uint8_t *p)
{
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static bool test_gesture_find_payload(CFDataRef data, const uint8_t **payload, size_t *payload_size)
{
    const uint8_t *bytes = CFDataGetBytePtr(data);
    size_t length = CFDataGetLength(data);
    if (length < 4 || memcmp(bytes, "\0\0\0\2", 4)) return false;

    int matches = 0;
    for (size_t offset = 4; offset < length;) {
        if (length - offset < 4) return false;
        uint16_t size = (uint16_t) bytes[offset] << 8 | bytes[offset + 1];
        uint16_t tag_field = (uint16_t) bytes[offset + 2] << 8 | bytes[offset + 3];
        uint16_t tag = tag_field >> 14;
        size_t field_size;
        if (tag == 0 && size > 0) field_size = size == 1 ? 8 : size;
        else if (tag == 1 || tag == 3) field_size = (size_t) size * 4;
        else return false;
        offset += 4;
        if (field_size > length - offset) return false;
        if ((tag_field & 0x3fff) == 4205) {
            if (tag != 0) return false;
            ++matches;
            *payload = bytes + offset;
            *payload_size = field_size;
        }
        offset += field_size;
    }
    return matches == 1;
}

TEST_FUNC(space_gesture_encoding,
{
    for (int natural = 0; natural <= 1; ++natural) {
        for (int direction = -1; direction <= 1; direction += 2) {
            for (int i = 0; i < 3; ++i) {
                uint32_t phase = 1u << i;
                bool ended = phase == 4;
                int sign = natural ? -direction : direction;
                CGEventRef event = space_gesture_create_event(direction, natural, phase);
                TEST_CHECK(event != NULL, true);
                if (!event) continue;
                TEST_CHECK((int) CGEventGetIntegerValueField(event, 55), 30);
                TEST_CHECK((int) CGEventGetIntegerValueField(event, 110), 23);
                TEST_CHECK((int) CGEventGetIntegerValueField(event, 123), 1);
                TEST_CHECK((int) CGEventGetIntegerValueField(event, 132), (int) phase);
                TEST_CHECK((int) CGEventGetIntegerValueField(event, 134), (int) phase);
                TEST_CHECK(CGEventGetDoubleValueField(event, 124) * sign > 0, true);
                TEST_CHECK((int) CGEventGetDoubleValueField(event, 129), ended ? sign * 9999 : 0);

                CFDataRef data = CGEventCreateData(kCFAllocatorDefault, event);
                CFRelease(event);
                TEST_CHECK(data != NULL, true);
                if (!data) continue;
                const uint8_t *payload = NULL;
                size_t payload_size = 0;
                bool found = test_gesture_find_payload(data, &payload, &payload_size);
                TEST_CHECK(found, true);
                TEST_CHECK((int) payload_size, ended ? 96 : 68);
                if (found && payload_size == (ended ? 96 : 68)) {
                    // Header: event count; fluid record: size, type, phase, depth,
                    // position, horizontal motion, Dock flavor, 16.16 progress.
                    TEST_CHECK((int) test_gesture_le32(payload + 24), ended ? 2 : 1);
                    TEST_CHECK((int) test_gesture_le32(payload + 28), 40);
                    TEST_CHECK((int) test_gesture_le32(payload + 32), 23);
                    TEST_CHECK((int) (test_gesture_le32(payload + 36) >> 24), (int) phase);
                    TEST_CHECK((int) payload[40], 0);
                    TEST_CHECK((int) test_gesture_le32(payload + 44), 6553);
                    TEST_CHECK((int) payload[60], 1);
                    TEST_CHECK((int) payload[62], 3);
                    TEST_CHECK((int32_t) test_gesture_le32(payload + 64), sign);
                    if (ended) {
                        TEST_CHECK((int) test_gesture_le32(payload + 68), 28);
                        TEST_CHECK((int) test_gesture_le32(payload + 72), 9);
                        TEST_CHECK((int) payload[80], 1);
                        TEST_CHECK((int32_t) test_gesture_le32(payload + 84), sign * 655294464);
                        TEST_CHECK((int) test_gesture_le32(payload + 88), 0);
                        TEST_CHECK((int) test_gesture_le32(payload + 92), 0);
                    }
                }
                CFRelease(data);
            }
        }
    }
});

TEST_FUNC(space_gesture_invalid_request,
{
    TEST_CHECK(space_gesture_create_event(0, true, 1) == NULL, true);
    TEST_CHECK(space_gesture_create_event(2, true, 1) == NULL, true);
    TEST_CHECK(space_gesture_create_event(1, true, 0) == NULL, true);
    TEST_CHECK(space_gesture_create_event(1, true, 3) == NULL, true);
    TEST_CHECK(space_gesture_perform(0, 1), false);
    TEST_CHECK(space_gesture_perform(1, 0), false);
    TEST_CHECK(space_gesture_perform(1, -1), false);
});
