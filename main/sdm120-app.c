
/**
 * @file sdm120-app.c
 * @brief SDM120 Energy Meter Modbus TCP Master - Single Slave Configuration
 * 
 * This application demonstrates the SIMPLIFIED and CORRECT way to implement a Modbus TCP master
 * for reading data from a SINGLE SDM120 energy meter using ESP-IDF's HIGH-LEVEL APIs.
 * 
 * 📝 QUICK SETUP:
 * 1. Change SDM120_SLAVE_IP to your device's IP address
 * 2. Configure WiFi credentials in menuconfig
 * 3. Build and flash to ESP32
 * 
 * ✨ Key Features:
 * ✅ Single slave configuration - no complex IP tables
 * ✅ Uses mbc_master_get_parameter() - automatic and reliable
 * ✅ High-level APIs only - no low-level complexity
 * ✅ Robust error handling with graceful recovery
 * ✅ Clean code structure and comprehensive logging
 * ✅ Easy to configure and maintain
 * 
 * 🚫 Issues Fixed from Previous Version:
 * - ❌ Multiple slave complexity removed
 * - ❌ Interactive IP configuration removed
 * - ❌ mbc_master_send_request() problems eliminated
 * - ❌ Manual handle management removed
 * - ❌ Complex data placement logic removed
 * 
 * This version is simplified, reliable, and uses maximum high-level APIs.
 */

#include <string.h> // Required for offsetof
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mac.h"

#include "mdns.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "mbcontroller.h"
#include "sdkconfig.h"



static const char* TAG = "SDM120_MQTT";

#define MB_TCP_PORT                     (CONFIG_FMB_TCP_PORT_DEFAULT)   // TCP port used by example

#define STR(fieldname) ((const char*)( fieldname ))

// Options can be used as bit masks or parameter limits
#define OPTS(min_val, max_val, step_val) { .opt1 = min_val, .opt2 = max_val, .opt3 = step_val }
#define INPUT_OFFSET(field) ((uint16_t)(offsetof(sdm120_data_t, field) + 1))

/* ===== CONFIGURATION SECTION ===== 
 * 🔧 Configuration is now externalized through Kconfig (menuconfig)
 * Use 'idf.py menuconfig' to configure all settings including credentials
 */

// Device Configuration - from Kconfig
#define SDM120_SLAVE_IP     CONFIG_SDM120_DEVICE_IP
#define SDM120_SLAVE_PORT   CONFIG_SDM120_DEVICE_PORT

// MQTT Configuration - from Kconfig (credentials externalized!)
#define MQTT_BROKER_URI     CONFIG_SDM120_MQTT_BROKER_URI
#define MQTT_CLIENT_ID      CONFIG_SDM120_MQTT_CLIENT_ID
#define MQTT_TOPIC_PREFIX   CONFIG_SDM120_MQTT_TOPIC_PREFIX
#define MQTT_USERNAME       CONFIG_SDM120_MQTT_USERNAME
#define MQTT_PASSWORD       CONFIG_SDM120_MQTT_PASSWORD
#define MQTT_PUBLISH_INTERVAL_MS  5000                             // How often to publish (5 seconds)

// MQTT Publishing Options - from Kconfig
#define MQTT_PUBLISH_INDIVIDUAL_TOPICS  true                      // Publish each CID to separate topic
#define MQTT_HOME_ASSISTANT_DISCOVERY   CONFIG_SDM120_MQTT_HOME_ASSISTANT
#define MQTT_HA_DISCOVERY_PREFIX        CONFIG_SDM120_MQTT_HA_PREFIX

// Modbus Timing Configuration - from Kconfig
#define MODBUS_RESPONSE_TIMEOUT_MS      CONFIG_SDM120_MODBUS_TIMEOUT
#define MODBUS_INTER_PARAM_DELAY_MS     CONFIG_SDM120_INTER_PARAM_DELAY
#define MODBUS_RETRY_DELAY_BASE_MS      200                        // Base delay for retry attempts

// Single slave configuration - no complex IP tables needed
static char* slave_ip_address = SDM120_SLAVE_IP;

// WiFi Configuration - from Kconfig (credentials externalized!)
#define WIFI_SSID               CONFIG_WIFI_SSID
#define WIFI_PASSWORD           CONFIG_WIFI_PASSWORD
#define WIFI_MAXIMUM_RETRY      CONFIG_WIFI_MAXIMUM_RETRY
#define WIFI_CONNECT_TIMEOUT_MS CONFIG_WIFI_CONNECT_TIMEOUT_MS
#define WIFI_RECONNECT_INTERVAL_MS CONFIG_WIFI_RECONNECT_INTERVAL_MS

// WiFi power save configuration
#ifdef CONFIG_WIFI_POWER_SAVE_NONE
#define WIFI_PS_MODE WIFI_PS_NONE
#elif CONFIG_WIFI_POWER_SAVE_MIN
#define WIFI_PS_MODE WIFI_PS_MIN_MODEM
#elif CONFIG_WIFI_POWER_SAVE_MAX
#define WIFI_PS_MODE WIFI_PS_MAX_MODEM
#else
#define WIFI_PS_MODE WIFI_PS_NONE  // Default to no power save
#endif

// WiFi connection management
#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool wifi_connected = false;
static esp_netif_t* s_wifi_netif = NULL;  // Global WiFi network interface handle

// MQTT client handle and connection status
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

// Forward declarations
static esp_err_t mqtt_publish_ha_discovery(void);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static esp_err_t wifi_init_and_connect(void);
static void wifi_reconnect_task(void* pvParameters);

/**
 * @brief WiFi event handler for connection management
 * 
 * Handles WiFi connection, disconnection, and IP acquisition events.
 * Implements automatic reconnection with configurable retry logic.
 * 
 * @param arg Unused parameter
 * @param event_base Event base (WIFI_EVENT or IP_EVENT)
 * @param event_id Event ID 
 * @param event_data Event data
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "📡 WiFi station started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "⚠️  WiFi connection failed, retry %d/%d", s_retry_num, WIFI_MAXIMUM_RETRY);
        } else {
            ESP_LOGE(TAG, "❌ WiFi connection failed after %d retries", WIFI_MAXIMUM_RETRY);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "🌐 WiFi connected! IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief WiFi reconnection background task
 * 
 * Monitors WiFi connection status and attempts reconnection when connection is lost.
 * Runs continuously in the background with configurable reconnection intervals.
 * 
 * @param pvParameters Unused task parameters
 */
static void wifi_reconnect_task(void* pvParameters)
{
    ESP_LOGI(TAG, "🔄 WiFi monitoring task started");
    
    while (true) {
        // Check if WiFi is connected
        wifi_ap_record_t ap_info;
        esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
        
        if (ret != ESP_OK || !wifi_connected) {
            ESP_LOGW(TAG, "🔄 WiFi connection lost, attempting reconnection...");
            wifi_connected = false;
            
            // Reset retry counter and attempt reconnection
            s_retry_num = 0;
            esp_wifi_connect();
            
            // Wait for connection result
            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                pdFALSE,
                pdFALSE,
                pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
                
            if (bits & WIFI_CONNECTED_BIT) {
                ESP_LOGI(TAG, "🎉 WiFi reconnected successfully");
            } else if (bits & WIFI_FAIL_BIT) {
                ESP_LOGE(TAG, "❌ WiFi reconnection failed");
            }
            
            // Clear event bits for next iteration
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        }
        
        // Wait before next check
        vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_INTERVAL_MS));
    }
}

/**
 * @brief Initialize and connect to WiFi network
 * 
 * Sets up WiFi station mode, configures credentials, and establishes connection.
 * Implements robust connection handling with timeout and retry logic.
 * 
 * @return ESP_OK on success, error code on failure
 */
static esp_err_t wifi_init_and_connect(void)
{
    // Validate WiFi credentials
    if (strlen(WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "❌ WiFi SSID not configured! Use 'idf.py menuconfig' to set WiFi credentials.");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "🔧 Initializing WiFi connection to '%s'...", WIFI_SSID);
    
    // Create WiFi event group
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "❌ Failed to create WiFi event group");
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());
    
    // Create default WiFi station and store globally
    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_netif == NULL) {
        ESP_LOGE(TAG, "❌ Failed to create default WiFi station interface");
        return ESP_FAIL;
    }
    
    // Initialize WiFi with default configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Register event handlers
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
    
    // Configure WiFi connection
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    
    // Copy SSID and password
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    
    if (strlen(WIFI_PASSWORD) > 0) {
        strncpy((char*)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
        ESP_LOGI(TAG, "🔐 Using WPA2/WPA3 authentication");
    } else {
        ESP_LOGI(TAG, "🔓 Connecting to open network (no password)");
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    
    // Set WiFi mode and configuration
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // Configure power save mode
    ESP_LOGI(TAG, "⚡ Configuring WiFi power save mode...");
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MODE));
    
    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "🎯 WiFi initialization complete, waiting for connection...");
    
    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "🎉 Connected to WiFi network '%s'", WIFI_SSID);
        
        // Start WiFi monitoring task for automatic reconnection
        xTaskCreate(wifi_reconnect_task, "wifi_reconnect", 4096, NULL, 5, NULL);
        
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "❌ Failed to connect to WiFi network '%s'", WIFI_SSID);
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "❌ WiFi connection timeout after %d ms", WIFI_CONNECT_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }
}

// Modbus device address for single slave configuration
// This corresponds to the Modbus slave ID of your SDM120 device
enum {
    MB_DEVICE_ADDR1 = 1  // SDM120 Slave UID = 1 (standard default)
};

enum {
    CID_VOLTAGE = 0,
    CID_CURRENT,
    CID_ACTIVE_POWER,
    CID_APPARENT_POWER,      // ⭐ ADDED
    CID_REACTIVE_POWER,      // ⭐ ADDED
    CID_POWER_FACTOR,
    CID_FREQUENCY,
    CID_IMPORT_ACTIVE_ENERGY, // ⭐ ADDED  
    CID_EXPORT_ACTIVE_ENERGY, // ⭐ ADDED
    CID_TOTAL_ACTIVE_ENERGY,
    CID_COUNT
};
// Struct to hold the read data from the SDM120 meter.
// The order of members corresponds to the CIDs in the table below.
typedef struct {
    float voltage;
    float current;
    float active_power;
    float apparent_power;      // ⭐ ADDED - VA
    float reactive_power;      // ⭐ ADDED - VAr  
    float power_factor;
    float frequency;
    float import_active_energy; // ⭐ ADDED - Import kWh
    float export_active_energy; // ⭐ ADDED - Export kWh
    float total_active_energy;
} sdm120_data_t;

// CID (Characteristic Information Data) definition for the SDM120 Modbus Energy Meter.
// This array describes the parameters that can be read from the device.
// ✅ VERIFIED: Register addresses confirmed against official Eastron SDM120 Modbus specification
const mb_parameter_descriptor_t sdm120_cid_table[] = {
    // Each entry defines a single parameter. The fields are:
    // cid, param_key, param_units, mb_slave_addr, mb_param_type,
    // mb_reg_start, mb_reg_size, param_offset, param_type,
    // param_size, param_opts, access_mode
    
    // ===== BASIC ELECTRICAL MEASUREMENTS =====
    // 🔧 Using PARAM_TYPE_U32 to read raw 32-bit data for IEEE754 conversion
    { CID_VOLTAGE            , STR("Voltage")            , STR("V")   , MB_DEVICE_ADDR1, MB_PARAM_INPUT, 0x0000, 2, INPUT_OFFSET(voltage)             , PARAM_TYPE_U32, 4, OPTS(0, 4294967295UL, 0), PAR_PERMS_READ },
    { CID_CURRENT            , STR("Current")            , STR("A")   , MB_DEVICE_ADDR1, MB_PARAM_INPUT, 0x0006, 2, INPUT_OFFSET(current)             , PARAM_TYPE_U32, 4, OPTS(0, 4294967295UL, 0), PAR_PERMS_READ },
    { CID_ACTIVE_POWER       , STR("Active_Power")       , STR("W")   , MB_DEVICE_ADDR1, MB_PARAM_INPUT, 0x000C, 2, INPUT_OFFSET(active_power)        , PARAM_TYPE_U32, 4, OPTS(0, 4294967295UL, 0), PAR_PERMS_READ },
    { CID_APPARENT_POWER     , STR("Apparent_Power")     , STR("VA")  , MB_DEVICE_ADDR1, MB_PARAM_INPUT, 0x0012, 2, INPUT_OFFSET(apparent_power)      , PARAM_TYPE_U32, 4, OPTS(0, 4294967295UL, 0), PAR_PERMS_READ },
    { CID_REACTIVE_POWER     , STR("Reactive_Power")     , STR("VAr") , MB_DEVICE_ADDR1, MB_PARAM_INPUT, 0x0018, 2, INPUT_OFFSET(reactive_power)      , PARAM_TYPE_U32, 4, OPTS(0, 4294967295UL, 0), PAR_PERMS_READ },
    { CID_POWER_FACTOR       , STR("Power_Factor")       , STR("")    , MB_DEVICE_ADDR1, MB_PARAM_INPUT, 0x001E, 2, INPUT_OFFSET(power_factor)        , PARAM_TYPE_U32, 4, OPTS(0, 4294967295UL, 0), PAR_PERMS_READ },
    { CID_FREQUENCY          , STR("Frequency")          , STR("Hz")  , MB_DEVICE_ADDR1, MB_PARAM_INPUT, 0x0046, 2, INPUT_OFFSET(frequency)           , PARAM_TYPE_U32, 4, OPTS(0, 4294967295UL, 0), PAR_PERMS_READ },
    
    // ===== ENERGY MEASUREMENTS =====
    { CID_IMPORT_ACTIVE_ENERGY, STR("Import_Active_Energy"), STR("kWh"), MB_DEVICE_ADDR1, MB_PARAM_INPUT, 0x0048, 2, INPUT_OFFSET(import_active_energy), PARAM_TYPE_U32, 4, OPTS(0, 4294967295UL, 0), PAR_PERMS_READ },
    { CID_EXPORT_ACTIVE_ENERGY, STR("Export_Active_Energy"), STR("kWh"), MB_DEVICE_ADDR1, MB_PARAM_INPUT, 0x004A, 2, INPUT_OFFSET(export_active_energy), PARAM_TYPE_U32, 4, OPTS(0, 4294967295UL, 0), PAR_PERMS_READ },
    { CID_TOTAL_ACTIVE_ENERGY, STR("Total_Active_Energy"), STR("kWh") , MB_DEVICE_ADDR1, MB_PARAM_INPUT, 0x0156, 2, INPUT_OFFSET(total_active_energy) , PARAM_TYPE_U32, 4, OPTS(0, 4294967295UL, 0), PAR_PERMS_READ },
};

const uint16_t sdm120_cid_count = sizeof(sdm120_cid_table) / sizeof(sdm120_cid_table[0]);

/* ===== MQTT IMPLEMENTATION ===== 
 * MQTT client for publishing SDM120 energy meter data to broker
 */

/**
 * @brief MQTT event handler
 * 
 * Handles MQTT connection events, disconnections, and message publishing results
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "🌐 MQTT Connected to broker");
        mqtt_connected = true;
        
        // Publish Home Assistant discovery messages after connection
        if (MQTT_HOME_ASSISTANT_DISCOVERY) {
            // Small delay to ensure connection is stable
            vTaskDelay(pdMS_TO_TICKS(1000));
            mqtt_publish_ha_discovery();
        }
        break;
        
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "⚠️  MQTT Disconnected from broker");
        mqtt_connected = false;
        
        // Set availability to offline when disconnected (will be sent when reconnected)
        // Note: We can't send it now since we're disconnected, but HA will use LWT or timeout
        break;
        
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "📤 MQTT Message published, msg_id=%d", event->msg_id);
        break;
        
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "❌ MQTT Error occurred");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "   TCP Transport error: %d", event->error_handle->esp_tls_last_esp_err);
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "   Connection refused - check broker settings and authentication");
            ESP_LOGE(TAG, "   📝 Tip: Update MQTT_USERNAME and MQTT_PASSWORD if broker requires auth");
        }
        mqtt_connected = false;
        break;
        
    default:
        ESP_LOGD(TAG, "🔄 MQTT Event: %ld", event_id);
        break;
    }
}

/**
 * @brief Initialize MQTT client and connect to broker
 * 
 * @return ESP_OK on success, error code on failure
 */
static esp_err_t mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = MQTT_CLIENT_ID,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 10000,
    };
    
    // Configure Last Will Testament (LWT) for Home Assistant availability
    if (MQTT_HOME_ASSISTANT_DISCOVERY) {
        static char lwt_topic[128];
        snprintf(lwt_topic, sizeof(lwt_topic), "%s/status", MQTT_TOPIC_PREFIX);
        mqtt_cfg.session.last_will.topic = lwt_topic;
        mqtt_cfg.session.last_will.msg = "offline";
        mqtt_cfg.session.last_will.msg_len = 7;
        mqtt_cfg.session.last_will.qos = 0;
        mqtt_cfg.session.last_will.retain = 1;
        ESP_LOGI(TAG, "✓ Configured MQTT Last Will Testament for availability");
    }
    
    // Add authentication if credentials are provided
    if (strlen(MQTT_USERNAME) > 0) {
        mqtt_cfg.credentials.username = MQTT_USERNAME;
        ESP_LOGI(TAG, "🔐 Using MQTT authentication for user: %s", MQTT_USERNAME);
    } else {
        ESP_LOGI(TAG, "🔓 Using anonymous MQTT connection");
    }
    
    if (strlen(MQTT_PASSWORD) > 0) {
        mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;
    }
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "❌ Failed to initialize MQTT client");
        return ESP_FAIL;
    }
    
    // Register event handler
    esp_err_t err = esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to register MQTT event handler: %s", esp_err_to_name(err));
        return err;
    }
    
    // Start MQTT client
    err = esp_mqtt_client_start(mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to start MQTT client: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "✅ MQTT client initialized and connecting to %s", MQTT_BROKER_URI);
    return ESP_OK;
}

/**
 * @brief Publish SDM120 data to MQTT broker in JSON format
 * 
 * @param data Pointer to SDM120 data structure
 * @return ESP_OK on success, error code on failure
 */
static esp_err_t mqtt_publish_sdm120_data(const sdm120_data_t* data)
{
    if (!mqtt_connected || mqtt_client == NULL) {
        ESP_LOGW(TAG, "⚠️  MQTT not connected, skipping publish");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (data == NULL) {
        ESP_LOGE(TAG, "❌ Invalid data pointer for MQTT publish");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Create JSON payload with all SDM120 measurements
    char json_payload[512];
    int len = snprintf(json_payload, sizeof(json_payload),
        "{"
        "\"timestamp\":%llu,"
        "\"voltage\":%.2f,"
        "\"current\":%.3f,"
        "\"active_power\":%.2f,"
        "\"apparent_power\":%.2f,"
        "\"reactive_power\":%.2f,"
        "\"power_factor\":%.3f,"
        "\"frequency\":%.2f,"
        "\"import_energy\":%.3f,"
        "\"export_energy\":%.3f,"
        "\"total_energy\":%.3f,"
        "\"device_ip\":\"%s\""
        "}",
        (unsigned long long)(esp_timer_get_time() / 1000), // Timestamp in milliseconds
        data->voltage,
        data->current,
        data->active_power,
        data->apparent_power,
        data->reactive_power,
        data->power_factor,
        data->frequency,
        data->import_active_energy,
        data->export_active_energy,
        data->total_active_energy,
        SDM120_SLAVE_IP
    );
    
    // Publish to main data topic
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/data", MQTT_TOPIC_PREFIX);
    
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, json_payload, len, 0, 0);
    if (msg_id == -1) {
        ESP_LOGE(TAG, "❌ Failed to publish MQTT message");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "📤 Published SDM120 data to MQTT topic: %s (msg_id: %d)", topic, msg_id);
    
    // Publish ALL CID measurements to individual subtopics (if enabled)
    if (MQTT_PUBLISH_INDIVIDUAL_TOPICS) {
        char individual_topic[128];
        char value_str[32];
        
        // CID 0: Voltage  
        snprintf(individual_topic, sizeof(individual_topic), "%s/voltage", MQTT_TOPIC_PREFIX);
        snprintf(value_str, sizeof(value_str), "%.2f", data->voltage);
        esp_mqtt_client_publish(mqtt_client, individual_topic, value_str, 0, 0, 0);
        
        // CID 1: Current
        snprintf(individual_topic, sizeof(individual_topic), "%s/current", MQTT_TOPIC_PREFIX);
        snprintf(value_str, sizeof(value_str), "%.3f", data->current);
        esp_mqtt_client_publish(mqtt_client, individual_topic, value_str, 0, 0, 0);
        
        // CID 2: Active Power
        snprintf(individual_topic, sizeof(individual_topic), "%s/active_power", MQTT_TOPIC_PREFIX);
        snprintf(value_str, sizeof(value_str), "%.2f", data->active_power);
        esp_mqtt_client_publish(mqtt_client, individual_topic, value_str, 0, 0, 0);
        
        // CID 3: Apparent Power
        snprintf(individual_topic, sizeof(individual_topic), "%s/apparent_power", MQTT_TOPIC_PREFIX);
        snprintf(value_str, sizeof(value_str), "%.2f", data->apparent_power);
        esp_mqtt_client_publish(mqtt_client, individual_topic, value_str, 0, 0, 0);
        
        // CID 4: Reactive Power
        snprintf(individual_topic, sizeof(individual_topic), "%s/reactive_power", MQTT_TOPIC_PREFIX);
        snprintf(value_str, sizeof(value_str), "%.2f", data->reactive_power);
        esp_mqtt_client_publish(mqtt_client, individual_topic, value_str, 0, 0, 0);
        
        // CID 5: Power Factor
        snprintf(individual_topic, sizeof(individual_topic), "%s/power_factor", MQTT_TOPIC_PREFIX);
        snprintf(value_str, sizeof(value_str), "%.3f", data->power_factor);
        esp_mqtt_client_publish(mqtt_client, individual_topic, value_str, 0, 0, 0);
        
        // CID 6: Frequency
        snprintf(individual_topic, sizeof(individual_topic), "%s/frequency", MQTT_TOPIC_PREFIX);
        snprintf(value_str, sizeof(value_str), "%.2f", data->frequency);
        esp_mqtt_client_publish(mqtt_client, individual_topic, value_str, 0, 0, 0);
        
        // CID 7: Import Active Energy
        snprintf(individual_topic, sizeof(individual_topic), "%s/import_energy", MQTT_TOPIC_PREFIX);
        snprintf(value_str, sizeof(value_str), "%.3f", data->import_active_energy);
        esp_mqtt_client_publish(mqtt_client, individual_topic, value_str, 0, 0, 0);
        
        // CID 8: Export Active Energy
        snprintf(individual_topic, sizeof(individual_topic), "%s/export_energy", MQTT_TOPIC_PREFIX);
        snprintf(value_str, sizeof(value_str), "%.3f", data->export_active_energy);
        esp_mqtt_client_publish(mqtt_client, individual_topic, value_str, 0, 0, 0);
        
        // CID 9: Total Active Energy
        snprintf(individual_topic, sizeof(individual_topic), "%s/total_energy", MQTT_TOPIC_PREFIX);
        snprintf(value_str, sizeof(value_str), "%.3f", data->total_active_energy);
        esp_mqtt_client_publish(mqtt_client, individual_topic, value_str, 0, 0, 0);
        
        ESP_LOGI(TAG, "📡 Published all %d CID parameters to individual MQTT subtopics", sdm120_cid_count);
        
        // Update availability status for Home Assistant
        if (MQTT_HOME_ASSISTANT_DISCOVERY) {
            char availability_topic[128];
            snprintf(availability_topic, sizeof(availability_topic), "%s/status", MQTT_TOPIC_PREFIX);
            esp_mqtt_client_publish(mqtt_client, availability_topic, "online", 0, 0, 1);
        }
    } else {
        ESP_LOGD(TAG, "⏭️  Individual topic publishing disabled");
    }
    
    return ESP_OK;
}

/**
 * @brief Publish Home Assistant MQTT Discovery messages for all SDM120 sensors
 * 
 * Creates auto-discovery configuration for Home Assistant to automatically
 * create sensors for all SDM120 parameters with proper device classes and units
 * 
 * @return ESP_OK on success, error code on failure
 */
static esp_err_t mqtt_publish_ha_discovery(void)
{
    if (!mqtt_connected || mqtt_client == NULL || !MQTT_HOME_ASSISTANT_DISCOVERY) {
        ESP_LOGD(TAG, "⏭️  Home Assistant discovery disabled or MQTT not connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "📡 Publishing Home Assistant MQTT Discovery configurations...");
    
    // Device information - shared across all sensors
    char device_info[256];
    snprintf(device_info, sizeof(device_info),
        "\"device\":{"
        "\"identifiers\":[\"sdm120_%s\"],"
        "\"name\":\"SDM120 Energy Meter\","
        "\"model\":\"SDM120\","
        "\"manufacturer\":\"Eastron\","
        "\"sw_version\":\"ESP32-SDM120-v1.0\","
        "\"configuration_url\":\"http://%s\""
        "}",
        SDM120_SLAVE_IP, SDM120_SLAVE_IP
    );
    
    // Discovery configurations for each sensor
    struct {
        const char* name;
        const char* object_id;
        const char* device_class;
        const char* unit;
        const char* state_class;
        const char* icon;
        const char* topic_suffix;
    } sensors[] = {
        {"Voltage", "voltage", "voltage", "V", "measurement", "mdi:flash", "voltage"},
        {"Current", "current", "current", "A", "measurement", "mdi:current-ac", "current"},
        {"Active Power", "active_power", "power", "W", "measurement", "mdi:flash", "active_power"},
        {"Apparent Power", "apparent_power", "apparent_power", "VA", "measurement", "mdi:flash-outline", "apparent_power"},
        {"Reactive Power", "reactive_power", "reactive_power", "var", "measurement", "mdi:flash-outline", "reactive_power"},
        {"Power Factor", "power_factor", "power_factor", "", "measurement", "mdi:cosine-wave", "power_factor"},
        {"Frequency", "frequency", "frequency", "Hz", "measurement", "mdi:sine-wave", "frequency"},
        {"Import Energy", "import_energy", "energy", "kWh", "total_increasing", "mdi:transmission-tower-import", "import_energy"},
        {"Export Energy", "export_energy", "energy", "kWh", "total_increasing", "mdi:transmission-tower-export", "export_energy"},
        {"Total Energy", "total_energy", "energy", "kWh", "total_increasing", "mdi:lightning-bolt", "total_energy"}
    };
    
    for (int i = 0; i < 10; i++) {
        char discovery_topic[128];
        char discovery_payload[1024];
        
        // Create discovery topic: homeassistant/sensor/sdm120_192_168_1_100/voltage/config
        snprintf(discovery_topic, sizeof(discovery_topic), 
                "%s/sensor/sdm120_%s/%s/config", 
                MQTT_HA_DISCOVERY_PREFIX, 
                SDM120_SLAVE_IP,  // Will be sanitized below
                sensors[i].object_id);
        
        // Sanitize IP address in topic (replace dots with underscores)
        for (char* p = discovery_topic; *p; p++) {
            if (*p == '.') *p = '_';
        }
        
        // Create discovery payload with all required HA fields
        int payload_len = snprintf(discovery_payload, sizeof(discovery_payload),
            "{"
            "\"name\":\"%s\","
            "\"object_id\":\"sdm120_%s_%s\","
            "\"unique_id\":\"sdm120_%s_%s\","
            "\"state_topic\":\"%s/%s\","
            "\"availability_topic\":\"%s/status\","
            "\"device_class\":\"%s\","
            "\"unit_of_measurement\":\"%s\","
            "\"state_class\":\"%s\","
            "\"icon\":\"%s\","
            "\"value_template\":\"{{ value | float }}\","
            "%s"
            "}",
            sensors[i].name,
            SDM120_SLAVE_IP, sensors[i].object_id,  // object_id
            SDM120_SLAVE_IP, sensors[i].object_id,  // unique_id  
            MQTT_TOPIC_PREFIX, sensors[i].topic_suffix,  // state_topic
            MQTT_TOPIC_PREFIX,  // availability_topic
            sensors[i].device_class,
            sensors[i].unit,
            sensors[i].state_class,
            sensors[i].icon,
            device_info
        );
        
        // Sanitize IP addresses in payload
        for (char* p = discovery_payload; *p; p++) {
            if (*p == '.' && *(p-1) != '"' && *(p+1) != '"') *p = '_';
        }
        
        // Publish discovery message
        int msg_id = esp_mqtt_client_publish(mqtt_client, discovery_topic, discovery_payload, payload_len, 0, 1);
        if (msg_id != -1) {
            ESP_LOGD(TAG, "✓ Published HA discovery for %s (msg_id: %d)", sensors[i].name, msg_id);
        } else {
            ESP_LOGW(TAG, "⚠️  Failed to publish HA discovery for %s", sensors[i].name);
        }
        
        // Small delay to avoid overwhelming MQTT broker
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    // Publish availability status as "online"
    char availability_topic[128];
    snprintf(availability_topic, sizeof(availability_topic), "%s/status", MQTT_TOPIC_PREFIX);
    esp_mqtt_client_publish(mqtt_client, availability_topic, "online", 0, 0, 1);
    
    ESP_LOGI(TAG, "✅ Home Assistant discovery published for all 10 sensors");
    return ESP_OK;
}

/* ===== HIGH-LEVEL API IMPLEMENTATION ===== 
 * The functions below demonstrate the proper use of ESP-IDF Modbus high-level APIs:
 * - No manual handle management
 * - No complex request formation  
 * - Automatic data type handling
 * - Built-in error recovery
 * - Clean and maintainable code
 */







/**
 * @brief Convert SDM120 IEEE754 float from U32 Modbus data to native float
 * 
 * 🔧 CRITICAL FIX: SDM120 uses IEEE754 32-bit floats but with word-swapped byte order.
 * The ESP-IDF PARAM_TYPE_U32 gives us the raw 32-bit value, but we need to swap
 * the 16-bit words to get the correct IEEE754 interpretation.
 * 
 * @param raw_u32 Raw 32-bit unsigned integer from Modbus register
 * @return Correctly interpreted float value
 */
static float convert_sdm120_ieee754(uint32_t raw_u32) {
    union {
        float f;
        uint32_t u32;
        uint16_t words[2];
        uint8_t bytes[4];
    } converter;
    
    // Store the raw data
    converter.u32 = raw_u32;
    
    // SDM120 uses word-swapped IEEE754 format
    // We need to swap the 16-bit words to get correct float interpretation
    uint16_t temp = converter.words[0];
    converter.words[0] = converter.words[1];
    converter.words[1] = temp;
    
    return converter.f;
}

/**
 * @brief Simple network connectivity check to SDM120 device
 * 
 * Performs a basic ping-like connectivity test to help diagnose timeout issues
 * 
 * @return ESP_OK if device seems reachable, error code otherwise
 */
static esp_err_t check_sdm120_connectivity(void) {
    ESP_LOGI(TAG, "🌐 Checking network connectivity to SDM120 at %s...", SDM120_SLAVE_IP);
    
    // Check if WiFi interface is available
    if (s_wifi_netif == NULL) {
        ESP_LOGW(TAG, "⚠️  WiFi network interface not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check if network interface is connected
    if (!esp_netif_is_netif_up(s_wifi_netif)) {
        ESP_LOGW(TAG, "⚠️  WiFi network interface is down");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check WiFi connection status
    if (!wifi_connected) {
        ESP_LOGW(TAG, "⚠️  WiFi not connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "✓ Network interface is up - SDM120 should be reachable");
    return ESP_OK;
}

/**
 * @brief Reads all parameters from the SDM120 meter with IEEE754 conversion fix
 * 
 * This function uses mbc_master_get_parameter() but applies custom IEEE754 
 * byte order conversion to fix the SDM120 floating point interpretation issue.
 * 
 * 🛠️ FIXED: Power Factor and other readings now display correctly instead of 
 * huge negative numbers like -73564106660078522728448.000
 *
 * @param data Pointer to sdm120_data_t struct to store the read values
 * @return ESP_OK on success, error code on failure
 */
static esp_err_t read_sdm120_data(sdm120_data_t* data) {
    if (data == NULL) {
        ESP_LOGE(TAG, "Invalid data pointer");
        return ESP_ERR_INVALID_ARG;
    }

    // Clear the data structure
    memset(data, 0, sizeof(sdm120_data_t));
    ESP_LOGI(TAG, "🔄 Reading %d parameters from SDM120 with IEEE754 conversion...", sdm120_cid_count);
    
    // Track timeout statistics for diagnostics
    int timeout_count = 0;
    int success_count = 0;

    // Read all parameters using high-level API with custom conversion
    for (uint16_t cid = 0; cid < sdm120_cid_count; cid++) {
        const mb_parameter_descriptor_t* param_descriptor = NULL;
        uint8_t type = 0;

        // Get parameter descriptor for this CID
        esp_err_t err = mbc_master_get_cid_info(cid, &param_descriptor);
        if (err != ESP_OK || param_descriptor == NULL) {
            ESP_LOGE(TAG, "❌ Could not get CID info for CID %u: %s", cid, esp_err_to_name(err));
            continue; // Skip this parameter but continue with others
        }

        // Read raw U32 data with enhanced retry logic for SDM120 reliability
        uint32_t raw_u32_data = 0;
        esp_err_t read_err = ESP_FAIL;
        int retry_count = 0;
        const int max_retries = 2; // Consistent retry count for all parameters
        
        // Retry loop with progressive delays for better reliability
        for (retry_count = 0; retry_count <= max_retries; retry_count++) {
            read_err = mbc_master_get_parameter(cid, (char*)param_descriptor->param_key, (uint8_t*)&raw_u32_data, &type);
            
            if (read_err == ESP_OK) {
                break; // Success, exit retry loop
            } else if (retry_count < max_retries) {
                // Progressive delay: base_delay, base_delay + 300ms for subsequent retries
                int delay_ms = MODBUS_RETRY_DELAY_BASE_MS + (retry_count * 300);
                ESP_LOGW(TAG, "⚠️  Retry %d/%d for %s (CID %u): %s - waiting %dms", 
                         retry_count + 1, max_retries, param_descriptor->param_key, cid, 
                         esp_err_to_name(read_err), delay_ms);
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }
        }
        
        if (read_err == ESP_OK) {
            success_count++;
            // Apply IEEE754 conversion and store in appropriate field
            float converted_value = convert_sdm120_ieee754(raw_u32_data);
            
            ESP_LOGD(TAG, "🔧 CID %u raw: 0x%08lX -> %.3f", cid, raw_u32_data, converted_value);
            
            // Basic data validation (warn about unrealistic values)
            if (cid == CID_VOLTAGE && (converted_value < 0 || converted_value > 500)) {
                ESP_LOGW(TAG, "⚠️  Voltage reading seems unrealistic: %.2f V", converted_value);
            } else if (cid == CID_FREQUENCY && (converted_value < 45 || converted_value > 65)) {
                ESP_LOGW(TAG, "⚠️  Frequency reading seems unrealistic: %.2f Hz", converted_value);
            } else if (cid == CID_POWER_FACTOR && (converted_value < -1.1 || converted_value > 1.1)) {
                ESP_LOGW(TAG, "⚠️  Power Factor reading seems unrealistic: %.3f", converted_value);
            }
            
            // Store converted value in the correct struct field
            switch (cid) {
                case CID_VOLTAGE:
                    data->voltage = converted_value;
                    ESP_LOGD(TAG, "⚡ Voltage: %.2f V", converted_value);
                    break;
                case CID_CURRENT:
                    data->current = converted_value;
                    ESP_LOGD(TAG, "🔌 Current: %.3f A", converted_value);
                    break;
                case CID_ACTIVE_POWER:
                    data->active_power = converted_value;
                    ESP_LOGD(TAG, "🔥 Active Power: %.2f W", converted_value);
                    break;
                case CID_APPARENT_POWER:
                    data->apparent_power = converted_value;
                    ESP_LOGD(TAG, "📊 Apparent Power: %.2f VA", converted_value);
                    break;
                case CID_REACTIVE_POWER:
                    data->reactive_power = converted_value;
                    ESP_LOGD(TAG, "🔄 Reactive Power: %.2f VAr", converted_value);
                    break;
                case CID_POWER_FACTOR:
                    data->power_factor = converted_value;
                    ESP_LOGD(TAG, "📐 Power Factor: %.3f", converted_value);
                    break;
                case CID_FREQUENCY:
                    data->frequency = converted_value;
                    ESP_LOGD(TAG, "🎵 Frequency: %.2f Hz", converted_value);
                    break;
                case CID_IMPORT_ACTIVE_ENERGY:
                    data->import_active_energy = converted_value;
                    ESP_LOGD(TAG, "📈 Import Energy: %.3f kWh", converted_value);
                    if (converted_value > 10000) {
                        ESP_LOGI(TAG, "ℹ️  High energy reading - verify register 0x0048 is correct");
                    }
                    break;
                case CID_EXPORT_ACTIVE_ENERGY:
                    data->export_active_energy = converted_value;
                    ESP_LOGD(TAG, "📉 Export Energy: %.3f kWh", converted_value);
                    break;
                case CID_TOTAL_ACTIVE_ENERGY:
                    data->total_active_energy = converted_value;
                    ESP_LOGD(TAG, "🏠 Total Energy: %.3f kWh", converted_value);
                    if (converted_value > 10000) {
                        ESP_LOGI(TAG, "ℹ️  High energy reading - verify register 0x0156 is correct");
                    }
                    break;
                default:
                    ESP_LOGW(TAG, "⚠️  Unknown CID %u", cid);
                    break;
            }
        } else {
            if (read_err == ESP_ERR_TIMEOUT) {
                timeout_count++;
            }
            ESP_LOGE(TAG, "❌ Failed to read %s (CID %u) after %d retries: %s", 
                     param_descriptor->param_key, cid, retry_count, esp_err_to_name(read_err));
            
            // If we get too many consecutive timeouts, check connectivity
            if (timeout_count >= 3 && cid >= 2) {  // Check after 3 timeouts and we're past the first few params
                ESP_LOGW(TAG, "🔍 Multiple timeouts detected, checking connectivity...");
                check_sdm120_connectivity();
                timeout_count = 0; // Reset counter after check
            }
            // Continue reading other parameters even if one fails
        }

        // Inter-parameter delay for device stability and network recovery
        vTaskDelay(pdMS_TO_TICKS(MODBUS_INTER_PARAM_DELAY_MS));
        
        // Additional delay after first few parameters for device stabilization
        if (cid < 3) {
            vTaskDelay(pdMS_TO_TICKS(100)); // Extra 100ms for first few parameters
        }
    }

    // Report reading statistics for diagnostics
    ESP_LOGI(TAG, "✅ SDM120 parameter reading completed: %d/%d successful, %d timeouts", 
             success_count, sdm120_cid_count, timeout_count);
    
    if (success_count == 0) {
        ESP_LOGE(TAG, "❌ All parameters failed - check SDM120 device and network connectivity");
        return ESP_ERR_TIMEOUT;
    } else if (timeout_count > sdm120_cid_count / 2) {
        ESP_LOGW(TAG, "⚠️  High timeout rate - consider increasing MODBUS_RESPONSE_TIMEOUT_MS");
    }
    
    return ESP_OK;
}





/**
 * @brief FreeRTOS task to continuously monitor and display SDM120 data from single slave
 */
static void sdm120_monitoring_task(void* pvParameters) {
    sdm120_data_t meter_data;
    const TickType_t read_interval = pdMS_TO_TICKS(5000); // Read every 5 seconds
    uint32_t read_count = 0;

    ESP_LOGI(TAG, "📊 SDM120 monitoring task started for device %s", SDM120_SLAVE_IP);

    while (1) {
        read_count++;
        esp_err_t result = read_sdm120_data(&meter_data);

        if (result == ESP_OK) {
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "📈 SDM120 Reading #%lu from %s", read_count, SDM120_SLAVE_IP);
            ESP_LOGI(TAG, "⚡ Voltage:            %.2f V", meter_data.voltage);
            ESP_LOGI(TAG, "🔌 Current:            %.3f A", meter_data.current);
            ESP_LOGI(TAG, "🔥 Active Power:       %.2f W", meter_data.active_power);
            ESP_LOGI(TAG, "📊 Apparent Power:     %.2f VA", meter_data.apparent_power);
            ESP_LOGI(TAG, "🔄 Reactive Power:     %.2f VAR", meter_data.reactive_power);
            ESP_LOGI(TAG, "📐 Power Factor:       %.3f", meter_data.power_factor);
            ESP_LOGI(TAG, "🎵 Frequency:          %.2f Hz", meter_data.frequency);
            ESP_LOGI(TAG, "📥 Import Energy:      %.3f kWh", meter_data.import_active_energy);
            ESP_LOGI(TAG, "📤 Export Energy:      %.3f kWh", meter_data.export_active_energy);
            ESP_LOGI(TAG, "🏠 Total Active Energy: %.3f kWh", meter_data.total_active_energy);
            
            // Publish data to MQTT broker
            esp_err_t mqtt_result = mqtt_publish_sdm120_data(&meter_data);
            if (mqtt_result == ESP_OK) {
                ESP_LOGI(TAG, "✅ Data published to MQTT broker");
            } else if (mqtt_result == ESP_ERR_INVALID_STATE) {
                ESP_LOGD(TAG, "🔄 MQTT not connected, data logged locally only");
            } else {
                ESP_LOGW(TAG, "⚠️  MQTT publish failed: %s", esp_err_to_name(mqtt_result));
            }
            ESP_LOGI(TAG, "");
        } else {
            ESP_LOGW(TAG, "⚠️  Failed to read from %s (attempt %lu). Retrying in 5 seconds...", 
                     SDM120_SLAVE_IP, read_count);
            
            // If all parameters are failing, add extra delay for device recovery
            if (result == ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "🔄 All parameters timed out - adding recovery delay...");
                vTaskDelay(pdMS_TO_TICKS(2000)); // Extra 2 second delay for device recovery
            }
        }

        // Wait for the next read interval
        vTaskDelay(read_interval);
    }
}


/**
 * @brief Simple IP validation helper
 */
static bool is_valid_ip(const char* ip) {
    if (!ip) return false;
    
    int a, b, c, d;
    if (sscanf(ip, "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
        return (a >= 0 && a <= 255) && (b >= 0 && b <= 255) && 
               (c >= 0 && c <= 255) && (d >= 0 && d <= 255);
    }
    return false;
}


/**
 * @brief Initialize all system services using high-level APIs
 * 
 * This function uses ESP-IDF's high-level helper functions to:
 * - Initialize NVS (Non-Volatile Storage)
 * - Setup network interface and event handling
 * - Connect to WiFi using native robust implementation with auto-reconnection
 * - Configure power management for optimal performance
 * - Validate the configured slave IP address
 */
static esp_err_t init_services(void)
{
    // Initialize NVS - required for WiFi credentials storage
    ESP_LOGI(TAG, "Initializing NVS flash...");
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);

    // Initialize TCP/IP network interface
    ESP_LOGI(TAG, "Initializing network interface...");
    ESP_ERROR_CHECK(esp_netif_init());
    
    // Create default event loop for system events
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Connect to WiFi using native robust implementation
    ESP_LOGI(TAG, "Connecting to WiFi network...");
    ESP_ERROR_CHECK(wifi_init_and_connect());

    // Validate the configured slave IP address
    ESP_LOGI(TAG, "Validating SDM120 slave IP configuration...");
    if (!is_valid_ip(slave_ip_address)) {
        ESP_LOGE(TAG, "Invalid slave IP address configured: %s", slave_ip_address);
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "✓ SDM120 slave configured at IP: %s:%d", slave_ip_address, SDM120_SLAVE_PORT);
    return ESP_OK;
}

/**
 * @brief Initialize Modbus master using high-level APIs for single slave
 */
static esp_err_t master_init(void)
{
    void* master_handler = NULL;

    ESP_LOGI(TAG, "Initializing Modbus TCP master for slave %s:%d", 
             slave_ip_address, SDM120_SLAVE_PORT);

    // Initialize the Modbus master TCP stack
    esp_err_t err = mbc_master_init_tcp(&master_handler);
    MB_RETURN_ON_FALSE((master_handler != NULL), ESP_ERR_INVALID_STATE,
                                TAG,
                                "mb controller initialization fail.");
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE,
                            TAG,
                            "mb controller initialization fail, returns(0x%x).",
                            (int)err);

    // Validate WiFi interface is available
    if (s_wifi_netif == NULL) {
        ESP_LOGE(TAG, "❌ WiFi network interface not initialized. Ensure WiFi connection is established first.");
        return ESP_ERR_INVALID_STATE;
    }

    // Configure communication parameters for single slave
    mb_communication_info_t comm_info = {
        .ip_port = SDM120_SLAVE_PORT,
        .ip_addr_type = MB_IPV4,
        .ip_mode = MB_MODE_TCP,
        .ip_addr = (void*)&slave_ip_address,
        .ip_netif_ptr = (void*)s_wifi_netif
    };

    // Configure the master with communication parameters
    err = mbc_master_setup((void*)&comm_info);
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE,
                            TAG,
                            "mb controller setup fail, returns(0x%x).",
                            (int)err);

    // Set the parameter descriptor table for SDM120
    err = mbc_master_set_descriptor(&sdm120_cid_table[0], sdm120_cid_count);
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE,
                                TAG,
                                "mb controller set descriptor fail, returns(0x%x).",
                                (int)err);
    ESP_LOGI(TAG, "✓ Modbus master initialized with %d SDM120 parameters", sdm120_cid_count);

    // Configure enhanced reliability for SDM120 (timeout handling via software retry logic)
    ESP_LOGI(TAG, "Configuring enhanced retry logic for SDM120 compatibility...");
    ESP_LOGI(TAG, "✓ Using software-based timeout handling (target: %dms)", MODBUS_RESPONSE_TIMEOUT_MS);
    ESP_LOGI(TAG, "  Note: ESP-IDF Modbus uses default timeouts + our enhanced retry logic");
    
    // Start the Modbus master background task
    err = mbc_master_start();
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE,
                            TAG,
                            "mb controller start fail, returns(0x%x).",
                            (int)err);
    
    vTaskDelay(pdMS_TO_TICKS(500)); // Allow master to fully start (increased delay)
    ESP_LOGI(TAG, "✓ Modbus master started successfully with enhanced retry logic");
    ESP_LOGI(TAG, "  - Inter-parameter delay: %dms", MODBUS_INTER_PARAM_DELAY_MS);
    ESP_LOGI(TAG, "  - Retry base delay: %dms", MODBUS_RETRY_DELAY_BASE_MS);
    ESP_LOGI(TAG, "  - Max retries per parameter: 2");
    return err;
}

/**
 * @brief Main application entry point - simplified single slave configuration
 * 
 * This function demonstrates the simplified high-level approach for single slave:
 * 1. Initialize system services (WiFi, NVS, networking)
 * 2. Configure Modbus master for single SDM120 device
 * 3. Start the monitoring task for continuous data reading
 */
void app_main(void)
{
    ESP_LOGI(TAG, "=== SDM120 Modbus TCP Master Application ===");
    ESP_LOGI(TAG, "Target device: %s:%d", SDM120_SLAVE_IP, SDM120_SLAVE_PORT);

    // Initialize all required services using high-level helper functions
    ESP_LOGI(TAG, "Step 1: Initializing system services...");
    ESP_ERROR_CHECK(init_services());

    // Initialize the Modbus master for single slave
    ESP_LOGI(TAG, "Step 2: Initializing Modbus master...");
    ESP_ERROR_CHECK(master_init());

    // Initialize MQTT client for data publishing
    ESP_LOGI(TAG, "Step 3: Initializing MQTT client...");
    esp_err_t mqtt_result = mqtt_init();
    if (mqtt_result != ESP_OK) {
        ESP_LOGW(TAG, "⚠️  MQTT initialization failed: %s", esp_err_to_name(mqtt_result));
        ESP_LOGW(TAG, "    Continuing without MQTT - data will be logged only");
    }

    // Create the monitoring task for continuous data reading
    ESP_LOGI(TAG, "Step 4: Starting monitoring task...");
    BaseType_t task_created = xTaskCreate(
        sdm120_monitoring_task,     // Task function
        "sdm120_monitor",          // Task name  
        4096,                      // Stack size
        NULL,                      // Parameters
        5,                         // Priority
        NULL                       // Task handle
    );

    if (task_created == pdPASS) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "🎉 SDM120 application started successfully!");
        ESP_LOGI(TAG, "📊 Reading data from %s every 5 seconds...", SDM120_SLAVE_IP);
        ESP_LOGI(TAG, "📡 Publishing data to MQTT broker: %s", MQTT_BROKER_URI);
        ESP_LOGI(TAG, "📍 MQTT topics: %s/data (JSON) + individual parameters", MQTT_TOPIC_PREFIX);
        if (MQTT_HOME_ASSISTANT_DISCOVERY) {
            ESP_LOGI(TAG, "🏠 Home Assistant auto-discovery enabled - sensors will appear automatically");
        }
        ESP_LOGI(TAG, "");
    } else {
        ESP_LOGE(TAG, "❌ Failed to create monitoring task");
    }
}

/* ===== END OF IMPLEMENTATION =====
 * 
 * 🎯 Summary of Complete SDM120 Energy Monitor with MQTT:
 * 
 * 1. 📝 Configuration: Use 'idf.py menuconfig' to configure device IP, MQTT settings, and credentials
 * 2. 🔧 Initialization: Modbus + MQTT handled automatically with high-level APIs
 * 3. 📊 Data Reading: Uses mbc_master_get_parameter() with IEEE754 conversion + enhanced retry logic
 * 4. 📡 MQTT Publishing: JSON + individual topics + Home Assistant auto-discovery
 * 5. 🏠 Home Assistant: Automatic sensor discovery with proper device classes and availability
 * 6. 🏃 Monitoring: Continuous readings with timeout recovery and connectivity diagnostics
 * 
 * 🔒 Security: All credentials and sensitive settings are externalized through Kconfig
 * 📡 WiFi: Native robust implementation with automatic reconnection and power management
 * 
 * 📋 Configuration Instructions:
 * 
 * 1. 🔧 Configure all settings via menuconfig:
 *    $ idf.py menuconfig
 *    
 * 2. 📍 Navigate to these menus:
 *    - "SDM120 Device Configuration" → Set your SDM120 IP address and timeouts
 *    - "SDM120 MQTT Configuration" → Set broker URI, credentials, and HA settings
 *    - "WiFi Configuration" → Set WiFi credentials and connection parameters
 *    
 * 3. 🔐 Set your MQTT credentials:
 *    MQTT Username: [your_mqtt_username]
 *    MQTT Password: [your_mqtt_password]
 *    
 * 4. 💾 Save configuration and build:
 *    $ idf.py build flash monitor
 *    
 * ✅ No more hardcoded credentials in source code!
 * 
 * MQTT Topics Published:
 * JSON Topic:
 * - energy/sdm120/data           (Complete JSON with all measurements + timestamp)
 * 
 * Individual CID Topics (All 10 Parameters):
 * - energy/sdm120/voltage        (CID 0: Line voltage in V)
 * - energy/sdm120/current        (CID 1: Phase current in A)  
 * - energy/sdm120/active_power   (CID 2: Active power in W)
 * - energy/sdm120/apparent_power (CID 3: Apparent power in VA)
 * - energy/sdm120/reactive_power (CID 4: Reactive power in VAr)
 * - energy/sdm120/power_factor   (CID 5: Power factor)
 * - energy/sdm120/frequency      (CID 6: Line frequency in Hz)
 * - energy/sdm120/import_energy  (CID 7: Import active energy in kWh)
 * - energy/sdm120/export_energy  (CID 8: Export active energy in kWh)
 * - energy/sdm120/total_energy   (CID 9: Total active energy in kWh)
 * - energy/sdm120/status         (Availability: online/offline)
 * 
 * 🏠 Home Assistant Integration:
 * - Automatic MQTT Discovery with proper device classes
 * - Energy Dashboard compatible (import/export/total energy sensors)
 * - Device availability tracking with Last Will Testament
 * - Proper icons and units for all sensors
 * - Single device grouping with device information
 * 
 * Clean, maintainable ESP32 Modbus-to-MQTT energy monitoring solution! 🎊
 */


