#include "unity.h"
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "demo_config.h"
#include "esp_adc_cal.h"
#include "driver/touch_pad.h"

/* Standard includes */
#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>

/* Platform-Specific Headers */
#include "demo_config.h"
#include "clock.h"
#include "demo_header.h"

/* FreeRTOS includes */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Network and TLS includes */
#include "tls_freertos.h"
#include "esp_pthread.h"

/* MQTT Library includes */
#include "core_mqtt.h"
#include "mqtt_subscription_manager.h"

/* Backoff Algorithm include */
#include "backoff_algorithm.h"

/* OTA Library includes */
#include "ota.h"
#include "ota_config.h"
#include "ota_os_freertos.h"
#include "ota_mqtt_interface.h"
#include "ota_pal.h"

/* JSON Parsing */
#include "cJSON.h"

/* NVS (Non-Volatile Storage) */
#include "nvs.h"
#include "nvs_flash.h"

// void test_example(void);
static void print_banner(const char* text);

const int CONNECTED_BIT = BIT0;
EventGroupHandle_t wifi_event_group;
xQueueHandle s_timer_queue;
unsigned char blinkingPattern[9][10] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, // IDLE - solid green
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1}, // ACQUIRING - fast blue
    {1, 1, 1, 1, 1, 0, 0, 0, 0, 0}, // RECORDING - slow flashing blue
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, // SENDING MQTT - solid blue
    {1, 0, 1, 0, 1, 0, 1, 0, 1, 0}, // Finishing - flashing green
    {0, 0, 1, 1, 1, 1, 1, 1, 1, 1}, // ERROR - WIFI disconnecting
    {1, 1, 0, 0, 1, 1, 0, 0, 1, 1}, // ERROR -MQTT failed
    {1, 0, 1, 0, 1, 0, 1, 0, 1, 0}, // SSID address setting - flashing red
    {1, 0, 1, 0, 1, 0, 1, 0, 1, 0}  // SSID address setting - flashing red

};
adc_channel_t channel = ADC_CHANNEL_0;     // GPIO36 ADC1 CH0 Sensor VP
// extern int establishConnection(void);
extern int mqttPublishNoMutex();
extern void disconnect(void);

char macAddress[13];
extern short nvsProvisionStatus;
pthread_mutex_t mqttMutex;
extern char *private_key;
extern size_t private_key_len;
extern char *certificate_pem;
extern size_t certificate_pem_len;
extern int aws_iot_demo_main();

void calibrate_touch_pad(touch_pad_t pad)
{
    int avg = 0;
    const size_t calibration_count = 128;
    for (int i = 0; i < calibration_count; ++i)
    {
        uint16_t val;
        touch_pad_read(pad, &val);
        avg += val;
    }
    avg /= calibration_count;
    const int min_reading = 300;
    if (avg < min_reading)
    {
        printf("Touch pad #%d average reading is too low: %d (expecting at least %d). "
               "Not using for deep sleep wakeup.\n",
               pad, avg, min_reading);
        touch_pad_config(pad, 0);
    }
    else
    {
        int threshold = avg - 100;
        printf("Touch pad #%d average: %d, wakeup threshold set to %d.\n", pad, avg, threshold);
        touch_pad_config(pad, threshold);
    }
}

void app_main(void)
{
    print_banner("Executing one test by its tag");
    UNITY_BEGIN();
    // unity_run_tests_by_tag("[wifi_test]",false);

    // unity_run_tests_by_tag("[mqtt_connection_failure]",false);

    // unity_run_tests_by_tag("[idle_acquired]",false);

    // unity_run_tests_by_tag("[acquiring_to_recording]",false);

    // unity_run_tests_by_tag("[recording_to_sending_mqtt]",false);

    unity_run_tests_by_tag("[ECG_IDLE_led]",false);

    // unity_run_tests_by_tag("[ECG_ACQUIRING_led]",false);

    // unity_run_tests_by_tag("[ECG_RECORDING_led]",false);

    // unity_run_tests_by_tag("[deep_sleep]",false);

    UNITY_END();

    // UNITY_BEGIN();
    // unity_run_tests_by_tag("[idle_acquried]",false);
    // UNITY_END();

    // UNITY_BEGIN();
    // unity_run_tests_by_tag("[aws_iot]",false);
    // UNITY_END();
    
}

static void print_banner(const char* text)
{
    printf("\n#### %s #####\n\n", text);
}