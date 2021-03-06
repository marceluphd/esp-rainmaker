// Copyright 2020 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <sdkconfig.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>

#include <esp_rmaker_core.h>
#include <esp_rmaker_utils.h>
#include "esp_rmaker_internal.h"
#include "esp_rmaker_storage.h"
#include "esp_rmaker_mqtt.h"
#include "esp_rmaker_claim.h"
#include "esp_rmaker_client_data.h"

static const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t wifi_event_group;


ESP_EVENT_DEFINE_BASE(RMAKER_EVENT);

static const char *TAG = "esp_rmaker_core";

#define ESP_RMAKER_TASK_QUEUE_SIZE           8

#define ESP_RMAKER_TASK_STACK       CONFIG_ESP_RMAKER_TASK_STACK
#define ESP_RMAKER_TASK_PRIORITY    CONFIG_ESP_RMAKER_TASK_PRIORITY

#define ESP_RMAKER_CHECK_HANDLE(rval) \
{ \
    if (!esp_rmaker_priv_data) {\
        ESP_LOGE(TAG, "ESP RainMaker not initialised"); \
        return rval; \
    } \
}

#define ESP_CLAIM_NODE_ID_SIZE  12

typedef enum {
    ESP_RMAKER_STATE_DEINIT = 0,
    ESP_RMAKER_STATE_INIT_DONE,
    ESP_RMAKER_STATE_STARTING,
    ESP_RMAKER_STATE_STARTED,
    ESP_RMAKER_STATE_STOP_REQUESTED,
} esp_rmaker_state_t;

/* Handle to maintain internal information (will move to an internal file) */
typedef struct {
    char *node_id;
    const esp_rmaker_node_t *node;
    bool enable_time_sync;
    esp_rmaker_state_t state;
    bool mqtt_connected;
    esp_rmaker_mqtt_config_t *mqtt_config;
#ifdef CONFIG_ESP_RMAKER_SELF_CLAIM
    bool self_claim;
#endif /* CONFIG_ESP_RMAKER_SELF_CLAIM */
    QueueHandle_t work_queue;
} esp_rmaker_priv_data_t;

static esp_rmaker_priv_data_t *esp_rmaker_priv_data;

static char *esp_rmaker_populate_node_id()
{
    char *node_id = esp_rmaker_storage_get("node_id");
#ifdef CONFIG_ESP_RMAKER_SELF_CLAIM
    if (!node_id) {
        uint8_t eth_mac[6];
        esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not fetch MAC address. Please initialise Wi-Fi first");
            return NULL;
        }
        node_id = calloc(1, ESP_CLAIM_NODE_ID_SIZE + 1); /* +1 for NULL terminatation */
        snprintf(node_id, ESP_CLAIM_NODE_ID_SIZE + 1, "%02X%02X%02X%02X%02X%02X",
                eth_mac[0], eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);
    }
#endif /* CONFIG_ESP_RMAKER_SELF_CLAIM */
    return node_id;
}


/* Event handler for catching system events */
static void esp_rmaker_event_handler(void* arg, esp_event_base_t event_base,
                          int event_id, void* event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        /* Signal rmaker thread to continue execution */
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    }
}

static esp_err_t esp_rmaker_deinit_priv_data(esp_rmaker_priv_data_t *rmaker_priv_data)
{
    if (!rmaker_priv_data) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rmaker_priv_data->work_queue) {
        vQueueDelete(rmaker_priv_data->work_queue);
    }
    if (rmaker_priv_data->mqtt_config) {
        esp_rmaker_clean_mqtt_config(rmaker_priv_data->mqtt_config);
        free(rmaker_priv_data->mqtt_config);
    }
    if (rmaker_priv_data->node_id) {
        free(rmaker_priv_data->node_id);
    }
    free(rmaker_priv_data);
    return ESP_OK;
}

esp_err_t esp_rmaker_node_deinit(const esp_rmaker_node_t *node)
{
    if (!esp_rmaker_priv_data) {
        ESP_LOGE(TAG, "ESP RainMaker already de-initialized.");
        return ESP_ERR_INVALID_ARG;
    }

    if (esp_rmaker_priv_data->state != ESP_RMAKER_STATE_INIT_DONE) {
        ESP_LOGE(TAG, "ESP RainMaker is still running. Please stop it first.");
        return ESP_ERR_INVALID_STATE;
    }
    esp_rmaker_node_delete(node);
    esp_rmaker_priv_data->node = NULL;
    esp_rmaker_deinit_priv_data(esp_rmaker_priv_data);
    esp_rmaker_priv_data = NULL;
    return ESP_OK;
}

char *esp_rmaker_get_node_id(void)
{
    if (esp_rmaker_priv_data) {
        return esp_rmaker_priv_data->node_id;
    }
    return NULL;
}
/* Initialize ESP RainMaker */
static esp_err_t esp_rmaker_init(const esp_rmaker_config_t *config)
{
    if (esp_rmaker_priv_data) {
        ESP_LOGE(TAG, "ESP RainMaker already initialised");
        return ESP_ERR_INVALID_STATE;
    }
    if (!config) {
        ESP_LOGE(TAG, "RainMaker config missing. Cannot initialise");
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_rmaker_storage_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialise storage");
        return ESP_FAIL;
    }
    esp_rmaker_priv_data = calloc(1, sizeof(esp_rmaker_priv_data_t));
    if (!esp_rmaker_priv_data) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return ESP_ERR_NO_MEM;
    }

    esp_rmaker_priv_data->node_id = esp_rmaker_populate_node_id();
    if (!esp_rmaker_priv_data->node_id) {
        esp_rmaker_deinit_priv_data(esp_rmaker_priv_data);
        esp_rmaker_priv_data = NULL;
        ESP_LOGE(TAG, "Failed to initialise Node Id. Please perform \"claiming\" using RainMaker CLI.");
        return ESP_ERR_NO_MEM;
    }

    esp_rmaker_priv_data->work_queue = xQueueCreate(ESP_RMAKER_TASK_QUEUE_SIZE, sizeof(esp_rmaker_work_queue_entry_t));
    if (!esp_rmaker_priv_data->work_queue) {
        esp_rmaker_deinit_priv_data(esp_rmaker_priv_data);
        esp_rmaker_priv_data = NULL;
        ESP_LOGE(TAG, "ESP RainMaker Queue Creation Failed");
        return ESP_ERR_NO_MEM;
    }
    esp_rmaker_priv_data->mqtt_config = esp_rmaker_get_mqtt_config();
    if (!esp_rmaker_priv_data->mqtt_config) {
#ifdef CONFIG_ESP_RMAKER_SELF_CLAIM
        esp_rmaker_priv_data->self_claim = true;
        if (esp_rmaker_self_claim_init() != ESP_OK) {
            esp_rmaker_deinit_priv_data(esp_rmaker_priv_data);
            esp_rmaker_priv_data = NULL;
            ESP_LOGE(TAG, "Failed to initialise Self Claiming.");
            return ESP_FAIL;
        }
#else
        esp_rmaker_deinit_priv_data(esp_rmaker_priv_data);
        esp_rmaker_priv_data = NULL;
        ESP_LOGE(TAG, "Failed to initialise MQTT Config. Please perform \"claiming\" using RainMaker CLI.");
        return ESP_FAIL;
#endif /* !CONFIG_ESP_RMAKER_SELF_CLAIM */
    } else {
        if (esp_rmaker_mqtt_init(esp_rmaker_priv_data->mqtt_config) != ESP_OK) {
            esp_rmaker_deinit_priv_data(esp_rmaker_priv_data);
            esp_rmaker_priv_data = NULL;
            ESP_LOGE(TAG, "Failed to initialise MQTT");
            return ESP_FAIL;
        }
    }
    esp_rmaker_priv_data->enable_time_sync = config->enable_time_sync;
    esp_rmaker_post_event(RMAKER_EVENT_INIT_DONE, NULL, 0);
    esp_rmaker_priv_data->state = ESP_RMAKER_STATE_INIT_DONE;
    return ESP_OK;
}

static esp_err_t esp_rmaker_register_node(const esp_rmaker_node_t *node)
{
    ESP_RMAKER_CHECK_HANDLE(ESP_ERR_INVALID_STATE);
    if (esp_rmaker_priv_data->node) {
        ESP_LOGE(TAG, "A node has already been registered. Cannot register another.");
        return ESP_ERR_INVALID_STATE;
    }
    if (!node) {
        ESP_LOGE(TAG, "Node handle cannot be NULL.");
        return ESP_ERR_INVALID_ARG;
    }
    esp_rmaker_priv_data->node = node;
    return ESP_OK;
}

esp_rmaker_node_t *esp_rmaker_node_init(const esp_rmaker_config_t *config, const char *name, const char *type)
{
    esp_err_t err = esp_rmaker_init(config);
    if (err != ESP_OK) {
        return NULL;
    }
    esp_rmaker_node_t *node = esp_rmaker_node_create(name, type);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create node");
        return NULL;
    }
    err = esp_rmaker_register_node(node);
    if (err != ESP_OK) {
        free(node);
        return NULL;
    }
    return node;
}

const esp_rmaker_node_t *esp_rmaker_get_node()
{
    ESP_RMAKER_CHECK_HANDLE(NULL);
    return esp_rmaker_priv_data->node;
}

static esp_err_t esp_rmaker_report_node_config_and_state()
{
    if (esp_rmaker_report_node_config() != ESP_OK) {
        ESP_LOGE(TAG, "Report node config failed.");
        return ESP_FAIL;
    }
    if (esp_rmaker_report_node_state() != ESP_OK) {
        ESP_LOGE(TAG, "Report node state failed.");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void __esp_rmaker_report_node_config_and_state(void *data)
{
    esp_rmaker_report_node_config_and_state();
}

esp_err_t esp_rmaker_report_node_details()
{
    return esp_rmaker_queue_work(__esp_rmaker_report_node_config_and_state, NULL);
}

static void esp_rmaker_handle_work_queue()
{
    ESP_RMAKER_CHECK_HANDLE();
    esp_rmaker_work_queue_entry_t work_queue_entry;
    BaseType_t ret = xQueueReceive(esp_rmaker_priv_data->work_queue, &work_queue_entry, 0);
    while (ret == pdTRUE) {
        work_queue_entry.work_fn(work_queue_entry.priv_data);
        ret = xQueueReceive(esp_rmaker_priv_data->work_queue, &work_queue_entry, 0);
    }
}

static void esp_rmaker_task(void *param)
{
    ESP_RMAKER_CHECK_HANDLE();
    esp_rmaker_priv_data->state = ESP_RMAKER_STATE_STARTING;
    esp_err_t err;
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &esp_rmaker_event_handler, NULL)); 
    /* Wait for Wi-Fi connection */
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, false, true, portMAX_DELAY);

    if (esp_rmaker_priv_data->enable_time_sync) {
#ifdef CONFIG_MBEDTLS_HAVE_TIME_DATE
        esp_rmaker_time_wait_for_sync(portMAX_DELAY);
#endif
    }
#ifdef CONFIG_ESP_RMAKER_SELF_CLAIM
    if (esp_rmaker_priv_data->self_claim) {
        esp_rmaker_post_event(RMAKER_EVENT_CLAIM_STARTED, NULL, 0);
        err = esp_rmaker_self_claim_perform();
        if (err != ESP_OK) {
            esp_rmaker_post_event(RMAKER_EVENT_CLAIM_FAILED, NULL, 0);
            ESP_LOGE(TAG, "esp_rmaker_self_claim_perform() returned %d. Aborting", err);
            vTaskDelete(NULL);
        }
        esp_rmaker_post_event(RMAKER_EVENT_CLAIM_SUCCESSFUL, NULL, 0);
        esp_rmaker_priv_data->mqtt_config = esp_rmaker_get_mqtt_config();
        if (!esp_rmaker_priv_data->mqtt_config) {
            ESP_LOGE(TAG, "Failed to initialise MQTT Config after claiming. Aborting");
            vTaskDelete(NULL);
        }
        err = esp_rmaker_mqtt_init(esp_rmaker_priv_data->mqtt_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_rmaker_mqtt_init() returned %d. Aborting", err);
            vTaskDelete(NULL);
        }
    }
#endif /* CONFIG_ESP_RMAKER_SELF_CLAIM */
    err = esp_rmaker_mqtt_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_rmaker_mqtt_connect() returned %d. Aborting", err);
        vTaskDelete(NULL);
    }
    esp_rmaker_priv_data->mqtt_connected = true;
    esp_rmaker_priv_data->state = ESP_RMAKER_STATE_STARTED;
    if (esp_rmaker_report_node_config_and_state() != ESP_OK) {
        ESP_LOGE(TAG, "Aborting!!!");
        goto rmaker_end;
    }
    if (esp_rmaker_register_for_set_params() != ESP_OK) {
        ESP_LOGE(TAG, "Aborting!!!");
        goto rmaker_end;
    }
    while (esp_rmaker_priv_data->state != ESP_RMAKER_STATE_STOP_REQUESTED) {
        esp_rmaker_handle_work_queue(esp_rmaker_priv_data);
        /* 2 sec delay to prevent spinning */
        vTaskDelay(2000 / portTICK_RATE_MS);
    }
rmaker_end:
    esp_rmaker_mqtt_disconnect();
    esp_rmaker_priv_data->mqtt_connected = false;
    esp_rmaker_priv_data->state = ESP_RMAKER_STATE_INIT_DONE;
    vTaskDelete(NULL);
}

esp_err_t esp_rmaker_queue_work(esp_rmaker_work_fn_t work_fn, void *priv_data)
{
    ESP_RMAKER_CHECK_HANDLE(ESP_ERR_INVALID_STATE);
    esp_rmaker_work_queue_entry_t work_queue_entry = {
        .work_fn = work_fn,
        .priv_data = priv_data,
    };
    if (xQueueSend(esp_rmaker_priv_data->work_queue, &work_queue_entry, 0) == pdTRUE) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

/* Start the ESP RainMaker Core Task */
esp_err_t esp_rmaker_start()
{
    ESP_RMAKER_CHECK_HANDLE(ESP_ERR_INVALID_STATE);
    if (esp_rmaker_priv_data->enable_time_sync) {
        esp_rmaker_time_sync_init(NULL);
    }
    ESP_LOGI(TAG, "Starting RainMaker Core Task");
    if (xTaskCreate(&esp_rmaker_task, "esp_rmaker_task", ESP_RMAKER_TASK_STACK,
                NULL, ESP_RMAKER_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Couldn't create RainMaker core task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_rmaker_stop()
{
    ESP_RMAKER_CHECK_HANDLE(ESP_ERR_INVALID_STATE);
    esp_rmaker_priv_data->state = ESP_RMAKER_STATE_STOP_REQUESTED;
    return ESP_OK;
}

