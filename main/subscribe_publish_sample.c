/*
 * Copyright 2010-2015 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Additions Copyright 2016 Espressif Systems (Shanghai) PTE LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
/**
 * @file subscribe_publish_sample.c
 * @brief simple MQTT publish and subscribe on the same topic
 *
 * This example takes the parameters from the build configuration and establishes a connection to the AWS IoT MQTT Platform.
 * It subscribes and publishes to the same topic - "test_topic/esp32"
 *
 * Some setup is required. See example README for details.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "math.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/timer.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <time.h>
#include "esp_sntp.h"

#include "aws_iot_config.h"
#include "aws_iot_log.h"
#include "aws_iot_version.h"
#include "aws_iot_mqtt_client_interface.h"
#include "cJSON.h"

#include "demo_config.h"

#ifdef CONFIG_IDF_TARGET_ESP32
#define CHIP_NAME "ESP32"
#endif

#ifdef CONFIG_IDF_TARGET_ESP32S2BETA
#define CHIP_NAME "ESP32-S2 Beta"
#endif

//072122 ycc added the following bloc of includes
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>
#include "qrcode.h" 

#include "app_priv.h"   //for app_driver_init()
#include "board_esp32_devkitc.h"
//ycc 3-13-22 added include files
/* OpenSSL sockets transport implementation. */
#include "tls_freertos.h"

/* Clock for timer. */
#include "clock.h"

/* pthread include. */
#include <pthread.h>
//#include "semaphore.h"
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_pthread.h"

/* MQTT include. */
#include "core_mqtt.h"
#include "mqtt_subscription_manager.h"

/*Include backoff algorithm header for retry logic.*/
#include "backoff_algorithm.h"

/* OTA Library include. */
#include "ota.h"
#include "ota_config.h"

/* OTA Library Interface include. */
#include "ota_os_freertos.h"
#include "ota_mqtt_interface.h"
#include "ota_pal.h"

/* deep sleep */
#include "soc/soc_caps.h"
#include "esp_sleep.h"
#include "driver/adc.h"
#include "driver/rtc_io.h"
#include "soc/rtc.h"
#include "esp32/ulp.h"
#include "soc/sens_periph.h"
#include "driver/touch_pad.h"



#define DEFAULT_WAKEUP_LEVEL    ESP_GPIO_WAKEUP_GPIO_LOW
/* Include firmware version struct definition. */
#include "ota_appversion32.h"

#include "demo_header.h"
//ycc 3-13-22 end
static const char *TAG = "subpub";
static void initialize_sntp(void);
/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define CONFIG_ESP_MAXIMUM_RETRY   5
#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1


#define TIMER_DIVIDER         (80)  //  Hardware timer clock divider
#define TIMER_SCALE           (TIMER_BASE_CLK / TIMER_DIVIDER)  // convert counter value to seconds
#define DEFAULT_VREF    1100        //Use adc2_vref_to_gpio() to obtain a better estimate
#define NO_OF_SAMPLES   3          //Multisampling


// GPIO definition, in addtion GPIO 0 is used for button input and 27 is used for output
#define LOPlus  34
#define LOMinus 35
#define SDN     23
#define FR      21 
#define DC      22

#define LED_RED     GPIO_NUM_16
#define LED_GREEN   GPIO_NUM_17
#define LED_BLUE    GPIO_NUM_18

#define DEEPSLEEP   GPIO_NUM_32


#define ECG_IDLE 0              // Idle state
#define ECG_ACQUIRING 1         // ECG signal acquiring state
#define ECG_RECORDING 2         // ECG signal recording state
#define ECG_SENDING_MQTT 3      // ECG signal sending to mqtt state
#define ECG_FINISH 4            // data has been sent, and hands have to be removed from electroplates to return to idle state
#define ECG_ERROR_WIFI 5        // error WIFI, should flash the red LED
#define ECG_ERROR_MQTT 6        // error MQTT 
#define ECG_OTA_UPDATE 7        // ECG going through ota update
#define ECG_SSID_RESET 8
#define ECG_ACQCOUNT 3000      //  counter for acquiring time = 2ms x 1000 x 5 = 10 sec
#define ECG_RECCOUNT 9000     //  counter for recording time = 2ms x 1000 x 15 = 30 sec (15000 samples), 3.333ms x1000x15 (15000 samples)
#define ECG_MQTTCOUNT 2000      //  counter for sending mqtt expiration time = 2 ms x 1000 = 2 sec

#define JOB_CHECK_STATE_NOT_CHECKED             0   //right after booting, check once
#define JOB_CHECK_STATE_CHECKED_NO_UPDATE       1   //checked, wait for 24 hours before next check

#define MQTT_PROCESS_LOOP_TIMEOUT_MS        ( 1500U )

/* deep sleep defines */
#define DEFAULT_WAKEUP_LEVEL    ESP_GPIO_WAKEUP_GPIO_LOW
#define TOUCH_THRESH_NO_USE 0

typedef struct {
    int timer_group;
    int timer_idx;
    int alarm_interval;
    bool auto_reload;
} example_timer_info_t;

/**
 * @brief A sample structure to pass events from the timer ISR to task
 *
 */
typedef struct {
    example_timer_info_t info;
    uint64_t timer_counter_value;
} example_timer_event_t;

static xQueueHandle s_timer_queue;

static esp_adc_cal_characteristics_t *adc_chars;

static const adc_channel_t channel = ADC_CHANNEL_0;     //GPIO36 ADC1 CH0 Sensor VP
static const adc_bits_width_t width = ADC_WIDTH_BIT_12; //0 9bit, 1 10 bit, 2 11bit ,3  12 bit -set to 12 bit

static const adc_atten_t atten = ADC_ATTEN_DB_11;        /*!<The input voltage of ADC will be reduced to about 1/3.6*/
static const adc_unit_t unit = ADC_UNIT_1;

static short jobCheckState;

unsigned char blinkingPattern[9][10] = {
                            {0,0,0,0,0,0,0,0,0,0},  //IDLE - solid green
                            {0,1,0,1,0,1,0,1,0,1},  //ACQUIRING - fast blue
                            {1,1,1,1,1,0,0,0,0,0},  //RECORDING - slow flashing blue
                            {0,0,0,0,0,0,0,0,0,0},  //SENDING MQTT - solid blue
                            {1,0,1,0,1,0,1,0,1,0},  //Finishing - flashing green
                            {0,0,1,1,1,1,1,1,1,1},  //ERROR - WIFI disconnecting
                            {1,1,0,0,1,1,0,0,1,1},  //ERROR -MQTT failed
                            {1,0,1,0,1,0,1,0,1,0},  //SSID address setting - flashing red
                            {1,0,1,0,1,0,1,0,1,0}   //SSID address setting - flashing red

};
/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;
short ecgState;
char macAddress[13]; 
short nvsProvisionStatus = false;
char *private_key = NULL;
size_t private_key_len = 0;
char *certificate_pem = NULL;
size_t certificate_pem_len = 0;
extern int    aws_iot_demo_main();

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;
const int WIFI_CONNECTED_EVENT = BIT0;

//072221 ycc added the following block
#define PROV_QR_VERSION         "v1"
#define PROV_TRANSPORT_SOFTAP   "softap"
#define PROV_TRANSPORT_BLE      "ble"
#define QRCODE_BASE_URL         "https://espressif.github.io/esp-jumpstart/qrcode.html"

#define PROVISIONING_TIMEOUT_SEC 60  // Set your timeout period (e.g., 60 seconds)

static void start_provisioning_timer(void);
static void stop_provisioning_timer(void);
static void provisioning_timeout_handler(void* arg);

static esp_timer_handle_t provisioning_timer;

/* CA Root certificate, device ("Thing") certificate and device
 * ("Thing") key.

   Example can be configured one of two ways:

   "Embedded Certs" are loaded from files in "certs/" and embedded into the app binary.

   "Filesystem Certs" are loaded from the filesystem (SD card, etc.)

   See example README for more details.
*/
#if defined(CONFIG_EXAMPLE_EMBEDDED_CERTS)

extern const uint8_t aws_root_ca_pem_start[] asm("_binary_aws_root_ca_pem_start");
extern const uint8_t aws_root_ca_pem_end[] asm("_binary_aws_root_ca_pem_end");
extern const uint8_t certificate_pem_crt_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t certificate_pem_crt_end[] asm("_binary_certificate_pem_crt_end");
extern const uint8_t private_pem_key_start[] asm("_binary_private_pem_key_start");
extern const uint8_t private_pem_key_end[] asm("_binary_private_pem_key_end");

#elif defined(CONFIG_EXAMPLE_FILESYSTEM_CERTS)

static const char * DEVICE_CERTIFICATE_PATH = CONFIG_EXAMPLE_CERTIFICATE_PATH;
static const char * DEVICE_PRIVATE_KEY_PATH = CONFIG_EXAMPLE_PRIVATE_KEY_PATH;
static const char * ROOT_CA_PATH = CONFIG_EXAMPLE_ROOT_CA_PATH;

#else
#error "Invalid method for loading certs"
#endif

#define MQTT_RECV_POLLING_TIMEOUT_MS (1U)

/**
 * @brief Default MQTT HOST URL is pulled from the aws_iot_config.h
 */
char HostAddress[255] = AWS_IOT_MQTT_HOST;

/**
 * @brief Default MQTT port is pulled from the aws_iot_config.h
 */
uint32_t port = AWS_IOT_MQTT_PORT;

//ycc 3-13-22 added externs

typedef xSemaphoreHandle osi_sem_t;

/**
 * @brief Network connection context used in this demo.
 */
extern NetworkContext_t networkContext;

/**
 * @brief MQTT connection context used in this demo.
 */
extern MQTTContext_t mqttContext;

extern OtaAppBuffer_t otaBuffer;

extern char *registrationBuff;

/**
 * @brief Keep a flag for indicating if the MQTT connection is alive.
 */
bool mqttSessionEstablished;

/**
 * @brief Mutex for synchronizing coreMQTT API calls.
 */
pthread_mutex_t mqttMutex;

/**
 * @brief Semaphore for synchronizing buffer operations.
 */
osi_sem_t bufferSemaphore;

//ycc 071122 add this to be removed later
nvs_handle_t fleet_prov_handle;
extern int osi_sem_new(osi_sem_t *sem, uint32_t max_count, uint32_t init_count);

extern int osi_sem_free(osi_sem_t *sem);

extern int osi_sem_take(osi_sem_t *sem, uint32_t timeout);

extern void osi_sem_give(osi_sem_t *sem);

extern int initializeMqtt( MQTTContext_t * pMqttContext, NetworkContext_t * pNetworkContext );

extern int startOTADemo( void );

extern void disconnect( void );

extern int establishConnection(void);

extern int mqttPublish();

extern int mqttSubscribe();

extern int mqttPublishNoMutex();

extern void setOtaInterfaces();

extern void otaAppCallback();

extern void * otaThread();

extern void provisionEventCallback();
//ycc 3-13-22 end
//deep  sleep
static void calibrate_touch_pad(touch_pad_t pad);

float w0=0.0, w1=0.0, w2=0.0, w3=0.0, w4=0.0;         //for bandpath filter
float bpfX, bpfX1;
unsigned short aData;
int peakCounter=0;                                  // counts time after peak is detected 
int detectedBeat = 0;
float bpfS0=0, bpfS1=0, bpfS2=0, bpfS3=0,bpfS4=0, bpfS5=0, bpfS6=0, bpftmp= 0;                    //slope bpfS2 is peak
float mVariance=0, mAvg=100, m0=100, m1=100, m2=100, m3=100, m4=100;           //threshold average of last five peaks
float pVariance=300000, pAvg=0, p0=1000, p1=0, p2=1000, p3=0, p4=1000;           //threshold average of last five peaks
float variance=650;
float nAvg=0, n0=0, n1=0, n2=0, n3=0, n4=0, n5=0, n6=0;   //max noise level
int hb0=0, hb1=0;                                   //last heart beat
float heartRate=0;
float noiseFloor = 0;


unsigned short *dataBuffer;

void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Notification of a time synchronization event");
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
#endif
    sntp_init();
}


static void calibrate_touch_pad(touch_pad_t pad)
{
    int avg = 0;
    const size_t calibration_count = 128;
    for (int i = 0; i < calibration_count; ++i) {
        uint16_t val;
        touch_pad_read(pad, &val);
        avg += val;
    }
    avg /= calibration_count;
    const int min_reading = 300;
    if (avg < min_reading) {
        printf("Touch pad #%d average reading is too low: %d (expecting at least %d). "
               "Not using for deep sleep wakeup.\n", pad, avg, min_reading);
        touch_pad_config(pad, 0);
    } else {
        int threshold = avg - 100;
        printf("Touch pad #%d average: %d, wakeup threshold set to %d.\n", pad, avg, threshold);
        touch_pad_config(pad, threshold);
    }
}

void calibrate_touch_pad_for_pre_provision(touch_pad_t pad) {
    uint16_t touch_value;
    touch_pad_read(pad, &touch_value);
    touch_pad_set_thresh(pad, touch_value * 0.9);  // Set threshold to 90% of the read value
}

static void check_efuse(void)
{
    //Check if TP is burned into eFuse
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP) == ESP_OK) {
        //printf("eFuse Two Point: Supported\n");
    } else {
        //printf("eFuse Two Point: NOT supported\n");
    }
    //Check Vref is burned into eFuse
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_VREF) == ESP_OK) {
        //printf("eFuse Vref: Supported\n");
    } else {
        //printf("eFuse Vref: NOT supported\n");
    }
}

static void print_char_val_type(esp_adc_cal_value_t val_type)
{/*
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        printf("Characterized using Two Point Value\n");
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        printf("Characterized using eFuse Vref\n");
    } else {
        printf("Characterized using Default Vref\n");
    }
    */
}

static bool IRAM_ATTR timer_group_isr_callback(void *args)
{
    BaseType_t high_task_awoken = pdFALSE;
    example_timer_info_t *info = (example_timer_info_t *) args;

    uint64_t timer_counter_value = timer_group_get_counter_value_in_isr(info->timer_group, info->timer_idx);

    /* Prepare basic event data that will be then sent back to task */
 
    example_timer_event_t evt = {
        .info.timer_group = info->timer_group,
        .info.timer_idx = info->timer_idx,
        .info.auto_reload = info->auto_reload,
        .info.alarm_interval = info->alarm_interval,
        .timer_counter_value = timer_counter_value
    };

/*
    if (!info->auto_reload) {
        timer_counter_value += info->alarm_interval * TIMER_SCALE;
        timer_group_set_alarm_value_in_isr(info->timer_group, info->timer_idx, timer_counter_value);
    }
*/
    /* Now just send the event data back to the main program task */
    xQueueSendFromISR(s_timer_queue, &evt, &high_task_awoken);

    return high_task_awoken == pdTRUE; // return whether we need to yield at the end of ISR
}

/**
 * @brief Initialize selected timer of timer group
 *
 * @param group Timer Group number, index from 0
 * @param timer timer ID, index from 0
 * @param auto_reload whether auto-reload on alarm event
 * @param timer_interval_sec interval of alarm
 */
static void ad_tg_timer_init(int group, int timer, bool auto_reload, int timer_interval_sec)
{
    /* Select and initialize basic parameters of the timer */
    timer_config_t config = {
        .divider = TIMER_DIVIDER,
        .counter_dir = TIMER_COUNT_UP,
        .counter_en = TIMER_PAUSE,
        .alarm_en = TIMER_ALARM_EN,
        .auto_reload = auto_reload,
    }; // default clock source is APB
    timer_init(group, timer, &config);

    /* Timer's counter will initially start from value below.
       Also, if auto_reload is set, this value will be automatically reload on alarm */
    timer_set_counter_value(group, timer, 0);

    /* Configure the alarm value and the interrupt on alarm. */
    timer_set_alarm_value(group, timer, 3333);//2000); //3333 for a sampling rate of 300 - every 3.3 msec a interrupt is generated, 2000 for a sampling rate of 500, interrupt at 2 msec
    timer_enable_intr(group, timer);

    example_timer_info_t *timer_info = calloc(1, sizeof(example_timer_info_t));
    timer_info->timer_group = group;
    timer_info->timer_idx = timer;
    timer_info->auto_reload = auto_reload;
    timer_info->alarm_interval = timer_interval_sec;
    timer_isr_callback_add(group, timer, timer_group_isr_callback, timer_info, 0);

    timer_start(group, timer);
}

static int s_retry_num = 0;
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{

    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG, "Provisioning started");
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI(TAG, "Received Wi-Fi credentials"
                         "\n\tSSID     : %s\n\tPassword : %s",
                         (const char *) wifi_sta_cfg->ssid,
                         (const char *) wifi_sta_cfg->password);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s"
                         "\n\tPlease reset to factory and retry provisioning",
                         (*reason == WIFI_PROV_STA_AUTH_ERROR) ?
                         "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful");
                break;
            case WIFI_PROV_END:
                /* De-initialize manager once provisioning is finished */
                wifi_prov_mgr_deinit();
                break;
            default:
                break;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Received WIFI_EVENT_STA_START, esp_wifi_connect()");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        gpio_set_level(LED_RED, 1);
        ecgState = ECG_IDLE;
        /* Signal main application to continue execution */
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {   
                if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
                    esp_wifi_connect();
                    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_EVENT);
                    s_retry_num++;
                    gpio_set_level(LED_RED, 0);
                    ecgState = ECG_ERROR_WIFI;
                    ESP_LOGI(TAG, "retry to connect to the AP");
                } else {
                    xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
                }
                ESP_LOGI(TAG,"connect to the AP failed");
            }
}

static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];

    const char *ssid_prefix = "PROV_";
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X",
             ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

/* Handler for the optional provisioning endpoint registered by the application.
 * The data format can be chosen by applications. Here, we are using plain ascii text.
 * Applications can choose to use other formats like protobuf, JSON, XML, etc.
 */
esp_err_t custom_prov_data_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                          uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    if (inbuf) {
        ESP_LOGI(TAG, "Received data: %.*s", inlen, (char *)inbuf);
    }
    char response[] = "SUCCESS";
    *outbuf = (uint8_t *)strdup(response);
    if (*outbuf == NULL) {
        ESP_LOGE(TAG, "System out of memory");
        return ESP_ERR_NO_MEM;
    }
    *outlen = strlen(response) + 1; /* +1 for NULL terminating byte */

    return ESP_OK;
}

static void wifi_prov_print_qr(const char *name, const char *pop, const char *transport)
{
    if (!name || !transport) {
        ESP_LOGW(TAG, "Cannot generate QR code payload. Data missing.");
        return;
    }
    char payload[150] = {0};
    if (pop) {
        snprintf(payload, sizeof(payload), "{\"ver\":\"%s\",\"name\":\"%s\"" \
                    ",\"pop\":\"%s\",\"transport\":\"%s\"}",
                    PROV_QR_VERSION, name, pop, transport);
    } else {
        snprintf(payload, sizeof(payload), "{\"ver\":\"%s\",\"name\":\"%s\"" \
                    ",\"transport\":\"%s\"}",
                    PROV_QR_VERSION, name, transport);
    }
#ifdef CONFIG_EXAMPLE_PROV_SHOW_QR
    ESP_LOGI(TAG, "Scan this QR code from the provisioning application for Provisioning.");
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    esp_qrcode_generate(&cfg, payload);
#endif // CONFIG_APP_WIFI_PROV_SHOW_QR 
    ESP_LOGI(TAG, "If QR code is not visible, copy paste the below URL in a browser.\n%s?data=%s", QRCODE_BASE_URL, payload);
}

void iot_subscribe_callback_handler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                    IoT_Publish_Message_Params *params, void *pData) {
    ESP_LOGI(TAG, "Subscribe Callback %.*s\t%.*s", topicNameLen, topicName, (int) params->payloadLen, (char *)params->payload);

}

void disconnectCallbackHandler(AWS_IoT_Client *pClient, void *data) {
    ESP_LOGW(TAG, "MQTT Disconnect");
    IoT_Error_t rc = FAILURE;

    if(NULL == pClient) {
        return;
    }
    
    if(aws_iot_is_autoreconnect_enabled(pClient)) {
        ESP_LOGI(TAG, "Auto Reconnect is enabled, Reconnecting attempt will start now");
    } else {
        ESP_LOGW(TAG, "Auto Reconnect not enabled. Starting manual reconnect...");
        rc = aws_iot_mqtt_attempt_reconnect(pClient);
                if(NETWORK_RECONNECTED == rc) {
            ESP_LOGW(TAG, "Manual Reconnect Successful");
        } else {
            ESP_LOGW(TAG, "Manual Reconnect Failed - %d", rc);
        }
    }
}

// Timer callback function to handle provisioning timeout
static void provisioning_timeout_handler(void* arg) {
    ESP_LOGI(TAG, "Provisioning timeout reached. Entering deep sleep...");

    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    // Set reference voltage for charging/discharging
    // In this case, the high reference valtage will be 2.4V - 1V = 1.4V
    // The low reference voltage will be 0.5
    // The larger the range, the larger the pulse count value.

    touch_pad_set_voltage(TOUCH_HVOLT_2V4, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
    
    // Configure the touchpad wake-up source
    touch_pad_config(TOUCH_PAD_NUM9, TOUCH_THRESH_NO_USE);
    calibrate_touch_pad(TOUCH_PAD_NUM9);
    esp_sleep_enable_touchpad_wakeup();
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    esp_deep_sleep_start();
}

// Function to initialize and start the provisioning timer
static void start_provisioning_timer(void) {
    const esp_timer_create_args_t timer_args = {
        .callback = &provisioning_timeout_handler,
        .name = "provisioning_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &provisioning_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(provisioning_timer, PROVISIONING_TIMEOUT_SEC * 1000000));
}

// Function to stop the provisioning timer
static void stop_provisioning_timer(void) {
    ESP_ERROR_CHECK(esp_timer_stop(provisioning_timer));
    ESP_ERROR_CHECK(esp_timer_delete(provisioning_timer));
}

void aws_iot_task(void *param) {
    char cPayload[100];

    short ecgHandsOn;
    unsigned int ecgAcqCounter;
    unsigned int ecgRecCounter;
    unsigned int ecgMqttCounter;
    unsigned short *headBuffer;
    unsigned char *headTxBuffer;
    //unsigned char *mqttTxBuffer;
    unsigned short sequenceTimer;
    unsigned short oldSequenceTimer;
    unsigned short ledSelect;

    char strftime_buf[64]; // for sntp time


    //ycc 031221 end
    ecgState = (short) ECG_IDLE;
    ecgHandsOn = 0;
    ecgAcqCounter = 0;
    ecgRecCounter = 0;
    ecgMqttCounter = 0;
    sequenceTimer = 0;
    oldSequenceTimer=0;
    ledSelect = LED_GREEN;
    jobCheckState = JOB_CHECK_STATE_NOT_CHECKED;

    AWS_IoT_Client client;

    headTxBuffer = (unsigned char *)calloc(21000, sizeof(unsigned char)); //ycc 031022 change to 21000 from 31000
    if(headTxBuffer == NULL)
        {
            ESP_LOGE(TAG, "Failed to allocate data buffer");
            abort();
        }

    headBuffer = (char *)headTxBuffer; //for MAC address, no more MAC address 6->0
    dataBuffer = headBuffer;
    
    //mqttTxBuffer = (unsigned char *)calloc(21000, sizeof(unsigned char)); //ycc 031122 change to 21000 from 31000
    //if(mqttTxBuffer == NULL)
    //    {
    //        ESP_LOGE(TAG, "Failed to allocate mqttTx buffer");
    //        abort();
    //    }


    uint8_t brd_mac[6];
    char topic_name[16];
    const char *topic_prefix = "ecg/";      // set up topic on AWS IoT to take messages with ecg/MACaddress format
    esp_wifi_get_mac(WIFI_IF_STA, brd_mac);
    snprintf(topic_name, 18, "%s%02X%02X%02X%02X%02X%02X",
             topic_prefix, brd_mac[0], brd_mac[1], brd_mac[2], brd_mac[3], brd_mac[4], brd_mac[5]);

    ESP_LOGI(TAG, "calloc return %x", (int)dataBuffer);

    int32_t i = 0;

    IoT_Error_t rc = FAILURE;

    


    /* Wait for WiFI to show as connected */
    int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        false, true, portMAX_DELAY);

    if (!(bits & CONNECTED_BIT)) {
        ESP_LOGE(TAG, "timeout bits=%d\n", bits);
        gpio_set_level(LED_RED, 0);
        ecgState =  ECG_ERROR_WIFI;
    }

    int counter = 0;
    //int counter2=0;
    int counter3 = 0;
    unsigned long deepSleepCounter =10000; // aproximately 33.3 sec before going to sleep 3.3mx10000 = 33
    int debouceCounter = 101;
    int jobCheckCounter = 0;
    //turn off all LEDs so that the LEDs turned on during provisioning can be turned off
    gpio_set_level(LED_GREEN, 1);             //turn on Green LED
    gpio_set_level(LED_BLUE, 1);              //turn off BLUE and RED LEDs
    gpio_set_level(LED_RED, 1);   

    /*
        initialize_sntp();

        // wait for time to be set

        time_t now = 0;
        struct tm timeinfo = { 0 };
        int retry = 0;
        const int retry_count = 10;
        while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
            ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
        time(&now);
        localtime_r(&now, &timeinfo);
        setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0", 1);
        tzset();
        struct tm *tm_struct = localtime_r(&now, &timeinfo);
        strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
        ESP_LOGI(TAG, "%s=  %d", strftime_buf, tm_struct->tm_hour);
    */
while (1) {
        example_timer_event_t evt;
        xQueueReceive(s_timer_queue, &evt, portMAX_DELAY);

        counter++;
        oldSequenceTimer = sequenceTimer;
        sequenceTimer = (unsigned short)counter/30;
        //select LED color based on the state
        if(sequenceTimer!= oldSequenceTimer){
            ledSelect = LED_GREEN;
            switch(ecgState){
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
            gpio_set_level(LED_GREEN, 1);             //turn off all LEDs
            gpio_set_level(LED_BLUE, 1);              
            gpio_set_level(LED_RED, 1);   

            gpio_set_level(ledSelect, (uint32_t)blinkingPattern[ecgState][sequenceTimer]); //set the selected LED on

        }
        counter3++;
        jobCheckCounter++;
        if (counter >300) 
            {
            //printf("-----------------------------------------------------------------counter= %d----------------------------------------------\n", counter2);
            counter = 0; //this counter is used for LED sequencing
            //ycc 032122

            //uint32_t adc_reading = 0;
            //Multisampling
            //for (int i = 0; i < NO_OF_SAMPLES; i++) 
            //    adc_reading += adc1_get_raw((adc1_channel_t)channel);
       
            //adc_reading /= NO_OF_SAMPLES;
            //Convert adc_reading to voltage in mV
            //uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);
            //printf("Raw: %d\tVoltage: %dmV\n", adc_reading, voltage);
            //ESP_LOGI(TAG, "%f,%f,%f,%f, ecgstate=%d", bpfX, bpfX1, bpftmp, variance, ecgState);
            //if(ecgState != ECG_IDLE)
            uint32_t fs = xPortGetFreeHeapSize();
            ESP_LOGI(TAG, "ecg state = %d, Stack remaining for task '%s' is %d bytes, free mem is %d", ecgState, pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL), fs);
/*
            time(&now);
            //localtime_r(&now, &timeinfo);
            struct tm *tm_struct = localtime_r(&now, &timeinfo);
            strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
            ESP_LOGI(TAG, "%s=  %d", strftime_buf, tm_struct->tm_hour);
            ESP_LOGI(TAG, "%f,%f,%f,%f, ecgstate = %d\n", bpfX, bpfX1, bpftmp, variance, ecgState);
            if((tm_struct->tm_hour==0)&&(tm_struct->tm_min == 0)&&(tm_struct->tm_sec<=3))
                jobCheckState = JOB_CHECK_STATE_NOT_CHECKED;
            // if time is past midnight and before 2am check for job update
            if((tm_struct->tm_hour == 0) && (jobCheckState == JOB_CHECK_STATE_NOT_CHECKED)){
                gpio_set_level(LED_GREEN, 1);             //turn on Green LED
                gpio_set_level(LED_BLUE, 1);              //turn off BLUE 
                gpio_set_level(LED_RED,0);                //turn on RED LED
                aws_iot_demo_main(0, NULL);     // iot job has built in function to make sure it is executed so don't worry about ret          
                jobCheckState =  JOB_CHECK_STATE_CHECKED_NO_UPDATE; //check to see if there is an update
                jobCheckCounter = 0;
                gpio_set_level(LED_GREEN, 1);             //turn on Green LED
                gpio_set_level(LED_BLUE, 1);              //turn off BLUE 
                gpio_set_level(LED_RED,1);
                }
                */
            }

        if(((gpio_get_level((gpio_num_t)LOPlus) == 1) || (gpio_get_level((gpio_num_t)LOMinus) == 1)) && (debouceCounter >100))            //ycc 022022 debounce
            {
            pVariance=300000; pAvg=300; p0=1000; p1=0; p2=1000; p3=0; p4=1000;  
            variance = 1000;
            //counter = 0;
            //counter3=0;
            ecgHandsOn = 0;

            //if((gpio_get_level((gpio_num_t)LOPlus) == 1) && (gpio_get_level((gpio_num_t)LOMinus) == 0))
            //    {
            //        ecgState = ECG_SSID_RESET;
            //    }
            }
        else
            {
            //ycc 022022
            if(debouceCounter<=100){
                debouceCounter++;
            }
            else {
                debouceCounter = 0;
            }
            ecgHandsOn = 1;
            gpio_set_level(LED_RED, 1);
            // send the value of analog input 0:
            //Data = 0;
            //for (int i = 0; i < NO_OF_SAMPLES; i++) 
            aData = adc1_get_raw((adc1_channel_t)channel);//2045*sin(counter/10) + 2048; //for testing comment out 
            //aData = aData/NO_OF_SAMPLES;
            if((aData<4090) && (aData>6))
                bpfX = (float)aData;     //Discount saturarted data, fill it with the last point
            /*
            w0 = 3.336612*w1 -4.225986*w2+ 2.425819*w3 - 0.537195*w4 + bpfX;
            bpfX1 = 0.036575*(w0 - 2.0*w2 + w4);
            w4 = w3;
            w3 = w2;
            w2 = w1;
            w1 = w0;
            */
            w0 = 3.269793*w1 -4.169410*w2+ 2.523669*w3 - 0.624207*w4 + bpfX;
            bpfX1 = 0.082619*(w0 - 2.0*w2 + w4);
            w4 = w3;
            w3 = w2;
            w2 = w1;
            w1 = w0;
            w0 = 3.000162*w1 -3.294086*w2+ 1.577024*w3 - 0.283222*w4 + bpfX1;
            bpfX1 = 0.065274*(w0 - 2.0*w2 + w4);
            w4 = w3;
            w3 = w2;
            w2 = w1;
            w1 = w0;
            //end-band pass filter
          
            bpfS0=bpfS1;
            bpfS1=bpfS2;
            bpfS2=bpfS3;
            bpfS3=bpfS4;    
            bpfS4=bpfS5;
            bpfS5=bpfS6;
            bpfS6=bpfX1;
                          
            if((bpfS3>(mAvg+noiseFloor)/2) &&(peakCounter<750) && (peakCounter>100)&& ((bpfS3>bpfS2)&& (bpfS3>bpfS4)) && (((bpfS3-bpfS0)>3)&& ((bpfS3-bpfS6)>3)) && (bpfS3<1500))
                {
                //heart beat detected
          
                heartRate = 30000/peakCounter;
            
                m0=m1;
                m1=m2;
                m2=m3;
                m3=m4;
                //if(bpfS3> 40 && bpfS3<150) 
                m4=bpfS3;
                mAvg = (m0+m1+m2+m3+m4)/5;
                //mVariance = (m0-mAvg)*(m0-mAvg)+(m1-mAvg)*(m1-mAvg)+(m2-mAvg)*(m2-mAvg)+(m3-mAvg)*(m3-mAvg)+(m4-mAvg)*(m4-mAvg);
                p0=p1;
                p1=p2;
                p2=p3;
                p3=p4;
                p4=peakCounter;
            
                pAvg = (p0+p1+p2+p3+p4)/5;
                pVariance = ((p0-pAvg)*(p0-pAvg)+(p1-pAvg)*(p1-pAvg)+(p2-pAvg)*(p2-pAvg)+(p3-pAvg)*(p3-pAvg)+(p4-pAvg)*(p4-pAvg))/5;
                variance = sqrt(pVariance);
                    
                //digitalWrite(LEDPin,counter);
                detectedBeat++;
                bpftmp = bpfS3;
                peakCounter = 0;
                }
            else
                {
                if(peakCounter> 5000)
                   {
                    peakCounter = 0;
                    detectedBeat--;
                    }
             
                peakCounter++; 
                if((bpfS3>bpfS2) && (bpfS3>bpfS4))
                    {
                  
                    noiseFloor = (noiseFloor+bpfS3)/2;
                    if(peakCounter>500)mAvg=0.9*mAvg;
                    }
                bpftmp = 0;
                } //end if-else

            }//end if-else
        //state machine 

        switch(ecgState)
            {
            case ECG_IDLE:
                if(ecgHandsOn == 1){
                    //printf("IDLE move to ACQUIRING\n");
                    ecgState = ECG_ACQUIRING;
                    //start ACQ timer
                    ecgAcqCounter = 0;
                    ecgRecCounter = 0;
                    ecgMqttCounter = 0;
                    dataBuffer = headBuffer;
                    deepSleepCounter = 10000;
                }
                else {
                    deepSleepCounter--;
                    if(deepSleepCounter == 0){
                         //deep sleep init
                        // Initialize touch pad peripheral.
                        // The default fsm mode is software trigger mode.

                        ESP_ERROR_CHECK(touch_pad_init());
                        // If use touch pad wake up, should set touch sensor FSM mode at 'TOUCH_FSM_MODE_TIMER'.
                        
                        touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
                        // Set reference voltage for charging/discharging
                        // In this case, the high reference valtage will be 2.4V - 1V = 1.4V
                        // The low reference voltage will be 0.5
                        // The larger the range, the larger the pulse count value.

                        touch_pad_set_voltage(TOUCH_HVOLT_2V4, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
                        
                        //init RTC IO and mode for touch pad.
                        // touch pad number 9 is GPIO 32

                        touch_pad_config(TOUCH_PAD_NUM9, TOUCH_THRESH_NO_USE);

                        calibrate_touch_pad(TOUCH_PAD_NUM9);

                        printf("Enabling touch pad wakeup\n");
                        esp_sleep_enable_touchpad_wakeup();
                        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

                        // Isolate GPIO12 pin from external circuits. This is needed for modules
                        // which have an external pull-up resistor on GPIO12 (such as ESP32-WROVER)
                        // to minimize current consumption.
                        rtc_gpio_isolate(GPIO_NUM_12);
                        gpio_set_level(SDN, 0);             //disable AD8232 operaion 
                        printf("Entering deep sleep\n");
                        esp_deep_sleep_start();  
                    }
                }
                break;
            case ECG_ACQUIRING:
                ecgAcqCounter++;
                if(ecgHandsOn == 0)
                    {
                    //hands off go back to IDLE
                    printf("ACQUIRING move to IDLE due to echHandsOff\n");
                    ecgState = ECG_IDLE;
                    ecgAcqCounter = 0;
                    }
                else if (ecgAcqCounter >ECG_ACQCOUNT)
                    {//can't recognize ecg signal, start recording just for troubleshooting 
                    printf("ACQUIRING move to RECORDING due to ACQ timeout\n");
                    ecgState = ECG_RECORDING;
                    //start recording timer
                    ecgRecCounter = 0;
                    //set up pointer in the buffer for data to start recording
                    }
                else if (variance < 100)
                    {
                    //printf("variance less than one hundred ACQUIRING move to RECORDING\n");
                    ecgState = ECG_RECORDING;
                    //start recording timer
                    ecgRecCounter = 0;
                    //ecg signal recognized, move to recording state
                    //set up pointer in the buffer for data
                    }
                break;
            case ECG_RECORDING:
                    //dataBuffer[ecgRecCounter] = aData;
                    *dataBuffer++ = aData;//ecgRecCounter;
                    ecgRecCounter++;
                    if(ecgHandsOn == 0)
                        {   //hands off but there are enough samples to send so go ahead and send them
                            //printf("ECG_RECORDING: HANDS_OFF, ");
                            if (ecgRecCounter >ECG_RECCOUNT)
                                { //Set up MQTT}
                                    //printf("Count is greater than RECCOUNT, RECORDING MOVE TO SEND MQTT\n");
                                    ecgState = ECG_SENDING_MQTT;
                                    //ecgRecCounter = 0;
                                    ecgMqttCounter = 0;
                                }
                        else
                        //hands off go back to IDLE
                            {
                            ecgState = ECG_IDLE;
                            //printf("ECG_RECORDING mvoe to IDLE else clause\n");
                            }
                        }
                    else if (ecgRecCounter >=ECG_RECCOUNT)
                        {//finish timed recording, send data
                            //printf("ecgRecCounter > ECG_RECCOUNT, RECORDING MOVE TO SEND MQTT\n");
                            ecgState = ECG_SENDING_MQTT;
                            //ecgRecCounter = 0;
                            ecgMqttCounter = 0;
                            dataBuffer = headBuffer;
                        }

                break;
            case ECG_SENDING_MQTT:
                //AWS_IoT_Client client;
                rc = FAILURE;
                gpio_set_level(LED_BLUE, 0);  
                //ycc 3-13-22 added the following from aws_iot_demo_main
                /* Return error status. */
                int returnStatus = EXIT_SUCCESS;


                if( returnStatus == EXIT_SUCCESS )
                {
                    /* Start OTA demo. */
                    //returnStatus = startOTADemo();
                    //ycc 031422
                    uint32_t free_heap_size=0, min_free_heap_size=0;
                    free_heap_size = esp_get_free_heap_size();
                    min_free_heap_size = esp_get_minimum_free_heap_size(); 
                    //printf("\n free heap size = %d \t  min_free_heap_size = %d \n",free_heap_size,min_free_heap_size);
                    uint32_t fs = xPortGetFreeHeapSize();
                    ESP_LOGI(TAG, "Send MQTT Stack remaining for task '%s' is %d bytes at counter, free mem is %d", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL), fs);
                    if( mqttSessionEstablished != true ){
                        int ret = establishConnection();
                        //printf("establish connection return = %d\n", ret);
                        //printf("Publish....");
                    }
                    if( mqttSessionEstablished == true ){

                        unsigned short totalLen = 2*ecgRecCounter;
                        char topic_name[16];
                        const char *topic_prefix = "ecg/";  
                        uint8_t brd_mac[6];
                        esp_wifi_get_mac(WIFI_IF_STA, brd_mac);
                        snprintf(topic_name, 18, "%s%02X%02X%02X%02X%02X%02X",
                            topic_prefix, brd_mac[0], brd_mac[1], brd_mac[2], brd_mac[3], brd_mac[4], brd_mac[5]);
                        const char *TOPIC = topic_name;
                        const int TOPIC_LEN = strlen(TOPIC);
                        mqttPublishNoMutex(TOPIC, TOPIC_LEN,
                                        headBuffer,
                                        totalLen,
                                        QOS0);


                    }
                

                    
                }

                //ycc end 3-13-22
                //ycc 031422 end
                disconnect();
                ecgState =  ECG_FINISH;
                ecgRecCounter = 0;
                jobCheckCounter = 0;
                
                ecgMqttCounter++;
                break;
            case ECG_FINISH:
                if(ecgHandsOn == 0){
                    ecgState = ECG_IDLE;
                    jobCheckCounter = 0;
                    break;
                }
                else {

                        if(jobCheckCounter<6000){
                            ecgState = ECG_FINISH;
                            break;
                        }
                        else{
                            gpio_set_level(LED_GREEN, 1);             //turn on Green LED
                            gpio_set_level(LED_BLUE, 1);              //turn off BLUE 
                            gpio_set_level(LED_RED,0);                //turn on RED LED

                            //ESP_LOGI(TAG, "aws_iot_demo main entry point: Stack remaining for task '%s' is %d bytes at counter, free mem is %d", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL), xPortGetFreeHeapSize());
                            // ycc 031222 commentned out the next  four lines
                            //int ret = aws_iot_demo_main(0, NULL);   
                            //ESP_LOGI(TAG, "aws_iot_demo main exit point: Stack remaining for task '%s' is %d bytes at counter, free mem is %d", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL), xPortGetFreeHeapSize());              
                            //if(ret==EXIT_SUCCESS) 
                            //    jobCheckState =  JOB_CHECK_STATE_CHECKED_NO_UPDATE; //check to see if there is an update
                            jobCheckCounter = 0;
                            ecgState = ECG_FINISH;
                            gpio_set_level(LED_GREEN, 1);             //turn on Green LED
                            gpio_set_level(LED_BLUE, 1);              //turn off BLUE 
                            gpio_set_level(LED_RED,1);
                              
                            break;
                        }

                }
                break;
            case ECG_OTA_UPDATE:
                ecgState = ECG_OTA_UPDATE;
                break;
            case ECG_SSID_RESET:
                if(counter3 <3000)
                    break;
                else   
                    {
                    ecgState = ECG_IDLE;
                    counter3 = 0;
                    }
                break;
            default:
                //printf("Default move to IDLE\n");
                ecgState = ECG_IDLE;
                break;
            }

    }//end while

    ESP_LOGE(TAG, "An error occurred in the main loop.");
    abort();
}


void app_main()
{
    // Initialize touch pad
    touch_pad_init();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    /*
    printf("This is %s chip with %d CPU cores, WiFi%s%s, ",
            CHIP_NAME,
            chip_info.cores,
            (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
            (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");
    */
    //printf("silicon revision %d, ", chip_info.revision);

    //printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
    //        (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    //072521 ycc added the following line to initialize buttons for erasing flash;
    app_driver_init();

    //printf("Configured WiFi SSID is %s\n", CONFIG_WIFI_SSID);
    // Initialize NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );


    s_timer_queue = xQueueCreate(10, sizeof(example_timer_event_t));
    //GPIO setup
    gpio_reset_pin(32);
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_DEFAULT);
    gpio_set_direction(LOPlus, GPIO_MODE_INPUT);
    gpio_set_direction(LOMinus, GPIO_MODE_INPUT);
    gpio_set_direction(SDN, GPIO_MODE_OUTPUT);
    gpio_set_direction(FR, GPIO_MODE_OUTPUT);
    gpio_set_direction(DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_GREEN, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_BLUE, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_RED, GPIO_MODE_OUTPUT);

    gpio_set_direction(DEEPSLEEP, GPIO_MODE_INPUT);

    gpio_set_level(LED_GREEN, 1);             //turn on Green LED
    gpio_set_level(LED_BLUE, 1);              //turn off BLUE and RED LEDs
    gpio_set_level(LED_RED, 1);   
    gpio_set_level(SDN, 1);             //enable AD8232 operaion
    gpio_set_level(FR, 1);              //turn on fast recovery
    gpio_set_level(DC, 0);              //select dc operation for AD8232

    //gpio_set_level(SDN, 0);             //disable AD8232 operaion for testing so that it will stay in IDLE
    //ESP_ERROR_CHECK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TOUCHPAD));
    //ESP_ERROR_CHECK(touch_pad_deinit());

    ad_tg_timer_init(TIMER_GROUP_0, TIMER_0, true,1);
    /*example_tg_timer_init(TIMER_GROUP_1, TIMER_0, false, 5);*/
    //Check if Two Point or Vref are burned into eFuse
    check_efuse();

    //Configure ADC
    adc1_config_width(width);
    adc1_config_channel_atten(channel, atten);


    //Characterize ADC
    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(unit, atten, width, DEFAULT_VREF, adc_chars);
    print_char_val_type(val_type);

   
    /* 072221 ycc */
    tcpip_adapter_init(); // move from initialize_wifi to here

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_event_group = xEventGroupCreate();

  /* Register our event handler for Wi-Fi, IP and Provisioning related events */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    //072321 ycc commented out the following line
    //ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );


    //initialise_wifi();
    /* 072221 ycc add Configuration for the provisioning manager */
    wifi_prov_mgr_config_t config = {
        /* What is the Provisioning Scheme that we want ?
         * wifi_prov_scheme_softap or wifi_prov_scheme_ble */
        .scheme = wifi_prov_scheme_ble,

        /* Any default scheme specific event handler that you would
         * like to choose. Since our example application requires
         * neither BT nor BLE, we can choose to release the associated
         * memory once provisioning is complete, or not needed
         * (in case when device is already provisioned). Choosing
         * appropriate scheme specific event handler allows the manager
         * to take care of this automatically. This can be set to
         * WIFI_PROV_EVENT_HANDLER_NONE when using wifi_prov_scheme_softap*/
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM
    

    };

    /* Initialize provisioning manager with the
     * configuration parameters set above */
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

    bool provisioned = false;
    /* Let's find out if the device is provisioned */
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    /* If device is not yet provisioned start provisioning service */
    if (!provisioned) {
        ESP_LOGI(TAG, "Starting provisioning");
        gpio_set_level(LED_RED, 0); //turn on RED LED to indicate the state of not provisioned. 
        /* What is the Device Service Name that we want
         * This translates to :
         *     - Wi-Fi SSID when scheme is wifi_prov_scheme_softap
         *     - device name when scheme is wifi_prov_scheme_ble
         */

        start_provisioning_timer(); // Start the provisioning timer

        char service_name[12];
        get_device_service_name(service_name, sizeof(service_name));
        /* What is the security level that we want (0 or 1):
         *      - WIFI_PROV_SECURITY_0 is simply plain text communication.
         *      - WIFI_PROV_SECURITY_1 is secure communication which consists of secure handshake
         *          using X25519 key exchange and proof of possession (pop) and AES-CTR
         *          for encryption/decryption of messages.
         */
        wifi_prov_security_t security = WIFI_PROV_SECURITY_1;

        /* Do we want a proof-of-possession (ignored if Security 0 is selected):
         *      - this should be a string with length > 0
         *      - NULL if not used
         */
        const char *pop = "abcd1234";

        /* What is the service key (could be NULL)
         * This translates to :
         *     - Wi-Fi password when scheme is wifi_prov_scheme_softap
         *     - simply ignored when scheme is wifi_prov_scheme_ble
         */
        const char *service_key = NULL;


        /* This step is only useful when scheme is wifi_prov_scheme_ble. This will
         * set a custom 128 bit UUID which will be included in the BLE advertisement
         * and will correspond to the primary GATT service that provides provisioning
         * endpoints as GATT characteristics. Each GATT characteristic will be
         * formed using the primary service UUID as base, with different auto assigned
         * 12th and 13th bytes (assume counting starts from 0th byte). The client side
         * applications must identify the endpoints by reading the User Characteristic
         * Description descriptor (0x2901) for each characteristic, which contains the
         * endpoint name of the characteristic */
        uint8_t custom_service_uuid[] = {
            /* LSB <---------------------------------------
             * ---------------------------------------> MSB */
            0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
            0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
        };
        wifi_prov_scheme_ble_set_service_uuid(custom_service_uuid);


        /* An optional endpoint that applications can create if they expect to
         * get some additional custom data during provisioning workflow.
         * The endpoint name can be anything of your choice.
         * This call must be made before starting the provisioning.
         */
        wifi_prov_mgr_endpoint_create("custom-data");
        /* Start provisioning service */
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key));

        /* The handler for the optional endpoint created above.
         * This call must be made after starting the provisioning, and only if the endpoint
         * has already been created above.
         */
        wifi_prov_mgr_endpoint_register("custom-data", custom_prov_data_handler, NULL);


        wifi_prov_print_qr(service_name, pop, PROV_TRANSPORT_BLE);

        wifi_prov_mgr_wait();
        wifi_prov_mgr_deinit();
        /* Print QR code for provisioning */

        //wifi_prov_print_qr(service_name, pop, PROV_TRANSPORT_BLE);
        stop_provisioning_timer(); // Stop the provisioning timer once provisioning is done

        //wifi_prov_print_qr(service_name, pop, PROV_TRANSPORT_BLE);
    } else {    
         ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");

        /* We don't need the manager as device is already provisioned,
         * so let's release it's resources */
        wifi_prov_mgr_deinit();
        ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
        ESP_ERROR_CHECK( esp_wifi_start() );

    }
    /* ycc 030922 added to Wait for WiFI to show as connected */
    int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        false, true, portMAX_DELAY);

    if (!(bits & CONNECTED_BIT)) {
        ESP_LOGE(TAG, "timeout bits=%d\n", bits);
        gpio_set_level(LED_RED, 0);
    }
 
    //ycc 051722 get board mac address, this can only be done after wifi is established
    //get mac address
    uint8_t brd_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, brd_mac);
    snprintf(macAddress, 13, "%02X%02X%02X%02X%02X%02X", brd_mac[0], brd_mac[1], brd_mac[2], brd_mac[3], brd_mac[4], brd_mac[5]);
    //printf("macAddress is %s, length is %d\n", macAddress,strlen(macAddress));
    //ycc 3-15-22 initialize mqtt and 
    /* Return error status. */
    int returnStatus = EXIT_SUCCESS;
    /* Semaphore initialization flag. */
    bool bufferSemInitialized = false;
    bool mqttMutexInitialized = false;

    mqttSessionEstablished = false;

    /* Initialize semaphore for buffer operations. */
    if( osi_sem_new( &bufferSemaphore, 0x7FFFU, 1 ) != 0 )
    {
        LogError( ( "Failed to initialize buffer semaphore"
                    ",errno=%s",
                    strerror( errno ) ) );

        returnStatus = EXIT_FAILURE;
    }
    else
    {
        bufferSemInitialized = true;
    }

    /* Initialize mutex for coreMQTT APIs. */
    if( pthread_mutex_init( &mqttMutex, NULL ) != 0 )
    {
        LogError( ( "Failed to initialize mutex for mqtt apis"
                    ",errno=%s",
                    strerror( errno ) ) );

        returnStatus = EXIT_FAILURE;
    }
    else
    {
        mqttMutexInitialized = true;
    }

    if( returnStatus == EXIT_SUCCESS )
    {
        /* Initialize MQTT library. Initialization of the MQTT library needs to be
            * done only once in this demo. */
        returnStatus = initializeMqtt( &mqttContext, &networkContext );
    }
    //aws_iot_task(&aws_iot_task);
    //xTaskCreatePinnedToCore(&aws_iot_task, "aws_iot_task", 9216, NULL, 5, NULL, 1); //ycc 031022 change from 19216

    //ycc 070522 check nvs to see if provisioned for x.509
    printf("Opening Non-Volatile Storage (NVS) handle... ");
    //nvs_handle_t fleet_prov_handle;
    err = nvs_open("storage", NVS_READWRITE, &fleet_prov_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        printf("Done\n");

        // Read
        printf("Reading fleet prov certificate from NVS ... ");
        //size_t private_key_len;
        err = nvs_get_str(fleet_prov_handle, "privateKey", NULL, &private_key_len);
        private_key = malloc(private_key_len);
        err = nvs_get_str(fleet_prov_handle, "privateKey", private_key, &private_key_len);
        switch (err) {
            case ESP_OK:
                printf("Done\n");
                //printf("private_key = %s\n", private_key);
                nvsProvisionStatus = true;
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                printf("The value is not initialized yet!\n");
                nvsProvisionStatus = false;
                break;
            default :
                printf("Error (%s) reading!\n", esp_err_to_name(err));
                nvsProvisionStatus = false;
        }
        //size_t certificate_pem_len;
        err = nvs_get_str(fleet_prov_handle, "certificatePem", NULL, &certificate_pem_len);
        certificate_pem = malloc(certificate_pem_len);
        if(nvsProvisionStatus == true)
            {
            err = nvs_get_str(fleet_prov_handle, "certificatePem", certificate_pem, &certificate_pem_len);
            switch (err) {
                case ESP_OK:
                    printf("Done\n");
                    //printf("certificate_pem = %s\n", certificate_pem);
                    nvsProvisionStatus = true;
                    break;
                case ESP_ERR_NVS_NOT_FOUND:
                    printf("The value is not initialized yet!\n");
                    nvsProvisionStatus = false;
                    break;
                default :
                    printf("Error (%s) reading!\n", esp_err_to_name(err));
                    nvsProvisionStatus = false;
                }
            }
    }
    //ycc 090222
    if (nvsProvisionStatus == true) {
        //nvs has credentials, go ahead and connect to the server
        typedef struct Data_t
            {
            uint32_t uData;
            char **id;
            } arg_params_t;

        arg_params_t data1 = {0, NULL};
        uint32_t free_heap_size=0, min_free_heap_size=0;
        free_heap_size = esp_get_free_heap_size();
        min_free_heap_size = esp_get_minimum_free_heap_size(); 
        printf("\n free heap size = %d \t  min_free_heap_size = %d \n",free_heap_size,min_free_heap_size); 
        
        xTaskCreatePinnedToCore((TaskFunction_t) &aws_iot_demo_main, "aws_iot_demo_main", 9216, (void *)&data1, 5, NULL, 1);
        aws_iot_task(&aws_iot_task);
        /* Disconnect from broker and close connection. */
    }
    else
    {
        //ycc 062222 for fleet provisioning
        if( mqttSessionEstablished != true ){
            int ret = establishConnection();
            printf("establish connection return = %d\n", ret);
                            //printf("Publish....");
        }
        if( mqttSessionEstablished == true ){
                
                const char *topic = "$aws/certificates/create/json/accepted";
                OtaMqttStatus_t mqttReturn = mqttSubscribe(topic, strlen(topic), 1);
                printf("mqttreturn 0= %d\n", mqttReturn);
                const char *topic1 = "$aws/certificates/create/json/rejected";
                mqttReturn = mqttSubscribe(topic1, strlen(topic1), 1);
                printf("mqttreturn 1= %d\n", mqttReturn);
                const char *topic2 = "$aws/provisioning-templates/testP2/provision/json/accepted";
                mqttReturn = mqttSubscribe(topic2, strlen(topic2), 1);
                printf("mqttreturn 2= %d\n", mqttReturn);
                const char *topic3 = "$aws/provisioning-templates/testP2/provision/json/rejected";
                mqttReturn = mqttSubscribe(topic3, strlen(topic3), 1);
                printf("mqttreturn 3= %d\n", mqttReturn);
                const char *topic4 = "$aws/certificates/create/json";
                mqttReturn = mqttPublish(topic4, strlen(topic4), NULL, 0,  1);
                printf("mqttreturn 4= %d\n", mqttReturn);
                //vTaskDelay(300);
                // registering with the token just received
                const char *topic5 = "$aws/provisioning-templates/testP2/provision/json";
                printf("lenghth of registrationBuff = %d\n", strlen(registrationBuff));
                mqttReturn = mqttPublish(topic5, strlen(topic5), registrationBuff,strlen(registrationBuff),  1);
                printf("mqttreturn 5= %d\n", mqttReturn);
                nvsProvisionStatus = true;


                MQTTStatus_t mqttStatus = MQTTBadParameter;
                {
                                {
                    /* Acquire the mqtt mutex lock. */
                    if( pthread_mutex_lock( &mqttMutex ) == 0 )
                    {
                        /* Loop to receive packet from transport interface. */
                        mqttStatus = MQTT_ProcessLoop( &mqttContext, MQTT_PROCESS_LOOP_TIMEOUT_MS );

                        pthread_mutex_unlock( &mqttMutex );
                    }
                    else
                    {
                        LogError( ( "Failed to acquire mutex to execute process loop"
                                    ",errno=%s",
                                    strerror( errno ) ) );
                    }

                    if( mqttStatus == MQTTSuccess )
                    {
                        /* Get OTA statistics for currently executing job. */
                        /* Delay if mqtt process loop is set to zero.*/
                        if( MQTT_PROCESS_LOOP_TIMEOUT_MS > 0 )
                        {
                            vTaskDelay( 500 );
                        }
                        printf("Delay task in MQTT_Processloop\n");
                    }
                    else
                    {
                        LogError( ( "MQTT_ProcessLoop returned with status = %s.",
                                    MQTT_Status_strerror( mqttStatus ) ) );

                        /* Disconnect from broker and close connection. */
                        disconnect();


                    }
                    // Read
                    printf("Reading fleet prov certificate from NVS ... ");
                    
                    err = nvs_get_str(fleet_prov_handle, "privateKey", NULL, &private_key_len);
                    private_key = malloc(private_key_len);
                    err = nvs_get_str(fleet_prov_handle, "privateKey", private_key, &private_key_len);
                    switch (err) {
                        case ESP_OK:
                            printf("Done\n");
                            //printf("private_key = %s\n", private_key);
                            break;
                        case ESP_ERR_NVS_NOT_FOUND:
                            printf("The value is not initialized yet!\n");
                            break;
                        default :
                            printf("Error (%s) reading!\n", esp_err_to_name(err));
                    }
                    
                    err = nvs_get_str(fleet_prov_handle, "certificatePem", NULL, &certificate_pem_len);
                    certificate_pem = malloc(certificate_pem_len);
                    err = nvs_get_str(fleet_prov_handle, "certificatePem", certificate_pem, &certificate_pem_len);
                    switch (err) {
                        case ESP_OK:
                            printf("Done\n");
                            //printf("certificate_pem = %s\n", certificate_pem);
                            break;
                        case ESP_ERR_NVS_NOT_FOUND:
                            printf("The value is not initialized yet!\n");
                            break;
                        default :
                            printf("Error (%s) reading!\n", esp_err_to_name(err));
                    }
                    disconnect();
                    printf("reconnecting using the new credentials\n");
                    //ycc 062222 for fleet provisioning
                    if( mqttSessionEstablished != true ){
                        int ret = establishConnection();
                        printf("establish connection return = %d\n", ret);
                            //printf("Publish....");
                    }

                }
            }

        }
    }
    disconnect();

    

    if( bufferSemInitialized == true )
    {
        /* Cleanup semaphore created for buffer operations. */
        if( osi_sem_free( &bufferSemaphore ) != 0 )
        {
            LogError( ( "Failed to destroy buffer semaphore"
                        ",errno=%s",
                        strerror( errno ) ) );

            returnStatus = EXIT_FAILURE;
        }
    }

    if( mqttMutexInitialized == true )
    {
        /* Cleanup mutex created for MQTT operations. */
        if( pthread_mutex_destroy( &mqttMutex ) != 0 )
        {
            LogError( ( "Failed to destroy mutex for mqtt apis"
                        ",errno=%s",
                        strerror( errno ) ) );

            returnStatus = EXIT_FAILURE;
        }
    }
    esp_restart();
}
