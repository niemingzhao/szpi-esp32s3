/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Services - BLE HID over GATT 服务本体（属性表、报告收发、协议模式）
 *
 * 移植自 ESP-IDF 官方例程 ble_hid_device_demo 的 hid_device_le_prf.c。命名与接口已
 * 改写为本项目约定（svc_bt_hid_* / SVC_BT_HID_*），行内注释保留英文原文以便与上游
 * 比对，HID_* 是 HID 规范里的名字，保持原样。
 * 主要改动：GATTS 回调由 svc_bt 统一注册后转发到本文件，事件分发只认 HID 的 app_id。
 */

#include "svc_bt_hid_dev.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "svc.bt.hid";

/// characteristic presentation information
struct prf_char_pres_fmt
{
    /// Unit (The Unit is a UUID)
    uint16_t unit;
    /// Description
    uint16_t description;
    /// Format
    uint8_t format;
    /// Exponent
    uint8_t exponent;
    /// Name space
    uint8_t name_space;
};

// HID report mapping table
static svc_bt_hid_report_map_t svc_bt_hid_rpt_map[SVC_BT_HID_NUM_REPORTS];

// HID Report Map characteristic value
// Keyboard report descriptor (using format for Boot interface descriptor)
static const uint8_t s_report_map[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x02,  // Usage (Mouse)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x01,  // Report Id (1)
    0x09, 0x01,  //   Usage (Pointer)
    0xA1, 0x00,  //   Collection (Physical)
    0x05, 0x09,  //     Usage Page (Buttons)
    0x19, 0x01,  //     Usage Minimum (01) - Button 1
    0x29, 0x03,  //     Usage Maximum (03) - Button 3
    0x15, 0x00,  //     Logical Minimum (0)
    0x25, 0x01,  //     Logical Maximum (1)
    0x75, 0x01,  //     Report Size (1)
    0x95, 0x03,  //     Report Count (3)
    0x81, 0x02,  //     Input (Data, Variable, Absolute) - Button states
    0x75, 0x05,  //     Report Size (5)
    0x95, 0x01,  //     Report Count (1)
    0x81, 0x01,  //     Input (Constant) - Padding or Reserved bits
    0x05, 0x01,  //     Usage Page (Generic Desktop)
    0x09, 0x30,  //     Usage (X)
    0x09, 0x31,  //     Usage (Y)
    0x09, 0x38,  //     Usage (Wheel)
    0x15, 0x81,  //     Logical Minimum (-127)
    0x25, 0x7F,  //     Logical Maximum (127)
    0x75, 0x08,  //     Report Size (8)
    0x95, 0x03,  //     Report Count (3)
    0x81, 0x06,  //     Input (Data, Variable, Relative) - X & Y coordinate
    0xC0,        //   End Collection
    0xC0,        // End Collection

    0x05, 0x01,  // Usage Pg (Generic Desktop)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection: (Application)
    0x85, 0x02,  // Report Id (2)
    //
    0x05, 0x07,  //   Usage Pg (Key Codes)
    0x19, 0xE0,  //   Usage Min (224)
    0x29, 0xE7,  //   Usage Max (231)
    0x15, 0x00,  //   Log Min (0)
    0x25, 0x01,  //   Log Max (1)
    //
    //   Modifier byte
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)
    0x81, 0x02,  //   Input: (Data, Variable, Absolute)
    //
    //   Reserved byte
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x01,  //   Input: (Constant)
    //
    //   LED report
    0x05, 0x08,  //   Usage Pg (LEDs)
    0x19, 0x01,  //   Usage Min (1)
    0x29, 0x05,  //   Usage Max (5)
    0x95, 0x05,  //   Report Count (5)
    0x75, 0x01,  //   Report Size (1)
    0x91, 0x02,  //   Output: (Data, Variable, Absolute)
    //
    //   LED report padding
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x03,  //   Report Size (3)
    0x91, 0x01,  //   Output: (Constant)
    //
    //   Key arrays (6 bytes)
    0x95, 0x06,  //   Report Count (6)
    0x75, 0x08,  //   Report Size (8)
    0x15, 0x00,  //   Log Min (0)
    0x25, 0x65,  //   Log Max (101)
    0x05, 0x07,  //   Usage Pg (Key Codes)
    0x19, 0x00,  //   Usage Min (0)
    0x29, 0x65,  //   Usage Max (101)
    0x81, 0x00,  //   Input: (Data, Array)
    //
    0xC0,        // End Collection
    //
    0x05, 0x0C,   // Usage Pg (Consumer Devices)
    0x09, 0x01,   // Usage (Consumer Control)
    0xA1, 0x01,   // Collection (Application)
    0x85, 0x03,   // Report Id (3)
    0x09, 0x02,   //   Usage (Numeric Key Pad)
    0xA1, 0x02,   //   Collection (Logical)
    0x05, 0x09,   //     Usage Pg (Button)
    0x19, 0x01,   //     Usage Min (Button 1)
    0x29, 0x0A,   //     Usage Max (Button 10)
    0x15, 0x01,   //     Logical Min (1)
    0x25, 0x0A,   //     Logical Max (10)
    0x75, 0x04,   //     Report Size (4)
    0x95, 0x01,   //     Report Count (1)
    0x81, 0x00,   //     Input (Data, Ary, Abs)
    0xC0,         //   End Collection
    0x05, 0x0C,   //   Usage Pg (Consumer Devices)
    0x09, 0x86,   //   Usage (Channel)
    0x15, 0xFF,   //   Logical Min (-1)
    0x25, 0x01,   //   Logical Max (1)
    0x75, 0x02,   //   Report Size (2)
    0x95, 0x01,   //   Report Count (1)
    0x81, 0x46,   //   Input (Data, Var, Rel, Null)
    0x09, 0xE9,   //   Usage (Volume Up)
    0x09, 0xEA,   //   Usage (Volume Down)
    0x15, 0x00,   //   Logical Min (0)
    0x75, 0x01,   //   Report Size (1)
    0x95, 0x02,   //   Report Count (2)
    0x81, 0x02,   //   Input (Data, Var, Abs)
    0x09, 0xE2,   //   Usage (Mute)
    0x09, 0x30,   //   Usage (Power)
    0x09, 0x83,   //   Usage (Recall Last)
    0x09, 0x81,   //   Usage (Assign Selection)
    0x09, 0xB0,   //   Usage (Play)
    0x09, 0xB1,   //   Usage (Pause)
    0x09, 0xB2,   //   Usage (Record)
    0x09, 0xB3,   //   Usage (Fast Forward)
    0x09, 0xB4,   //   Usage (Rewind)
    0x09, 0xB5,   //   Usage (Scan Next)
    0x09, 0xB6,   //   Usage (Scan Prev)
    0x09, 0xB7,   //   Usage (Stop)
    0x15, 0x01,   //   Logical Min (1)
    0x25, 0x0C,   //   Logical Max (12)
    0x75, 0x04,   //   Report Size (4)
    0x95, 0x01,   //   Report Count (1)
    0x81, 0x00,   //   Input (Data, Ary, Abs)
    0x09, 0x80,   //   Usage (Selection)
    0xA1, 0x02,   //   Collection (Logical)
    0x05, 0x09,   //     Usage Pg (Button)
    0x19, 0x01,   //     Usage Min (Button 1)
    0x29, 0x03,   //     Usage Max (Button 3)
    0x15, 0x01,   //     Logical Min (1)
    0x25, 0x03,   //     Logical Max (3)
    0x75, 0x02,   //     Report Size (2)
    0x81, 0x00,   //     Input (Data, Ary, Abs)
    0xC0,           //   End Collection
    0x81, 0x03,   //   Input (Const, Var, Abs)
    0xC0,            // End Collectionq

#if (SVC_BT_HID_SUPPORT_VENDOR_REPORT == true)
    0x06, 0xFF, 0xFF, // Usage Page(Vendor defined)
    0x09, 0xA5,       // Usage(Vendor Defined)
    0xA1, 0x01,       // Collection(Application)
    0x85, 0x04,   // Report Id (4)
    0x09, 0xA6,   // Usage(Vendor defined)
    0x09, 0xA9,   // Usage(Vendor defined)
    0x75, 0x08,   // Report Size
    0x95, 0x7F,   // Report Count = 127 Btyes
    0x91, 0x02,   // Output(Data, Variable, Absolute)
    0xC0,         // End Collection
#endif

};

/// Battery Service Attributes Indexes
enum
{
    BAS_IDX_SVC,

    BAS_IDX_BATT_LVL_CHAR,
    BAS_IDX_BATT_LVL_VAL,
    BAS_IDX_BATT_LVL_NTF_CFG,
    BAS_IDX_BATT_LVL_PRES_FMT,

    BAS_IDX_NB,
};

#define SVC_BT_HID_HI_UINT16(a) (((a) >> 8) & 0xFF)
#define SVC_BT_HID_LO_UINT16(a) ((a) & 0xFF)
#define SVC_BT_HID_PROFILE_NUM            1
#define SVC_BT_HID_PROFILE_APP_IDX        0

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
};

svc_bt_hid_dev_env_t svc_bt_hid_dev_env;

// HID report map length
uint8_t s_report_map_len = sizeof(s_report_map);
uint8_t protocol_mode = HID_PROTOCOL_MODE_REPORT;

// HID report mapping table
//static svc_bt_hid_rpt_map_t  svc_bt_hid_rpt_map[SVC_BT_HID_NUM_REPORTS];

// HID Information characteristic value
static const uint8_t info[HID_INFORMATION_LEN] = {
    SVC_BT_HID_LO_UINT16(0x0111), SVC_BT_HID_HI_UINT16(0x0111),             // bcd_hid (USB HID version)
    0x00,                                             // b_country_code
    HID_KBD_FLAGS                                     // Flags
};


// HID External Report Reference Descriptor
static uint16_t s_ext_report_ref_desc = ESP_GATT_UUID_BATTERY_LEVEL;

// HID Report Reference characteristic descriptor, mouse input
static uint8_t s_report_ref_mouse_in[HID_REPORT_REF_LEN] =
             { HID_RPT_ID_MOUSE_IN, HID_REPORT_TYPE_INPUT };


// HID Report Reference characteristic descriptor, key input
static uint8_t s_report_ref_key_in[HID_REPORT_REF_LEN] =
             { HID_RPT_ID_KEY_IN, HID_REPORT_TYPE_INPUT };

// HID Report Reference characteristic descriptor, LED output
static uint8_t s_report_ref_led_out[HID_REPORT_REF_LEN] =
             { HID_RPT_ID_LED_OUT, HID_REPORT_TYPE_OUTPUT };

#if (SVC_BT_HID_SUPPORT_VENDOR_REPORT  == true)

static uint8_t s_report_ref_vendor_out[HID_REPORT_REF_LEN] =
             {HID_RPT_ID_VENDOR_OUT, HID_REPORT_TYPE_OUTPUT};
#endif

// HID Report Reference characteristic descriptor, Feature
static uint8_t s_report_ref_feature[HID_REPORT_REF_LEN] =
             { HID_RPT_ID_FEATURE, HID_REPORT_TYPE_FEATURE };

// HID Report Reference characteristic descriptor, consumer control input
static uint8_t s_report_ref_cc_in[HID_REPORT_REF_LEN] =
             { HID_RPT_ID_CC_IN, HID_REPORT_TYPE_INPUT };


/*
 *  Heart Rate PROFILE ATTRIBUTES
 ****************************************************************************************
 */

/// hid Service uuid
static uint16_t svc_bt_hid_le_svc = ATT_SVC_HID;
uint16_t            svc_bt_hid_count = 0;
esp_gatts_incl_svc_desc_t incl_svc = {0};

#define SVC_BT_HID_CHAR_DECL_SIZE   (sizeof(uint8_t))
///the uuid definition
static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t include_service_uuid = ESP_GATT_UUID_INCLUDE_SERVICE;
static const uint16_t character_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint16_t svc_bt_hid_info_char_uuid = ESP_GATT_UUID_HID_INFORMATION;
static const uint16_t svc_bt_hid_report_map_uuid    = ESP_GATT_UUID_HID_REPORT_MAP;
static const uint16_t svc_bt_hid_control_point_uuid = ESP_GATT_UUID_HID_CONTROL_POINT;
static const uint16_t svc_bt_hid_report_uuid = ESP_GATT_UUID_HID_REPORT;
static const uint16_t svc_bt_hid_proto_mode_uuid = ESP_GATT_UUID_HID_PROTO_MODE;
static const uint16_t svc_bt_hid_kb_input_uuid = ESP_GATT_UUID_HID_BT_KB_INPUT;
static const uint16_t svc_bt_hid_kb_output_uuid = ESP_GATT_UUID_HID_BT_KB_OUTPUT;
static const uint16_t svc_bt_hid_mouse_input_uuid = ESP_GATT_UUID_HID_BT_MOUSE_INPUT;
static const uint16_t svc_bt_hid_repot_map_ext_desc_uuid = ESP_GATT_UUID_EXT_RPT_REF_DESCR;
static const uint16_t svc_bt_hid_report_ref_descr_uuid = ESP_GATT_UUID_RPT_REF_DESCR;
///the propoty definition
static const uint8_t char_prop_notify = ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t char_prop_read = ESP_GATT_CHAR_PROP_BIT_READ;
static const uint8_t char_prop_write_nr = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t char_prop_read_write = ESP_GATT_CHAR_PROP_BIT_WRITE|ESP_GATT_CHAR_PROP_BIT_READ;
static const uint8_t char_prop_read_notify = ESP_GATT_CHAR_PROP_BIT_READ|ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t char_prop_read_write_notify = ESP_GATT_CHAR_PROP_BIT_READ|ESP_GATT_CHAR_PROP_BIT_WRITE|ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t char_prop_read_write_write_nr = ESP_GATT_CHAR_PROP_BIT_READ|ESP_GATT_CHAR_PROP_BIT_WRITE|ESP_GATT_CHAR_PROP_BIT_WRITE_NR;

/// battary Service
static const uint16_t battary_svc = ESP_GATT_UUID_BATTERY_SERVICE_SVC;

static const uint16_t bat_lev_uuid = ESP_GATT_UUID_BATTERY_LEVEL;
static const uint8_t   bat_lev_ccc[2] ={ 0x00, 0x00};
static const uint16_t char_format_uuid = ESP_GATT_UUID_CHAR_PRESENT_FORMAT;

static uint8_t battary_lev = 50;
/// Full HRS Database Description - Used to add attributes into the database
static const esp_gatts_attr_db_t bas_att_db[BAS_IDX_NB] =
{
    // Battary Service Declaration
    [BAS_IDX_SVC]               =  {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
                                            sizeof(uint16_t), sizeof(battary_svc), (uint8_t *)&battary_svc}},

    // Battary level Characteristic Declaration
    [BAS_IDX_BATT_LVL_CHAR]    = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
                                                   SVC_BT_HID_CHAR_DECL_SIZE,SVC_BT_HID_CHAR_DECL_SIZE, (uint8_t *)&char_prop_read_notify}},

    // Battary level Characteristic Value
    [BAS_IDX_BATT_LVL_VAL]             	= {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&bat_lev_uuid, ESP_GATT_PERM_READ,
                                                                sizeof(uint8_t),sizeof(uint8_t), &battary_lev}},

    // Battary level Characteristic - Client Characteristic Configuration Descriptor
    [BAS_IDX_BATT_LVL_NTF_CFG]     	=  {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid, ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE,
                                                          sizeof(uint16_t),sizeof(bat_lev_ccc), (uint8_t *)bat_lev_ccc}},

    // Battary level report Characteristic Declaration
    [BAS_IDX_BATT_LVL_PRES_FMT]  = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&char_format_uuid, ESP_GATT_PERM_READ,
                                                        sizeof(struct prf_char_pres_fmt), 0, NULL}},
};


/// Full Hid device Database Description - Used to add attributes into the database
static esp_gatts_attr_db_t svc_bt_hid_dev_gatt_db[SVC_BT_HID_LE_IDX_NB] =
{
            // HID Service Declaration
    [SVC_BT_HID_LE_IDX_SVC]                       = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid,
                                                             ESP_GATT_PERM_READ_ENCRYPTED, sizeof(uint16_t), sizeof(svc_bt_hid_le_svc),
                                                            (uint8_t *)&svc_bt_hid_le_svc}},

    // HID Service Declaration
    [SVC_BT_HID_LE_IDX_INCL_SVC]               = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&include_service_uuid,
                                                            ESP_GATT_PERM_READ,
                                                            sizeof(esp_gatts_incl_svc_desc_t), sizeof(esp_gatts_incl_svc_desc_t),
                                                            (uint8_t *)&incl_svc}},

    // HID Information Characteristic Declaration
    [SVC_BT_HID_LE_IDX_HID_INFO_CHAR]     = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
                                                            ESP_GATT_PERM_READ,
                                                            SVC_BT_HID_CHAR_DECL_SIZE, SVC_BT_HID_CHAR_DECL_SIZE,
                                                            (uint8_t *)&char_prop_read}},
    // HID Information Characteristic Value
    [SVC_BT_HID_LE_IDX_HID_INFO_VAL]       = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_info_char_uuid,
                                                            ESP_GATT_PERM_READ,
                                                            sizeof(svc_bt_hid_info_t), sizeof(info),
                                                            (uint8_t *)&info}},

    // HID Control Point Characteristic Declaration
    [SVC_BT_HID_LE_IDX_HID_CTNL_PT_CHAR]  = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
                                                              ESP_GATT_PERM_READ,
                                                              SVC_BT_HID_CHAR_DECL_SIZE, SVC_BT_HID_CHAR_DECL_SIZE,
                                                              (uint8_t *)&char_prop_write_nr}},
    // HID Control Point Characteristic Value
    [SVC_BT_HID_LE_IDX_HID_CTNL_PT_VAL]    = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_control_point_uuid,
                                                             ESP_GATT_PERM_WRITE,
                                                             sizeof(uint8_t), 0,
                                                             NULL}},

    // Report Map Characteristic Declaration
    [SVC_BT_HID_LE_IDX_REPORT_MAP_CHAR]   = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
                                                              ESP_GATT_PERM_READ,
                                                              SVC_BT_HID_CHAR_DECL_SIZE, SVC_BT_HID_CHAR_DECL_SIZE,
                                                              (uint8_t *)&char_prop_read}},
    // Report Map Characteristic Value
    [SVC_BT_HID_LE_IDX_REPORT_MAP_VAL]     = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_report_map_uuid,
                                                              ESP_GATT_PERM_READ,
                                                              SVC_BT_HID_LE_REPORT_MAP_MAX_LEN, sizeof(s_report_map),
                                                              (uint8_t *)&s_report_map}},

    // Report Map Characteristic - External Report Reference Descriptor
    [SVC_BT_HID_LE_IDX_REPORT_MAP_EXT_REP_REF]  = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_repot_map_ext_desc_uuid,
                                                                        ESP_GATT_PERM_READ,
                                                                        sizeof(uint16_t), sizeof(uint16_t),
                                                                        (uint8_t *)&s_ext_report_ref_desc}},

    // Protocol Mode Characteristic Declaration
    [SVC_BT_HID_LE_IDX_PROTO_MODE_CHAR]            = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
                                                                        ESP_GATT_PERM_READ,
                                                                        SVC_BT_HID_CHAR_DECL_SIZE, SVC_BT_HID_CHAR_DECL_SIZE,
                                                                        (uint8_t *)&char_prop_read_write}},
    // Protocol Mode Characteristic Value
    [SVC_BT_HID_LE_IDX_PROTO_MODE_VAL]               = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_proto_mode_uuid,
                                                                        (ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE),
                                                                        sizeof(uint8_t), sizeof(protocol_mode),
                                                                        (uint8_t *)&protocol_mode}},

    [SVC_BT_HID_LE_IDX_REPORT_MOUSE_IN_CHAR]       = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
                                                                         ESP_GATT_PERM_READ,
                                                                         SVC_BT_HID_CHAR_DECL_SIZE, SVC_BT_HID_CHAR_DECL_SIZE,
                                                                         (uint8_t *)&char_prop_read_notify}},

    [SVC_BT_HID_LE_IDX_REPORT_MOUSE_IN_VAL]        = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_report_uuid,
                                                                       ESP_GATT_PERM_READ,
                                                                       SVC_BT_HID_LE_REPORT_MAX_LEN, 0,
                                                                       NULL}},

    [SVC_BT_HID_LE_IDX_REPORT_MOUSE_IN_CCC]        = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid,
                                                                      (ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE),
                                                                      sizeof(uint16_t), 0,
                                                                      NULL}},

    [SVC_BT_HID_LE_IDX_REPORT_MOUSE_REP_REF]       = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_report_ref_descr_uuid,
                                                                       ESP_GATT_PERM_READ,
                                                                       sizeof(s_report_ref_mouse_in), sizeof(s_report_ref_mouse_in),
                                                                       s_report_ref_mouse_in}},
    // Report Characteristic Declaration
    [SVC_BT_HID_LE_IDX_REPORT_KEY_IN_CHAR]         = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
                                                                         ESP_GATT_PERM_READ,
                                                                         SVC_BT_HID_CHAR_DECL_SIZE, SVC_BT_HID_CHAR_DECL_SIZE,
                                                                         (uint8_t *)&char_prop_read_notify}},
    // Report Characteristic Value
    [SVC_BT_HID_LE_IDX_REPORT_KEY_IN_VAL]            = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_report_uuid,
                                                                       ESP_GATT_PERM_READ,
                                                                       SVC_BT_HID_LE_REPORT_MAX_LEN, 0,
                                                                       NULL}},
    // Report KEY INPUT Characteristic - Client Characteristic Configuration Descriptor
    [SVC_BT_HID_LE_IDX_REPORT_KEY_IN_CCC]              = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid,
                                                                      (ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE),
                                                                      sizeof(uint16_t), 0,
                                                                      NULL}},
     // Report Characteristic - Report Reference Descriptor
    [SVC_BT_HID_LE_IDX_REPORT_KEY_IN_REP_REF]       = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_report_ref_descr_uuid,
                                                                       ESP_GATT_PERM_READ,
                                                                       sizeof(s_report_ref_key_in), sizeof(s_report_ref_key_in),
                                                                       s_report_ref_key_in}},

     // Report Characteristic Declaration
    [SVC_BT_HID_LE_IDX_REPORT_LED_OUT_CHAR]         = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
                                                                         ESP_GATT_PERM_READ,
                                                                         SVC_BT_HID_CHAR_DECL_SIZE, SVC_BT_HID_CHAR_DECL_SIZE,
                                                                         (uint8_t *)&char_prop_read_write_write_nr}},

    [SVC_BT_HID_LE_IDX_REPORT_LED_OUT_VAL]            = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_report_uuid,
                                                                       ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE,
                                                                       SVC_BT_HID_LE_REPORT_MAX_LEN, 0,
                                                                       NULL}},
    [SVC_BT_HID_LE_IDX_REPORT_LED_OUT_REP_REF]      =  {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_report_ref_descr_uuid,
                                                                       ESP_GATT_PERM_READ,
                                                                       sizeof(s_report_ref_led_out), sizeof(s_report_ref_led_out),
                                                                       s_report_ref_led_out}},
#if (SVC_BT_HID_SUPPORT_VENDOR_REPORT  == true)
    // Report Characteristic Declaration
    [SVC_BT_HID_LE_IDX_REPORT_VENDOR_OUT_CHAR]        = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
                                                                         ESP_GATT_PERM_READ,
                                                                         SVC_BT_HID_CHAR_DECL_SIZE, SVC_BT_HID_CHAR_DECL_SIZE,
                                                                         (uint8_t *)&char_prop_read_write_notify}},
    [SVC_BT_HID_LE_IDX_REPORT_VENDOR_OUT_VAL]         = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_report_uuid,
                                                                       ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE,
                                                                       SVC_BT_HID_LE_REPORT_MAX_LEN, 0,
                                                                       NULL}},
    [SVC_BT_HID_LE_IDX_REPORT_VENDOR_OUT_REP_REF]     = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_report_ref_descr_uuid,
                                                                       ESP_GATT_PERM_READ,
                                                                       sizeof(s_report_ref_vendor_out), sizeof(s_report_ref_vendor_out),
                                                                       s_report_ref_vendor_out}},
#endif
    // Report Characteristic Declaration
    [SVC_BT_HID_LE_IDX_REPORT_CC_IN_CHAR]         = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
                                                                         ESP_GATT_PERM_READ,
                                                                         SVC_BT_HID_CHAR_DECL_SIZE, SVC_BT_HID_CHAR_DECL_SIZE,
                                                                         (uint8_t *)&char_prop_read_notify}},
    // Report Characteristic Value
    [SVC_BT_HID_LE_IDX_REPORT_CC_IN_VAL]            = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_report_uuid,
                                                                       ESP_GATT_PERM_READ,
                                                                       SVC_BT_HID_LE_REPORT_MAX_LEN, 0,
                                                                       NULL}},
    // Report KEY INPUT Characteristic - Client Characteristic Configuration Descriptor
    [SVC_BT_HID_LE_IDX_REPORT_CC_IN_CCC]              = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid,
                                                                      (ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE_ENCRYPTED),
                                                                      sizeof(uint16_t), 0,
                                                                      NULL}},
     // Report Characteristic - Report Reference Descriptor
    [SVC_BT_HID_LE_IDX_REPORT_CC_IN_REP_REF]       = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_report_ref_descr_uuid,
                                                                       ESP_GATT_PERM_READ,
                                                                       sizeof(s_report_ref_cc_in), sizeof(s_report_ref_cc_in),
                                                                       s_report_ref_cc_in}},

    // Boot Keyboard Input Report Characteristic Declaration
    [SVC_BT_HID_LE_IDX_BOOT_KB_IN_REPORT_CHAR] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
                                                                        ESP_GATT_PERM_READ,
                                                                        SVC_BT_HID_CHAR_DECL_SIZE, SVC_BT_HID_CHAR_DECL_SIZE,
                                                                        (uint8_t *)&char_prop_read_notify}},
    // Boot Keyboard Input Report Characteristic Value
    [SVC_BT_HID_LE_IDX_BOOT_KB_IN_REPORT_VAL]   = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_kb_input_uuid,
                                                                        ESP_GATT_PERM_READ,
                                                                        SVC_BT_HID_LE_BOOT_REPORT_MAX_LEN, 0,
                                                                        NULL}},
    // Boot Keyboard Input Report Characteristic - Client Characteristic Configuration Descriptor
    [SVC_BT_HID_LE_IDX_BOOT_KB_IN_REPORT_NTF_CFG]  = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid,
                                                                              (ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE),
                                                                              sizeof(uint16_t), 0,
                                                                              NULL}},

    // Boot Keyboard Output Report Characteristic Declaration
    [SVC_BT_HID_LE_IDX_BOOT_KB_OUT_REPORT_CHAR]    = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
                                                                              ESP_GATT_PERM_READ,
                                                                              SVC_BT_HID_CHAR_DECL_SIZE, SVC_BT_HID_CHAR_DECL_SIZE,
                                                                              (uint8_t *)&char_prop_read_write}},
    // Boot Keyboard Output Report Characteristic Value
    [SVC_BT_HID_LE_IDX_BOOT_KB_OUT_REPORT_VAL]      = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_kb_output_uuid,
                                                                              (ESP_GATT_PERM_READ|ESP_GATT_PERM_WRITE),
                                                                              SVC_BT_HID_LE_BOOT_REPORT_MAX_LEN, 0,
                                                                              NULL}},

    // Boot Mouse Input Report Characteristic Declaration
    [SVC_BT_HID_LE_IDX_BOOT_MOUSE_IN_REPORT_CHAR] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
                                                                              ESP_GATT_PERM_READ,
                                                                              SVC_BT_HID_CHAR_DECL_SIZE, SVC_BT_HID_CHAR_DECL_SIZE,
                                                                              (uint8_t *)&char_prop_read_notify}},
    // Boot Mouse Input Report Characteristic Value
    [SVC_BT_HID_LE_IDX_BOOT_MOUSE_IN_REPORT_VAL]   = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_mouse_input_uuid,
                                                                              ESP_GATT_PERM_READ,
                                                                              SVC_BT_HID_LE_BOOT_REPORT_MAX_LEN, 0,
                                                                              NULL}},
    // Boot Mouse Input Report Characteristic - Client Characteristic Configuration Descriptor
    [SVC_BT_HID_LE_IDX_BOOT_MOUSE_IN_REPORT_NTF_CFG]    = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid,
                                                                                      (ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE),
                                                                                      sizeof(uint16_t), 0,
                                                                                      NULL}},

    // Report Characteristic Declaration
    [SVC_BT_HID_LE_IDX_REPORT_CHAR]                    = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
                                                                         ESP_GATT_PERM_READ,
                                                                         SVC_BT_HID_CHAR_DECL_SIZE, SVC_BT_HID_CHAR_DECL_SIZE,
                                                                         (uint8_t *)&char_prop_read_write}},
    // Report Characteristic Value
    [SVC_BT_HID_LE_IDX_REPORT_VAL]                      = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_report_uuid,
                                                                       ESP_GATT_PERM_READ,
                                                                       SVC_BT_HID_LE_REPORT_MAX_LEN, 0,
                                                                       NULL}},
    // Report Characteristic - Report Reference Descriptor
    [SVC_BT_HID_LE_IDX_REPORT_REP_REF]               = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&svc_bt_hid_report_ref_descr_uuid,
                                                                       ESP_GATT_PERM_READ,
                                                                       sizeof(s_report_ref_feature), sizeof(s_report_ref_feature),
                                                                       s_report_ref_feature}},
};

static void svc_bt_hid_add_id_tbl(void);

void svc_bt_hid_dev_cb_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
									esp_ble_gatts_cb_param_t *param)
{
    switch(event) {
        case ESP_GATTS_REG_EVT: {
            esp_ble_gap_config_local_icon (ESP_BLE_APPEARANCE_GENERIC_HID);
            svc_bt_hid_evt_param_t svc_bt_hid_dev_param;
            svc_bt_hid_dev_param.init_finish.state = param->reg.status;
            if(param->reg.app_id == SVC_BT_HID_APP_ID) {
                svc_bt_hid_dev_env.gatt_if = gatts_if;
                if(svc_bt_hid_dev_env.svc_bt_hid_dev_cb != NULL) {
                    (svc_bt_hid_dev_env.svc_bt_hid_dev_cb)(SVC_BT_HID_EVENT_REG_FINISH, &svc_bt_hid_dev_param);
                    svc_bt_hid_dev_create_service(svc_bt_hid_dev_env.gatt_if);
                }
            }
            if(param->reg.app_id == SVC_BT_HID_BATTERY_APP_ID) {
                svc_bt_hid_dev_param.init_finish.gatts_if = gatts_if;
                 if(svc_bt_hid_dev_env.svc_bt_hid_dev_cb != NULL) {
                    (svc_bt_hid_dev_env.svc_bt_hid_dev_cb)(SVC_BT_HID_BATTERY_EVENT_REG, &svc_bt_hid_dev_param);
                }

            }

            break;
        }
        case ESP_GATTS_CONF_EVT: {
            break;
        }
        case ESP_GATTS_CREATE_EVT:
            break;
        case ESP_GATTS_CONNECT_EVT: {
            svc_bt_hid_evt_param_t cb_param = {0};
			ESP_LOGI(TAG, "HID connection establish, conn_id = %x",param->connect.conn_id);
			memcpy(cb_param.connect.remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            cb_param.connect.conn_id = param->connect.conn_id;
            svc_bt_hid_dev_clcb_alloc(param->connect.conn_id, param->connect.remote_bda);
            esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_NO_MITM);
            if(svc_bt_hid_dev_env.svc_bt_hid_dev_cb != NULL) {
                (svc_bt_hid_dev_env.svc_bt_hid_dev_cb)(SVC_BT_HID_EVENT_BLE_CONNECT, &cb_param);
            }
            break;
        }
        case ESP_GATTS_DISCONNECT_EVT: {
			 if(svc_bt_hid_dev_env.svc_bt_hid_dev_cb != NULL) {
                    (svc_bt_hid_dev_env.svc_bt_hid_dev_cb)(SVC_BT_HID_EVENT_BLE_DISCONNECT, NULL);
             }
            svc_bt_hid_dev_clcb_dealloc(param->disconnect.conn_id);
            break;
        }
        case ESP_GATTS_CLOSE_EVT:
            break;
        case ESP_GATTS_WRITE_EVT: {
            svc_bt_hid_evt_param_t cb_param = {0};
            if (param->write.handle == svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_REPORT_LED_OUT_VAL]) {
                cb_param.led_write.conn_id = param->write.conn_id;
                cb_param.led_write.report_id = HID_RPT_ID_LED_OUT;
                cb_param.led_write.length = param->write.len;
                cb_param.led_write.data = param->write.value;
                (svc_bt_hid_dev_env.svc_bt_hid_dev_cb)(SVC_BT_HID_EVENT_BLE_LED_REPORT_WRITE_EVT, &cb_param);
            }
#if (SVC_BT_HID_SUPPORT_VENDOR_REPORT == true)
            if (param->write.handle == svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_REPORT_VENDOR_OUT_VAL] &&
                svc_bt_hid_dev_env.svc_bt_hid_dev_cb != NULL) {
                cb_param.vendor_write.conn_id = param->write.conn_id;
                cb_param.vendor_write.report_id = HID_RPT_ID_VENDOR_OUT;
                cb_param.vendor_write.length = param->write.len;
                cb_param.vendor_write.data = param->write.value;
                (svc_bt_hid_dev_env.svc_bt_hid_dev_cb)(SVC_BT_HID_EVENT_BLE_VENDOR_REPORT_WRITE_EVT, &cb_param);
            }
#endif
            break;
        }
        case ESP_GATTS_CREAT_ATTR_TAB_EVT: {
            if (param->add_attr_tab.num_handle == BAS_IDX_NB &&
                param->add_attr_tab.svc_uuid.uuid.uuid16 == ESP_GATT_UUID_BATTERY_SERVICE_SVC &&
                param->add_attr_tab.status == ESP_GATT_OK) {
                incl_svc.start_hdl = param->add_attr_tab.handles[BAS_IDX_SVC];
                incl_svc.end_hdl = incl_svc.start_hdl + BAS_IDX_NB -1;
                ESP_LOGI(TAG, "%s(), start added the hid service to the stack database. incl_handle = %d",
                           __func__, incl_svc.start_hdl);
                esp_ble_gatts_create_attr_tab(svc_bt_hid_dev_gatt_db, gatts_if, SVC_BT_HID_LE_IDX_NB, 0);
            }
            if (param->add_attr_tab.num_handle == SVC_BT_HID_LE_IDX_NB &&
                param->add_attr_tab.status == ESP_GATT_OK) {
                memcpy(svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl, param->add_attr_tab.handles,
                            SVC_BT_HID_LE_IDX_NB*sizeof(uint16_t));
                ESP_LOGI(TAG, "hid svc handle = %x",svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_SVC]);
                svc_bt_hid_add_id_tbl();
		        esp_ble_gatts_start_service(svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_SVC]);
            } else {
                esp_ble_gatts_start_service(param->add_attr_tab.handles[0]);
            }
            break;
         }

        default:
            break;
    }
}

void svc_bt_hid_dev_create_service(esp_gatt_if_t gatts_if)
{
    /* Here should added the battery service first, because the hid service should include the battery service.
       After finish to added the battery service then can added the hid service. */
    esp_ble_gatts_create_attr_tab(bas_att_db, gatts_if, BAS_IDX_NB, 0);

}

void svc_bt_hid_dev_clcb_alloc (uint16_t conn_id, esp_bd_addr_t bda)
{
    uint8_t                   i_clcb = 0;
    svc_bt_hid_dev_clcb_t      *p_clcb = NULL;

    for (i_clcb = 0, p_clcb= svc_bt_hid_dev_env.svc_bt_hid_dev_clcb; i_clcb < SVC_BT_HID_MAX_APPS; i_clcb++, p_clcb++) {
        if (!p_clcb->in_use) {
            p_clcb->in_use      = true;
            p_clcb->conn_id     = conn_id;
            p_clcb->connected   = true;
            memcpy (p_clcb->remote_bda, bda, ESP_BD_ADDR_LEN);
            break;
        }
    }
    return;
}

bool svc_bt_hid_dev_clcb_dealloc (uint16_t conn_id)
{
    uint8_t              i_clcb = 0;
    svc_bt_hid_dev_clcb_t      *p_clcb = NULL;

    for (i_clcb = 0, p_clcb= svc_bt_hid_dev_env.svc_bt_hid_dev_clcb; i_clcb < SVC_BT_HID_MAX_APPS; i_clcb++, p_clcb++) {
            memset(p_clcb, 0, sizeof(svc_bt_hid_dev_clcb_t));
            return true;
    }

    return false;
}

static struct gatts_profile_inst s_profile_tab[SVC_BT_HID_PROFILE_NUM] = {
    [SVC_BT_HID_PROFILE_APP_IDX] = {
        .gatts_cb = svc_bt_hid_dev_cb_handler,
        .gatts_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    },

};

void svc_bt_hid_dev_gatts_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    /* If event is register event, store the gatts_if for each profile */
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.app_id != SVC_BT_HID_APP_ID) return;
        if (param->reg.status == ESP_GATT_OK) {
            s_profile_tab[SVC_BT_HID_PROFILE_APP_IDX].gatts_if = gatts_if;
        } else {
            ESP_LOGI(TAG, "Reg app failed, app_id %04x, status %d\n",
                    param->reg.app_id,
                    param->reg.status);
            return;
        }
    }

    do {
        int idx;
        for (idx = 0; idx < SVC_BT_HID_PROFILE_NUM; idx++) {
            if (gatts_if == ESP_GATT_IF_NONE || /* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function */
                    gatts_if == s_profile_tab[idx].gatts_if) {
                if (s_profile_tab[idx].gatts_cb) {
                    s_profile_tab[idx].gatts_cb(event, gatts_if, param);
                }
            }
        }
    } while (0);
}


esp_err_t svc_bt_hid_dev_register_cb(void)
{
	esp_err_t status;
	status = ESP_OK; /* SZPI-OS: the GATTS callback is registered by svc_bt */
	return status;
}

void svc_bt_hid_dev_set_attr_value(uint16_t handle, uint16_t val_len, const uint8_t *value)
{
    svc_bt_hid_dev_inst_t *svc_bt_hid_dev_inst = &svc_bt_hid_dev_env.svc_bt_hid_dev_inst;
    if(svc_bt_hid_dev_inst->att_tbl[SVC_BT_HID_LE_IDX_HID_INFO_VAL] <= handle &&
        svc_bt_hid_dev_inst->att_tbl[SVC_BT_HID_LE_IDX_REPORT_REP_REF] >= handle) {
        esp_ble_gatts_set_attr_value(handle, val_len, value);
    } else {
        ESP_LOGE(TAG, "%s error:Invalid handle value.",__func__);
    }
    return;
}

void svc_bt_hid_dev_get_attr_value(uint16_t handle, uint16_t *length, uint8_t **value)
{
    svc_bt_hid_dev_inst_t *svc_bt_hid_dev_inst = &svc_bt_hid_dev_env.svc_bt_hid_dev_inst;
    if(svc_bt_hid_dev_inst->att_tbl[SVC_BT_HID_LE_IDX_HID_INFO_VAL] <= handle &&
        svc_bt_hid_dev_inst->att_tbl[SVC_BT_HID_LE_IDX_REPORT_REP_REF] >= handle){
        esp_ble_gatts_get_attr_value(handle, length, (const uint8_t **)value);
    } else {
        ESP_LOGE(TAG, "%s error:Invalid handle value.", __func__);
    }

    return;
}

static void svc_bt_hid_add_id_tbl(void)
{
     // Mouse input report
      svc_bt_hid_rpt_map[0].id = s_report_ref_mouse_in[0];
      svc_bt_hid_rpt_map[0].type = s_report_ref_mouse_in[1];
      svc_bt_hid_rpt_map[0].handle = svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_REPORT_MOUSE_IN_VAL];
      svc_bt_hid_rpt_map[0].cccd_handle = svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_REPORT_MOUSE_IN_VAL];
      svc_bt_hid_rpt_map[0].mode = HID_PROTOCOL_MODE_REPORT;

      // Key input report
      svc_bt_hid_rpt_map[1].id = s_report_ref_key_in[0];
      svc_bt_hid_rpt_map[1].type = s_report_ref_key_in[1];
      svc_bt_hid_rpt_map[1].handle = svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_REPORT_KEY_IN_VAL];
      svc_bt_hid_rpt_map[1].cccd_handle = svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_REPORT_KEY_IN_CCC];
      svc_bt_hid_rpt_map[1].mode = HID_PROTOCOL_MODE_REPORT;

      // Consumer Control input report
      svc_bt_hid_rpt_map[2].id = s_report_ref_cc_in[0];
      svc_bt_hid_rpt_map[2].type = s_report_ref_cc_in[1];
      svc_bt_hid_rpt_map[2].handle = svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_REPORT_CC_IN_VAL];
      svc_bt_hid_rpt_map[2].cccd_handle = svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_REPORT_CC_IN_CCC];
      svc_bt_hid_rpt_map[2].mode = HID_PROTOCOL_MODE_REPORT;

      // LED output report
      svc_bt_hid_rpt_map[3].id = s_report_ref_led_out[0];
      svc_bt_hid_rpt_map[3].type = s_report_ref_led_out[1];
      svc_bt_hid_rpt_map[3].handle = svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_REPORT_LED_OUT_VAL];
      svc_bt_hid_rpt_map[3].cccd_handle = 0;
      svc_bt_hid_rpt_map[3].mode = HID_PROTOCOL_MODE_REPORT;

      // Boot keyboard input report
      // Use same ID and type as key input report
      svc_bt_hid_rpt_map[4].id = s_report_ref_key_in[0];
      svc_bt_hid_rpt_map[4].type = s_report_ref_key_in[1];
      svc_bt_hid_rpt_map[4].handle = svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_BOOT_KB_IN_REPORT_VAL];
      svc_bt_hid_rpt_map[4].cccd_handle = 0;
      svc_bt_hid_rpt_map[4].mode = HID_PROTOCOL_MODE_BOOT;

      // Boot keyboard output report
      // Use same ID and type as LED output report
      svc_bt_hid_rpt_map[5].id = s_report_ref_led_out[0];
      svc_bt_hid_rpt_map[5].type = s_report_ref_led_out[1];
      svc_bt_hid_rpt_map[5].handle = svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_BOOT_KB_OUT_REPORT_VAL];
      svc_bt_hid_rpt_map[5].cccd_handle = 0;
      svc_bt_hid_rpt_map[5].mode = HID_PROTOCOL_MODE_BOOT;

      // Boot mouse input report
      // Use same ID and type as mouse input report
      svc_bt_hid_rpt_map[6].id = s_report_ref_mouse_in[0];
      svc_bt_hid_rpt_map[6].type = s_report_ref_mouse_in[1];
      svc_bt_hid_rpt_map[6].handle = svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_BOOT_MOUSE_IN_REPORT_VAL];
      svc_bt_hid_rpt_map[6].cccd_handle = 0;
      svc_bt_hid_rpt_map[6].mode = HID_PROTOCOL_MODE_BOOT;

      // Feature report
      svc_bt_hid_rpt_map[7].id = s_report_ref_feature[0];
      svc_bt_hid_rpt_map[7].type = s_report_ref_feature[1];
      svc_bt_hid_rpt_map[7].handle = svc_bt_hid_dev_env.svc_bt_hid_dev_inst.att_tbl[SVC_BT_HID_LE_IDX_REPORT_VAL];
      svc_bt_hid_rpt_map[7].cccd_handle = 0;
      svc_bt_hid_rpt_map[7].mode = HID_PROTOCOL_MODE_REPORT;


  // Setup report ID map
  svc_bt_hid_dev_register_reports(SVC_BT_HID_NUM_REPORTS, svc_bt_hid_rpt_map);
}
