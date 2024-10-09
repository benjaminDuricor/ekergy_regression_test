#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "driver/touch_pad.h"
#include "driver/rtc_io.h"
#include "driver/adc.h"
#include "aws_iot_task.h"
#include "shared.h" // Include the shared header file

static const char *TAG = "aws_iot_task";

// Define the external variables
short ecgState = 0;
short ecgHandsOn = 0;
unsigned int ecgAcqCounter = 0;
unsigned int ecgRecCounter = 0;
unsigned int ecgMqttCounter = 0;
unsigned short *headBuffer = NULL;
unsigned char *headTxBuffer = NULL;
unsigned short sequenceTimer = 0;
unsigned short oldSequenceTimer = 0;
unsigned short ledSelect = 0;
#ifdef TEST_ENV
unsigned long deepSleepCounter = 4;
#else
unsigned long deepSleepCounter = 10000;
#endif

short jobCheckState = 0;
// unsigned char blinkingPattern[9][10] = {{0}};
unsigned short *dataBuffer = NULL;
// EventGroupHandle_t wifi_event_group = NULL;
// const int CONNECTED_BIT = BIT0;
float w0 = 0, w1 = 0, w2 = 0, w3 = 0, w4 = 0;
float bpfX = 0, bpfX1 = 0;
unsigned short aData = 0;
int peakCounter = 0;
int detectedBeat = 0;
float bpfS0 = 0, bpfS1 = 0, bpfS2 = 0, bpfS3 = 0, bpfS4 = 0, bpfS5 = 0, bpfS6 = 0, bpftmp = 0;
float mVariance = 0, mAvg = 0, m0 = 0, m1 = 0, m2 = 0, m3 = 0, m4 = 0;
float pVariance = 0, pAvg = 0, p0 = 0, p1 = 0, p2 = 0, p3 = 0, p4 = 0;
float variance = 0;
float nAvg = 0, n0 = 0, n1 = 0, n2 = 0, n3 = 0, n4 = 0, n5 = 0, n6 = 0;
int hb0 = 0, hb1 = 0;
float heartRate = 0;
float noiseFloor = 0;
// QueueHandle_t s_timer_queue = NULL;
// adc_channel_t channel = 0;
bool mqttSessionEstablished = false;

#ifdef TEST_ENV
// Test-specific variable and mock function
int test_counter = 0;

__attribute__((weak)) void esp_deep_sleep_start() {
    test_counter++;  // Increment counter instead of going to deep sleep
    ESP_LOGI("TEST", "Mocked deep sleep start called. Test counter: %d", test_counter);
}
#endif

void aws_iot_task(void *param) {
    // Initialize the watchdog timer
    // esp_task_wdt_init(10, true); // Set timeout to 10 seconds, panic=true
    // esp_task_wdt_add(NULL); // Add current task to the watchdog
    char cPayload[100];

    ecgHandsOn = 0;
    ecgAcqCounter = 0;
    ecgRecCounter = 0;
    ecgMqttCounter = 0;
    sequenceTimer = 0;
    oldSequenceTimer = 0;
    ledSelect = LED_GREEN;
    jobCheckState = JOB_CHECK_STATE_NOT_CHECKED;
    AWS_IoT_Client client;

    headTxBuffer = (unsigned char *)calloc(21000, sizeof(unsigned char));
    if (headTxBuffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate data buffer");
        abort();
    }

    headBuffer = (unsigned short *)headTxBuffer;
    dataBuffer = headBuffer;

    uint8_t brd_mac[6];
    char topic_name[16];
    const char *topic_prefix = "ecg/";
    esp_wifi_get_mac(WIFI_IF_STA, brd_mac);
    snprintf(topic_name, 18, "%s%02X%02X%02X%02X%02X%02X",
             topic_prefix, brd_mac[0], brd_mac[1], brd_mac[2], brd_mac[3], brd_mac[4], brd_mac[5]);

    ESP_LOGI(TAG, "calloc return %x", (int)dataBuffer);

    IoT_Error_t rc = FAILURE;

    int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                                   false, true, portMAX_DELAY);

    if (!(bits & CONNECTED_BIT)) {
        ESP_LOGE(TAG, "timeout bits=%d\n", bits);
        gpio_set_level(LED_RED, 0);
        ecgState = ECG_ERROR_WIFI;
    }


    int counter = 0;
    int counter3 = 0;
    // unsigned long deepSleepCounter = 10000;
    int debouceCounter = 101;
    int jobCheckCounter = 0;
    gpio_set_level(LED_GREEN, 1);
    gpio_set_level(LED_BLUE, 1);
    gpio_set_level(LED_RED, 1);

    while (1) {
        example_timer_event_t evt;
        xQueueReceive(s_timer_queue, &evt, portMAX_DELAY);

        counter++;
        oldSequenceTimer = sequenceTimer;
        sequenceTimer = (unsigned short)counter / 30;

        // esp_task_wdt_reset();

        if (sequenceTimer != oldSequenceTimer) {
            ledSelect = LED_GREEN;
            switch (ecgState) {
                case ECG_IDLE:
                    ledSelect = LED_GREEN;
                    break;
                case ECG_ACQUIRING:
                case ECG_RECORDING:
                case ECG_SENDING_MQTT:
                    ledSelect = LED_BLUE;
                    break;
                case ECG_SSID_RESET:
                    ledSelect = LED_RED;
                    break;
                case ECG_FINISH:
                    ledSelect = LED_GREEN;
                    break;
                case ECG_ERROR_WIFI:
                case ECG_ERROR_MQTT:
                default:
                    ledSelect = LED_RED;
                    break;
            }
            gpio_set_level(LED_GREEN, 1);
            gpio_set_level(LED_BLUE, 1);
            gpio_set_level(LED_RED, 1);

            gpio_set_level(ledSelect, (uint32_t)blinkingPattern[ecgState][sequenceTimer]);
        }

        counter3++;
        jobCheckCounter++;
        if (counter > 300) {
            counter = 0;
            uint32_t fs = xPortGetFreeHeapSize();
            ESP_LOGI(TAG, "ecg state = %d, Stack remaining for task '%s' is %d bytes, free mem is %d", ecgState, pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL), fs);
        }

        #ifdef TEST_ENV
        int loPlusValue = 1;
        int loMinusValue = 1;

        // Print the values before the if statement
        // printf("LOPlus value: %d\n", loPlusValue);
        // printf("LOMinus value: %d\n", loMinusValue);
        // printf("deep_sleep_test_switch: %d\n", deep_sleep_test_switch);

            // if (((loPlusValue == 1) || (loMinusValue == 1)) && (debouceCounter > 100)) {

        if ((deep_sleep_test_switch ? 
            ((loPlusValue == 1) || (loMinusValue == 1)) && (debouceCounter > 100): 
            ((gpio_get_level((gpio_num_t)LOPlus) == 1) || (gpio_get_level((gpio_num_t)LOMinus) == 1))) && (debouceCounter > 100)) {
        #else
            if (((gpio_get_level((gpio_num_t)LOPlus) == 1) || (gpio_get_level((gpio_num_t)LOMinus) == 1)) && (debouceCounter > 100)) {
        #endif

            pVariance = 300000; pAvg = 300; p0 = 1000; p1 = 0; p2 = 1000; p3 = 0; p4 = 1000;
            variance = 1000;
            ecgHandsOn = 0;
            // printf("No touch detected, ecgHandsOn: %d, LOPlus: %d, LOMinus: %d, debounceCounter: %d\n", 
            //     ecgHandsOn, gpio_get_level((gpio_num_t)LOPlus), gpio_get_level((gpio_num_t)LOMinus), debouceCounter);
        } else {
            if (debouceCounter <= 100) {
                debouceCounter++;
            } else {
                debouceCounter = 0;
            }
            ecgHandsOn = 1;
            // printf("Touch detected, ecgHandsOn: %d, ADC value: %d\n", ecgHandsOn, aData);
            // printf("ADC: %d, bpfX: %f, w0: %f, bpfS3: %f\n", aData, bpfX, w0, bpfS3);
            
            gpio_set_level(LED_RED, 1);
            aData = adc1_get_raw((adc1_channel_t)channel);
            if ((aData < 4090) && (aData > 6))
                bpfX = (float)aData;

            w0 = 3.269793 * w1 - 4.169410 * w2 + 2.523669 * w3 - 0.624207 * w4 + bpfX;
            bpfX1 = 0.082619 * (w0 - 2.0 * w2 + w4);
            w4 = w3;
            w3 = w2;
            w2 = w1;
            w1 = w0;
            w0 = 3.000162 * w1 - 3.294086 * w2 + 1.577024 * w3 - 0.283222 * w4 + bpfX1;
            bpfX1 = 0.065274 * (w0 - 2.0 * w2 + w4);
            w4 = w3;
            w3 = w2;
            w2 = w1;
            w1 = w0;

            bpfS0 = bpfS1;
            bpfS1 = bpfS2;
            bpfS2 = bpfS3;
            bpfS3 = bpfS4;
            bpfS4 = bpfS5;
            bpfS5 = bpfS6;
            bpfS6 = bpfX1;

            if ((bpfS3 > (mAvg + noiseFloor) / 2) && (peakCounter < 750) && (peakCounter > 100) && ((bpfS3 > bpfS2) && (bpfS3 > bpfS4)) && (((bpfS3 - bpfS0) > 3) && ((bpfS3 - bpfS6) > 3)) && (bpfS3 < 1500)) {
                heartRate = 30000 / peakCounter;
                m0 = m1;
                m1 = m2;
                m2 = m3;
                m3 = m4;
                m4 = bpfS3;
                mAvg = (m0 + m1 + m2 + m3 + m4) / 5;
                p0 = p1;
                p1 = p2;
                p2 = p3;
                p3 = p4;
                p4 = peakCounter;
                pAvg = (p0 + p1 + p2 + p3 + p4) / 5;
                pVariance = ((p0 - pAvg) * (p0 - pAvg) + (p1 - pAvg) * (p1 - pAvg) + (p2 - pAvg) * (p2 - pAvg) + (p3 - pAvg) * (p3 - pAvg) + (p4 - pAvg) * (p4 - pAvg)) / 5;
                variance = sqrt(pVariance);
                detectedBeat++;
                bpftmp = bpfS3;
                peakCounter = 0;
            } else {
                if (peakCounter > 5000) {
                    peakCounter = 0;
                    detectedBeat--;
                }
                peakCounter++;
                if ((bpfS3 > bpfS2) && (bpfS3 > bpfS4)) {
                    noiseFloor = (noiseFloor + bpfS3) / 2;
                    if (peakCounter > 500) mAvg = 0.9 * mAvg;
                }
                bpftmp = 0;
            }
        }

        switch (ecgState) {
            case ECG_IDLE:
                if (ecgHandsOn == 1) {
                    ecgState = ECG_ACQUIRING;
                    ecgAcqCounter = 0;
                    ecgRecCounter = 0;
                    ecgMqttCounter = 0;
                    dataBuffer = headBuffer;
                    deepSleepCounter = 10000;
                } else {
                    deepSleepCounter--;
                    if (deepSleepCounter == 0) {
                        ESP_ERROR_CHECK(touch_pad_init());
                        touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
                        touch_pad_set_voltage(TOUCH_HVOLT_2V4, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
                        touch_pad_config(TOUCH_PAD_NUM9, TOUCH_THRESH_NO_USE);
                        calibrate_touch_pad(TOUCH_PAD_NUM9);
                        printf("Enabling touch pad wakeup\n");
                        esp_sleep_enable_touchpad_wakeup();
                        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
                        rtc_gpio_isolate(GPIO_NUM_12);
                        gpio_set_level(SDN, 0);
                        printf("Entering deep sleep\n");
                        #ifdef TEST_ENV
                            deep_sleep_called = true;
                        #else
                            esp_deep_sleep_start();
                        #endif
                    }
                }
                break;
            case ECG_ACQUIRING:
                ecgAcqCounter++;
                if (ecgHandsOn == 0) {
                    ecgState = ECG_IDLE;
                    ecgAcqCounter = 0;
                } else if (ecgAcqCounter > ECG_ACQCOUNT) {
                    ecgState = ECG_RECORDING;
                    ecgRecCounter = 0;
                } else if (variance < 100) {
                    ecgState = ECG_RECORDING;
                    ecgRecCounter = 0;
                }
                break;
            case ECG_RECORDING:
                *dataBuffer++ = aData;
                ecgRecCounter++;
                if (ecgHandsOn == 0) {
                    if (ecgRecCounter > ECG_RECCOUNT) {
                        ecgState = ECG_SENDING_MQTT;
                        ecgMqttCounter = 0;
                    } else {
                        ecgState = ECG_IDLE;
                    }
                } else if (ecgRecCounter >= ECG_RECCOUNT) {
                    printf("ljljl\n");
                    ecgState = ECG_SENDING_MQTT;
                    ecgMqttCounter = 0;
                    dataBuffer = headBuffer;
                }
                break;
            case ECG_SENDING_MQTT:
                rc = FAILURE;
                gpio_set_level(LED_BLUE, 0);
                int returnStatus = EXIT_SUCCESS;

                if (returnStatus == EXIT_SUCCESS) {
                    uint32_t free_heap_size = esp_get_free_heap_size();
                    uint32_t min_free_heap_size = esp_get_minimum_free_heap_size();
                    ESP_LOGI(TAG, "Send MQTT Stack remaining for task '%s' is %d bytes at counter, free mem is %d", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL), xPortGetFreeHeapSize());
                    if (!mqttSessionEstablished) {
                        int ret = establishConnection();
                    }
                    if (mqttSessionEstablished) {
                        unsigned short totalLen = 2 * ecgRecCounter;
                        char topic_name[16];
                        const char *topic_prefix = "ecg/";
                        uint8_t brd_mac[6];
                        esp_wifi_get_mac(WIFI_IF_STA, brd_mac);
                        snprintf(topic_name, 18, "%s%02X%02X%02X%02X%02X%02X",
                                 topic_prefix, brd_mac[0], brd_mac[1], brd_mac[2], brd_mac[3], brd_mac[4], brd_mac[5]);
                        const char *TOPIC = topic_name;
                        const int TOPIC_LEN = strlen(TOPIC);
                        mqttPublishNoMutex(TOPIC, TOPIC_LEN, headBuffer, totalLen, QOS0);
                    }
                }
                disconnect();
                ecgState = ECG_FINISH;
                ecgRecCounter = 0;
                jobCheckCounter = 0;
                ecgMqttCounter++;
                break;
            case ECG_FINISH:
                if (ecgHandsOn == 0) {
                    ecgState = ECG_IDLE;
                    jobCheckCounter = 0;
                    break;
                } else {
                    if (jobCheckCounter < 6000) {
                        ecgState = ECG_FINISH;
                        break;
                    } else {
                        gpio_set_level(LED_GREEN, 1);
                        gpio_set_level(LED_BLUE, 1);
                        gpio_set_level(LED_RED, 0);
                        jobCheckCounter = 0;
                        ecgState = ECG_FINISH;
                        gpio_set_level(LED_GREEN, 1);
                        gpio_set_level(LED_BLUE, 1);
                        gpio_set_level(LED_RED, 1);
                        break;
                    }
                }
                break;
            case ECG_OTA_UPDATE:
                ecgState = ECG_OTA_UPDATE;
                break;
            case ECG_SSID_RESET:
                if (counter3 < 3000)
                    break;
                else {
                    ecgState = ECG_IDLE;
                    counter3 = 0;
                }
                break;
            default:
                ecgState = ECG_IDLE;
                break;
        }
        // esp_task_wdt_reset();

        // Only include the following block during testing
        #ifdef TEST_ENV
            
            if (deep_sleep_test_switch == false && rec_mqtt_switch == false){
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // Wait for test notification
            }
        #endif

    }

    ESP_LOGE(TAG, "An error occurred in the main loop.");
    abort();
}
