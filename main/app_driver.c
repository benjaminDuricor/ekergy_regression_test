/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <iot_button.h>
#include <nvs_flash.h>
#include "esp_system.h"

#include "app_priv.h"
#include "board_esp32_devkitc.h"

static bool g_output_state;
static void push_btn_cb(void *arg)
{
    app_driver_set_state(!g_output_state);
}

static void button_press_3sec_cb(void *arg)
{
    nvs_flash_erase();
    esp_restart();          //restart after the nvs flash has been erased
}

static void configure_push_button(int gpio_num, void (*btn_cb)(void *))
{
    button_handle_t btn_handle = iot_button_create(JUMPSTART_BOARD_BUTTON_GPIO, JUMPSTART_BOARD_BUTTON_ACTIVE_LEVEL);  // create an instance with the gpio # and the active level (?)
    if (btn_handle) {
        iot_button_set_evt_cb(btn_handle, BUTTON_CB_RELEASE, btn_cb, "RELEASE");    // set up the event for call back but not the call back function , in this case it's a release (from 0 to 1) of button, does it do debounce(?)
        iot_button_add_on_press_cb(btn_handle, 3, button_press_3sec_cb, NULL);      // add to the timer queue an event with call back after 3 seconds 
    }
}

static void set_output_state(bool target)
{
    gpio_set_level(JUMPSTART_BOARD_OUTPUT_GPIO, target);
}

void app_driver_init()
{
    configure_push_button(JUMPSTART_BOARD_BUTTON_GPIO, push_btn_cb);    //cnofigure the push button with the gpio number (?) and a call back function

    /* Configure output */
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 1,
    };
    io_conf.pin_bit_mask = ((uint64_t)1 << JUMPSTART_BOARD_OUTPUT_GPIO);
    /* Configure the GPIO */
    gpio_config(&io_conf);
}

int IRAM_ATTR app_driver_set_state(bool state)
{
    if(g_output_state != state) {
        g_output_state = state;
        set_output_state(g_output_state);
    }
    return ESP_OK;
}

bool app_driver_get_state(void)
{
    return g_output_state;
}
