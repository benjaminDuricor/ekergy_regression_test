#ifndef SHARED_H
#define SHARED_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/touch_pad.h"
#include "driver/adc.h"
#include "esp_err.h"
#include "aws_iot_mqtt_client_interface.h"
#include "ota_mqtt_interface.h"
#include "aws_iot_config.h"
#include "nvs.h"
#include <math.h>
#include <stdbool.h>
// #include "aws_iot_ota_agent.h"
#include "driver/rtc_io.h"

// Define example_timer_info_t
typedef struct {
    int timer_group;
    int timer_idx;
    int alarm_interval;
    bool auto_reload;
} example_timer_info_t;

// Define example_timer_event_t
typedef struct {
    example_timer_info_t info;
    uint64_t timer_counter_value;
} example_timer_event_t;

// External variables
extern short ecgState;
extern short ecgHandsOn;
extern unsigned int ecgAcqCounter;
extern unsigned int ecgRecCounter;
extern unsigned int ecgMqttCounter;
extern unsigned short *headBuffer;
extern unsigned char *headTxBuffer;
extern unsigned short sequenceTimer;
extern unsigned short oldSequenceTimer;
extern unsigned short ledSelect;
extern unsigned long deepSleepCounter;

extern short jobCheckState;
extern unsigned char blinkingPattern[9][10];
extern unsigned short *dataBuffer;
extern EventGroupHandle_t wifi_event_group;
extern const int CONNECTED_BIT;
extern float w0, w1, w2, w3, w4;
extern float bpfX, bpfX1;
extern unsigned short aData;
extern int peakCounter;
extern int detectedBeat;
extern float bpfS0, bpfS1, bpfS2, bpfS3, bpfS4, bpfS5, bpfS6, bpftmp;
extern float mVariance, mAvg, m0, m1, m2, m3, m4;
extern float pVariance, pAvg, p0, p1, p2, p3, p4;
extern float variance;
extern float nAvg, n0, n1, n2, n3, n4, n5, n6;
extern int hb0, hb1;
extern float heartRate;
extern float noiseFloor;
extern QueueHandle_t s_timer_queue;
extern adc_channel_t channel;
extern bool mqttSessionEstablished;
extern bool deep_sleep_called;
extern bool deep_sleep_test_switch;
extern bool rec_mqtt_switch;

// Function declarations
int establishConnection(void);
OtaMqttStatus_t mqttPublishNoMutex(const char * const pacTopic, uint16_t topicLen, const char * pMsg, uint32_t msgSize, uint8_t qos); // Use the detailed signature
void disconnect(void);

// Define constants
#define ECG_IDLE 0
#define ECG_ACQUIRING 1
#define ECG_RECORDING 2
#define ECG_SENDING_MQTT 3
#define ECG_FINISH 4
#define ECG_ERROR_WIFI 5
#define ECG_ERROR_MQTT 6
#define ECG_OTA_UPDATE 7
#define ECG_SSID_RESET 8
#define ECG_ACQCOUNT 3000
#ifdef TEST_ENV
#define ECG_RECCOUNT 1000
#else
#define ECG_RECCOUNT 9000
#endif
#define ECG_MQTTCOUNT 2000
#define JOB_CHECK_STATE_NOT_CHECKED 0
#define JOB_CHECK_STATE_CHECKED_NO_UPDATE 1

#define LED_RED GPIO_NUM_16
#define LED_GREEN GPIO_NUM_17
#define LED_BLUE GPIO_NUM_18
#define LOPlus 34
#define LOMinus 35
#define SDN 23
#define FR 21
#define DC 22
#define DEEPSLEEP GPIO_NUM_32

#define TOUCH_THRESH_NO_USE 0

void calibrate_touch_pad(touch_pad_t pad);

#endif // SHARED_H
