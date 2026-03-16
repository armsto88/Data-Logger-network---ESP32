#include <Arduino.h>
#include <unity.h>
#include "protocol.h"

static void test_espnow_channel_is_valid(void) {
    TEST_ASSERT_TRUE(ESPNOW_CHANNEL >= 1);
    TEST_ASSERT_TRUE(ESPNOW_CHANNEL <= 14);
}

static void test_default_wake_interval_is_reasonable(void) {
    TEST_ASSERT_TRUE(DEFAULT_WAKE_INTERVAL_MINUTES >= 1);
    TEST_ASSERT_TRUE(DEFAULT_WAKE_INTERVAL_MINUTES <= 60);
}

static void test_protocol_structs_have_expected_footprint(void) {
    // Guard against accidental protocol bloat and layout drift.
    TEST_ASSERT_TRUE(sizeof(sensor_data_message_t) <= 64);
    TEST_ASSERT_TRUE(sizeof(discovery_message_t) <= 64);
    TEST_ASSERT_TRUE(sizeof(pairing_request_t) <= 64);
    TEST_ASSERT_TRUE(sizeof(deployment_command_t) <= 96);
    TEST_ASSERT_TRUE(sizeof(time_sync_response_t) <= 64);
}

void setup() {
    delay(1000);
    UNITY_BEGIN();
    RUN_TEST(test_espnow_channel_is_valid);
    RUN_TEST(test_default_wake_interval_is_reasonable);
    RUN_TEST(test_protocol_structs_have_expected_footprint);
    UNITY_END();
}

void loop() {
}
