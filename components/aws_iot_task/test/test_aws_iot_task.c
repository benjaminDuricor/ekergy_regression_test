#include "unity.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "aws_iot_task.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "core_mqtt.h"
#include "mqtt_subscription_manager.h"
#include "shared.h"
#include "esp_sleep.h"
#include "driver/timer.h"
#include "driver/gpio.h"  // Include GPIO header
#include "driver/adc.h"

// #ifdef TEST_ENV
// // Global variable to track if esp_deep_sleep_start was called
// static volatile bool deep_sleep_called = false;

// // Mocked esp_deep_sleep_start function
// __attribute__((weak)) void esp_deep_sleep_start() {
//     ESP_LOGI("TEST", "Mocking deep sleep. Not entering deep sleep.");
// }
// #endif

static const char *TAG = "test_aws_iot_task";

extern int aws_iot_demo_main();
typedef xSemaphoreHandle osi_sem_t;
osi_sem_t bufferSemaphore;

short nvsProvisionStatus = false;
extern int initializeMqtt(MQTTContext_t *pMqttContext, NetworkContext_t *pNetworkContext);
extern int startOTADemo(void);
extern void disconnect(void);
extern int establishConnection(void);
extern int mqttPublish();
extern int mqttSubscribe();
extern MQTTContext_t mqttContext;
extern char *registrationBuff;
// extern unsigned long deepSleepCounter;
pthread_mutex_t mqttMutex;

extern NetworkContext_t networkContext;

char *private_key = NULL;
size_t private_key_len = 0;
char *certificate_pem = NULL;
size_t certificate_pem_len = 0;
nvs_handle_t fleet_prov_handle;
TaskHandle_t aws_task_handle = NULL; // Task handle for aws_iot_task

bool deep_sleep_test_switch = false;
bool rec_mqtt_switch = false;

#define MQTT_PROCESS_LOOP_TIMEOUT_MS (1500U)
// #define SimPlus 4   // Simulated output GPIO for testing
// #define SimMinus 5  // Simulated output GPIO for testing
#define TIMER_DIVIDER (80)

#ifdef TEST_ENV
extern int test_counter;  // Declare the test-specific variable as extern
#endif

// Global flag to monitor deep sleep
bool deep_sleep_called = false;

void mock_esp_deep_sleep_start() {
    deep_sleep_called = true;
    esp_sleep_wakeup_cause_t wakeup_cause = ESP_SLEEP_WAKEUP_TOUCHPAD;

}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    }
}

void wifi_init_sta() {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Check if Wi-Fi STA has already been created
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    // wifi_config_t wifi_config = {
    //     .sta = {
    //         .ssid = "TP-LINK_1DE6",
    //         .password = "1513679125877",
    //     },
    // };

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "FliportgardenHotel",
            .password = "",
            .threshold.authmode = WIFI_AUTH_OPEN, // Set the authentication mode to open for a password-less network
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait until connected
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

void cleanup_test_environment() {
    // Delete the queue if it was created
    if (s_timer_queue != NULL) {
        vQueueDelete(s_timer_queue);
        s_timer_queue = NULL;
    }

    // Destroy the MQTT mutex
    if (pthread_mutex_destroy(&mqttMutex) != 0) {
        ESP_LOGE(TAG, "Failed to destroy mutex for mqtt apis, errno=%s", strerror(errno));
    }

    // Clean up Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());
    esp_netif_destroy_default_wifi(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));

    // Clean up event loop
    ESP_ERROR_CHECK(esp_event_loop_delete_default());

    // Free any allocated memory (e.g., headTxBuffer)
    if (headTxBuffer != NULL) {
        free(headTxBuffer);
        headTxBuffer = NULL;
    }

    // Clean up NVS
    nvs_flash_deinit();

    ESP_LOGI(TAG, "Test environment cleaned up.");
}

static bool IRAM_ATTR timer_group_isr_callback(void *args)
{
    BaseType_t high_task_awoken = pdFALSE;
    example_timer_info_t *info = (example_timer_info_t *)args;

    uint64_t timer_counter_value = timer_group_get_counter_value_in_isr(info->timer_group, info->timer_idx);

    /* Prepare basic event data that will be then sent back to task */

    example_timer_event_t evt = {
        .info.timer_group = info->timer_group,
        .info.timer_idx = info->timer_idx,
        .info.auto_reload = info->auto_reload,
        .info.alarm_interval = info->alarm_interval,
        .timer_counter_value = timer_counter_value};

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
    timer_set_alarm_value(group, timer, 3333); // 2000); //3333 for a sampling rate of 300 - every 3.3 msec a interrupt is generated, 2000 for a sampling rate of 500, interrupt at 2 msec
    timer_enable_intr(group, timer);

    example_timer_info_t *timer_info = calloc(1, sizeof(example_timer_info_t));
    timer_info->timer_group = group;
    timer_info->timer_idx = timer;
    timer_info->auto_reload = auto_reload;
    timer_info->alarm_interval = timer_interval_sec;
    timer_isr_callback_add(group, timer, timer_group_isr_callback, timer_info, 0);

    timer_start(group, timer);
}

// Wrapper to create a FreeRTOS task for aws_iot_task
void aws_task_wrapper(void *param) {
    aws_iot_task(param); // Run aws_iot_task in this task
    vTaskDelete(NULL); // Delete this task after completion
}

void initialize_test_environment() {
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_timer_queue = xQueueCreate(10, sizeof(example_timer_event_t));

    // Initialize Wi-Fi
    wifi_init_sta();

    // Initialize MQTT Context
    int returnStatus = initializeMqtt(&mqttContext, &networkContext);
    if (returnStatus != EXIT_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize MQTT");
        return;
    }

    // Initialize Mutex for MQTT operations
    if (pthread_mutex_init(&mqttMutex, NULL) != 0) {
        LogError(("Failed to initialize mutex for mqtt apis, errno=%s", strerror(errno)));
        return;
    }
}

// setUp() is called before each test case
void setUp(void) {
    ESP_LOGI(TAG, "Setting up test environment.");
    initialize_test_environment();
}

// tearDown() is called after each test case
void tearDown(void) {
    ESP_LOGI(TAG, "Tearing down test environment.");

    // Delete any tasks created during the test
    if (aws_task_handle != NULL) {
        vTaskDelete(aws_task_handle);
        aws_task_handle = NULL;
    }

    // Clean up the test environment
    cleanup_test_environment();

    // Delete the default event loop to reset the state
    esp_err_t err = esp_event_loop_delete_default();
    // if (err != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to delete default event loop: %s", esp_err_to_name(err));
    // } else {
    //     ESP_LOGI(TAG, "Default event loop deleted successfully.");
    // }
}

TEST_CASE("ensure wifi is working and able to subscribe to topic", "[wifi_test]") {
    printf("Executing wifi_test\n");

    // Initialize the test environment
    // initialize_test_environment();
    int ret = 1;

    if (mqttSessionEstablished != true) {
        ret = establishConnection();
        printf("establish connection return = %d\n", ret);

    }

    // Test that the state is correctly set to idle
    TEST_ASSERT_EQUAL(ret, 0);

    // Clean up the test environment
    // cleanup_test_environment();
}

TEST_CASE("test_ecgState_idle_acquire", "[idle_acquired]") {
    // Initialize real hardware environment (uncomment if necessary)
    // initialize_test_environment();

    // Set initial state to ECG_IDLE
    ecgState = ECG_IDLE;

    // Configure ADC for the electrode
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11);  // Range up to 3.3V

    // Create the AWS task that will handle state transitions
    xTaskCreate(aws_task_wrapper, "aws_task", 8192, NULL, 5, &aws_task_handle);

    // Wait for user interaction (i.e., hands placed on ECG device)
    int max_wait_cycles = 1000;  // Adjust this value to set how long to wait for the user
    while (ecgHandsOn == 0 && max_wait_cycles > 0) {
        // Simulate ADC reading (you can replace this with actual ADC readings)
        int adc_value = adc1_get_raw(ADC1_CHANNEL_0);

        // Check if valid hand placement is detected (replace with actual threshold based on testing)
        // if (adc_value > 6 && adc_value < 4090) {
        if (adc_value == 0) {
            ecgHandsOn = 1;
            ESP_LOGI("Test", "Electrode touched! ADC value: %d", adc_value);
            // printf("touch detected, ecgHandsOn: %d, LOPlus: %d, LOMinus: %d\n", 
            //     ecgHandsOn, gpio_get_level((gpio_num_t)LOPlus), gpio_get_level((gpio_num_t)LOMinus));
        } else {
            ecgHandsOn = 0;
            ESP_LOGI("Test", "No touch detected. ADC value: %d", adc_value);
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);  // Check every 100 ms
        max_wait_cycles--;
    }

    // Once hands are detected, wait for the state to transition to ECG_ACQUIRING
    if (ecgHandsOn == 1) {
        int transition_wait_cycles = 100;
        while (ecgState != ECG_ACQUIRING && transition_wait_cycles > 0) {
            // Inject a timer event
            example_timer_event_t evt = {.timer_counter_value = 1};
            xQueueSend(s_timer_queue, &evt, portMAX_DELAY);

            vTaskDelay(10 / portTICK_PERIOD_MS);  // 10 ms delay to allow task processing
            transition_wait_cycles--;
        }

        // Assert that the state transitioned to ECG_ACQUIRING
        TEST_ASSERT_EQUAL(ECG_ACQUIRING, ecgState);
        TEST_ASSERT_EQUAL(1, ecgHandsOn);  // Ensure hands are detected
    } else {
        // If no interaction happened within the max wait time
        TEST_FAIL_MESSAGE("No user interaction detected, hands were not placed on the device.");
    }

    // Clean up the task and environment
    vTaskDelete(aws_task_handle);
    aws_task_handle = NULL;
}


TEST_CASE("test_ecgState_acquiring_to_recording", "[acquiring_to_recording]") {
    // Simulate starting in the ECG_ACQUIRING state
    ecgState = ECG_ACQUIRING;
    ecgAcqCounter = 0;
    ecgRecCounter = 0;

    // Simulate the scenario where ecgHandsOn is true and variance is high initially
    ecgHandsOn = 1;
    // int variance = 150; // High variance initially

    // Create the AWS task
    xTaskCreate(aws_task_wrapper, "aws_task", 8192, NULL, 5, &aws_task_handle);

    // Run a loop to simulate state changes
    int max_wait_cycles = 100;
    while (ecgState != ECG_RECORDING && max_wait_cycles > 0) {
        // Simulate acquiring state
        example_timer_event_t evt = {.timer_counter_value = 1};
        xQueueSend(s_timer_queue, &evt, portMAX_DELAY);

        // Increment the acquisition counter
        // ecgAcqCounter++;

        // Check if the acquisition counter reaches ECG_ACQCOUNT
        // if (ecgAcqCounter > ECG_ACQCOUNT || variance < 100) {
        //     printf("qweqeqe\n");
        //     ecgState = ECG_RECORDING;
        // }

        vTaskDelay(10 / portTICK_PERIOD_MS);
        max_wait_cycles--;
    }

    // Assert that the state transitioned to ECG_RECORDING
    TEST_ASSERT_EQUAL(ECG_RECORDING, ecgState);

    // Cleanup task and environment
    vTaskDelete(aws_task_handle);
    aws_task_handle = NULL;
}

TEST_CASE("test_ecgState_recording_to_sending_mqtt", "[recording_to_sending_mqtt]") {
    // Initialize test environment
    // initialize_test_environment();
    rec_mqtt_switch = true;
    // Simulate starting in the ECG_RECORDING state
    ecgState = ECG_RECORDING;
    ecgRecCounter = 500;
    // ecgMqttCounter = 0;
    ecgHandsOn = 1; // Simulate hands on the ECG device

    // Allocate the buffer for data (assuming the headBuffer is already set up)
    headBuffer = (unsigned short *)calloc(1000, sizeof(unsigned short)); // Example allocation
    dataBuffer = headBuffer; // Point dataBuffer to headBuffer

    // Create the AWS task
    xTaskCreate(aws_task_wrapper, "aws_task", 8192, NULL, 5, &aws_task_handle);

    // Simulate a loop to record data until conditions for transition are met
    int max_cycles = 1001; // Maximum cycles to avoid infinite loops
    while (ecgState == ECG_RECORDING && max_cycles > 0) {
        // Simulate appending data and incrementing the counter
        *dataBuffer++ = 1000; // Simulated data
        // ecgRecCounter++;

        // Log the current state and remaining max_cycles for debugging
        ESP_LOGI(TAG, "ecgRecCounter: %d, ecgHandsOn: %d, ecgState: %d, max_cycles: %d", ecgRecCounter, ecgHandsOn, ecgState, max_cycles);

        // Simulate timer event to trigger state logic
        example_timer_event_t evt = {.timer_counter_value = 1};
        xQueueSend(s_timer_queue, &evt, portMAX_DELAY);

        // Delay to simulate time passing
        vTaskDelay(10 / portTICK_PERIOD_MS);

        // Condition to check if state should transition
        // if (ecgRecCounter >= ECG_RECCOUNT || ecgHandsOn == 0) {
            // vTaskDelay(100 / portTICK_PERIOD_MS);  // Delay to allow the task to process
        // }

        max_cycles--;
    }

    // Assert that the state transitioned to ECG_SENDING_MQTT
    TEST_ASSERT_EQUAL(ECG_SENDING_MQTT, ecgState);
    TEST_ASSERT_EQUAL(0, ecgMqttCounter); // Ensure MQTT counter is reset

    // Cleanup task and environment
    vTaskDelete(aws_task_handle);
    aws_task_handle = NULL;
    // free(headBuffer); // Free allocated buffer
    // cleanup_test_environment();
}

TEST_CASE("test_led_state_for_ecg_states", "[ECG_IDLE_led]") {
    deep_sleep_test_switch = true;

    // rec_mqtt_switch = true;
    // Reset GPIOs for LEDs
    gpio_reset_pin(LED_RED);
    gpio_reset_pin(LED_GREEN);
    gpio_reset_pin(LED_BLUE);

    // Configure GPIOs for LEDs as output
    gpio_set_direction(LED_RED, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(LED_GREEN, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(LED_BLUE, GPIO_MODE_INPUT_OUTPUT);

    // Set initial state for LEDs (turn all off)
    gpio_set_level(LED_RED, 1);    // Turn off RED
    gpio_set_level(LED_GREEN, 1);  // Turn off GREEN
    gpio_set_level(LED_BLUE, 1);   // Turn off BLUE

    // Simulate the ECG_IDLE state and verify LED behavior
    ecgState = ECG_IDLE;
    xTaskCreate(aws_task_wrapper, "aws_task", 8192, NULL, 5, &aws_task_handle);

    int max_wait_cycles = 100;
    while (ecgState == ECG_IDLE && max_wait_cycles > 0) {
        // Inject a timer event
        example_timer_event_t evt = {.timer_counter_value = 1};
        xQueueSend(s_timer_queue, &evt, portMAX_DELAY);

        vTaskDelay(10 / portTICK_PERIOD_MS);
        max_wait_cycles--;
    }
    // Add a short delay to allow the task to run and update LED states

    // Print GPIO levels before assertions
    printf("LED_RED level: %d\n", gpio_get_level(LED_RED));
    printf("LED_GREEN level: %d\n", gpio_get_level(LED_GREEN));
    printf("LED_BLUE level: %d\n", gpio_get_level(LED_BLUE));

    // Assertions to verify the expected LED states
    TEST_ASSERT_EQUAL(1, gpio_get_level(LED_RED));    // RED should be on
    TEST_ASSERT_EQUAL(0, gpio_get_level(LED_GREEN));  // GREEN should be off
    TEST_ASSERT_EQUAL(1, gpio_get_level(LED_BLUE));   // BLUE should be off

    // Cleanup task and environment
    vTaskDelete(aws_task_handle);
    aws_task_handle = NULL;
}

TEST_CASE("test_led_state_for_ecg_states", "[ECG_ACQUIRING_led]") {
    // deep_sleep_test_switch = true;

    // rec_mqtt_switch = true;
    // Reset GPIOs for LEDs
    gpio_reset_pin(LED_RED);
    gpio_reset_pin(LED_GREEN);
    gpio_reset_pin(LED_BLUE);

    // Configure GPIOs for LEDs as output
    gpio_set_direction(LED_RED, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(LED_GREEN, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(LED_BLUE, GPIO_MODE_INPUT_OUTPUT);

    // Set initial state for LEDs (turn all off)
    gpio_set_level(LED_RED, 1);    // Turn off RED
    gpio_set_level(LED_GREEN, 1);  // Turn off GREEN
    gpio_set_level(LED_BLUE, 1);   // Turn off BLUE

    // Simulate the ECG_IDLE state and verify LED behavior
    ecgState = ECG_IDLE;
    xTaskCreate(aws_task_wrapper, "aws_task", 8192, NULL, 5, &aws_task_handle);

    int max_wait_cycles = 100;
    while (ecgState != ECG_ACQUIRING && max_wait_cycles > 0) {
        // Inject a timer event
        example_timer_event_t evt = {.timer_counter_value = 1};
        xQueueSend(s_timer_queue, &evt, portMAX_DELAY);

        vTaskDelay(10 / portTICK_PERIOD_MS);
        max_wait_cycles--;
    }
    // Add a short delay to allow the task to run and update LED states

    // Print GPIO levels before assertions
    printf("LED_RED level: %d\n", gpio_get_level(LED_RED));
    printf("LED_GREEN level: %d\n", gpio_get_level(LED_GREEN));
    printf("LED_BLUE level: %d\n", gpio_get_level(LED_BLUE));

        printf("Current ecgState: %d\n", ecgState);

    // Assertions to verify the expected LED states
    TEST_ASSERT_EQUAL(ECG_ACQUIRING, ecgState);
    // TEST_ASSERT_EQUAL(1, gpio_get_level(LED_RED));    // RED should be on
    // TEST_ASSERT_EQUAL(1, gpio_get_level(LED_GREEN));  // GREEN should be off
    // TEST_ASSERT_EQUAL(0, gpio_get_level(LED_BLUE));   // BLUE should be off

    // Cleanup task and environment
    vTaskDelete(aws_task_handle);
    aws_task_handle = NULL;
}

TEST_CASE("test_led_state_for_ecg_states", "[ECG_RECORDING_led]") {
    deep_sleep_test_switch = true;

    // rec_mqtt_switch = true;
    // Reset GPIOs for LEDs
    gpio_reset_pin(LED_RED);
    gpio_reset_pin(LED_GREEN);
    gpio_reset_pin(LED_BLUE);

    // Configure GPIOs for LEDs as output
    gpio_set_direction(LED_RED, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(LED_GREEN, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(LED_BLUE, GPIO_MODE_INPUT_OUTPUT);

    // Set initial state for LEDs (turn all off)
    gpio_set_level(LED_RED, 1);    // Turn off RED
    gpio_set_level(LED_GREEN, 1);  // Turn off GREEN
    gpio_set_level(LED_BLUE, 1);   // Turn off BLUE

    // Simulate the ECG_IDLE state and verify LED behavior
    // Simulate starting in the ECG_ACQUIRING state
    ecgState = ECG_ACQUIRING;
    ecgAcqCounter = 0;
    ecgRecCounter = 0;

    // Simulate the scenario where ecgHandsOn is true and variance is high initially
    ecgHandsOn = 1;
    xTaskCreate(aws_task_wrapper, "aws_task", 8192, NULL, 5, &aws_task_handle);

    int max_wait_cycles = 100;
    while (ecgState != ECG_RECORDING && max_wait_cycles > 0) {
        // Inject a timer event
        example_timer_event_t evt = {.timer_counter_value = 1};
        xQueueSend(s_timer_queue, &evt, portMAX_DELAY);

        vTaskDelay(10 / portTICK_PERIOD_MS);
        max_wait_cycles--;
    }
    // Add a short delay to allow the task to run and update LED states

    // Print GPIO levels before assertions
    printf("LED_RED level: %d\n", gpio_get_level(LED_RED));
    printf("LED_GREEN level: %d\n", gpio_get_level(LED_GREEN));
    printf("LED_BLUE level: %d\n", gpio_get_level(LED_BLUE));

    // Assertions to verify the expected LED states
    TEST_ASSERT_EQUAL(1, gpio_get_level(LED_RED));    // RED should be on
    TEST_ASSERT_EQUAL(1, gpio_get_level(LED_GREEN));  // GREEN should be off
    TEST_ASSERT_EQUAL(0, gpio_get_level(LED_BLUE));   // BLUE should be off

    // Cleanup task and environment
    vTaskDelete(aws_task_handle);
    aws_task_handle = NULL;
}

// Yet to complete
TEST_CASE("test_ecgState_error_mqtt_on_connection_failure", "[mqtt_connection_failure]") {
    // Initialize test environment
    // initialize_test_environment("valid_ssid", "valid_password"); // Use valid Wi-Fi credentials

    // Set up the initial state
    ecgState = ECG_SENDING_MQTT;
    ecgRecCounter = 500; // Simulate recorded data
    mqttSessionEstablished = false; // Simulate MQTT not established

    // Mock the establishConnection function to always fail
    int establishConnection(void) {
        return -1; // Simulate failure
    }

    // Start the aws_iot_task
    xTaskCreate(aws_task_wrapper, "aws_task", 8192, NULL, 5, &aws_task_handle);

    // Wait for the task to process
    int max_wait_cycles = 10;
    while (ecgState != ECG_ERROR_MQTT && max_wait_cycles > 0) {
        // Inject a timer event
        example_timer_event_t evt = {.timer_counter_value = 1};
        xQueueSend(s_timer_queue, &evt, portMAX_DELAY);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        max_wait_cycles--;
    }
    
    // Print the current value of ecgState before the assertion
    printf("ecgState before assertion: %d\n", ecgState);

    // Assert that the device went into ECG_ERROR_MQTT state
    TEST_ASSERT_EQUAL(ECG_ERROR_MQTT, ecgState);

    // Cleanup task and environment
    vTaskDelete(aws_task_handle);
    aws_task_handle = NULL;
}

// Mock esp_deep_sleep_start function globally to simulate deep sleep
__attribute__((weak)) void esp_deep_sleep_start() {
    deep_sleep_called = true;  // Simulate that deep sleep was called
    printf("Mock deep sleep called\n");
}

TEST_CASE("test_device_enters_deep_sleep_from_idle_state", "[deep_sleep]") {
    #ifdef TEST_ENV
    test_counter = 0;  // Reset test counter at the start of the test
    printf("Test counter value: %d\n", test_counter);  // Print the initial value of test_counter
    #endif
    
    // Set the initial state to ECG_IDLE
    ecgState = ECG_IDLE;
    ecgHandsOn = 0;  // Simulate no hands on the device
    deep_sleep_test_switch = true;
    // Print initial values before creating the task
    printf("Before task creation:\n");
    printf("ecgState: %d, ecgHandsOn: %d\n", ecgState, ecgHandsOn);

    // Create the AWS task to test
    xTaskCreate(aws_task_wrapper, "aws_task", 8192, NULL, 5, &aws_task_handle);

    // Run a loop to simulate the task running and decrementing the deepSleepCounter
    int max_wait_cycles = 100;  // Number of iterations before timeout
    while (deepSleepCounter > 0 && max_wait_cycles > 0) {
        // Print LOPlus and LOMinus values
        printf("LOPlus value: %d\n", gpio_get_level(LOPlus));
        printf("LOMinus value: %d\n", gpio_get_level(LOMinus));

        // Inject a timer event to simulate the passage of time
        printf("ecgState: %d, ecgHandsOn: %d, deepSleepCounter: %lu\n", ecgState, ecgHandsOn, deepSleepCounter);

        example_timer_event_t evt = {.timer_counter_value = 1};
        xQueueSend(s_timer_queue, &evt, portMAX_DELAY);  // Send event to task queue

        // Simulate some delay to allow the task to process the event
        vTaskDelay(10 / portTICK_PERIOD_MS);  // 10 ms delay
        max_wait_cycles--;

        // Print the current deepSleepCounter value after each loop iteration
        printf("deepSleepCounter after decrement: %lu\n", deepSleepCounter);
    }

    // Assert that the deepSleepCounter reached 0 and that deep sleep was triggered
    TEST_ASSERT_EQUAL(0, deepSleepCounter);

    // #ifdef TEST_ENV
    // // Verify that deep sleep was mocked and test_counter incremented
    // TEST_ASSERT_TRUE(deep_sleep_called);  // Check if the mocked deep sleep function was called
    // printf("Test counter after deep sleep attempt: %d\n", test_counter);  // Print the final value of test_counter
    // #endif

    // Cleanup task and environment
    vTaskDelete(aws_task_handle);
    aws_task_handle = NULL;
}






// TEST_CASE("test_ecgState_sending_mqtt_to_finish", "[sending_mqtt_to_finish]") {
//     // Initialize test environment
//     // initialize_test_environment();

//     // Simulate starting in the ECG_SENDING_MQTT state
//     ecgState = ECG_SENDING_MQTT;
//     ecgRecCounter = 50;  // Simulate some recorded data
//     ecgMqttCounter = 0;

//     // Simulate MQTT session not being established initially
//     mqttSessionEstablished = false;

//     // Mocked values for Wi-Fi MAC and topic name generation
//     uint8_t brd_mac[6] = {0x64, 0xB7, 0x08, 0x6E, 0xE0, 0xCC}; // Mock MAC address
//     esp_wifi_get_mac = [&](wifi_interface_t ifx, uint8_t mac[6]) {
//         memcpy(mac, brd_mac, sizeof(brd_mac));
//         return ESP_OK;
//     };

//     // Mock for MQTT publish without mutex
//     mqttPublishNoMutex = [&](const char *topic, int topic_len, const void *payload, unsigned short payload_len, int qos) {
//         // Simulate successful MQTT publish
//         return 0;
//     };

//     // Simulate the task and proceed through the state transition
//     int max_wait_cycles = 100;
//     while (ecgState != ECG_FINISH && max_wait_cycles > 0) {
//         // Simulate sending ECG data via MQTT
//         ecgMqttCounter++;
//         ecgState = ECG_FINISH;
        
//         vTaskDelay(10 / portTICK_PERIOD_MS);
//         max_wait_cycles--;
//     }

//     // Assert that the state transitioned to ECG_FINISH
//     TEST_ASSERT_EQUAL(ECG_FINISH, ecgState);
//     TEST_ASSERT_EQUAL(0, ecgRecCounter);  // Ensure the record counter is reset
//     TEST_ASSERT_EQUAL(1, ecgMqttCounter);  // Ensure the MQTT counter incremented

//     // Cleanup task and environment
//     vTaskDelete(aws_task_handle);
//     aws_task_handle = NULL;
//     // cleanup_test_environment();
// 

TEST_CASE("aws_iot_task", "[aws_iot]") {
    printf("Executing aws_iot_task_initialization\n");

    // Initialize the test environment
    initialize_test_environment();

    // Proceed with provisioning or demo logic based on NVS provision status
    // if (nvsProvisionStatus == false) {
        // Attempt to establish MQTT connection
        if (mqttSessionEstablished != true) {
            int ret = establishConnection();
            printf("Establish connection return = %d\n", ret);
        }
    // // } else {
    //     // Provisioned: Start the demo
        typedef struct Data_t {
            uint32_t uData;
            char **id;
        } arg_params_t;

        arg_params_t data1 = {0, NULL};
        xTaskCreatePinnedToCore((TaskFunction_t)&aws_iot_demo_main, "aws_iot_demo_main", 9216, (void *)&data1, 5, NULL, 1);
        aws_iot_task(&aws_iot_task);
    // // }

    // Test that the state is correctly set to idle
    TEST_ASSERT_EQUAL(ECG_IDLE, ecgState);
}


