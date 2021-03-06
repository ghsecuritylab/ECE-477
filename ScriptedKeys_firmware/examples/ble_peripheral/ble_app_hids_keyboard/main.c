/*
Purdue University ECE477 Senior Design - ScriptedKeys
Fall 2018
This is the ScriptedKeys keyboard firmware's main.c
This firmware is coded based on nRF52 SDK ver.15.2 's HID keyboard example

*/

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "nordic_common.h"
#include "Fingerprint_Scanner-TTL-master/src/Scanner.h"
#include "nrf.h"
#include "nrf_assert.h"
#include "app_error.h"
#include "ble.h"
#include "ble_err.h"
#include "ble_hci.h"
#include "ble_srv_common.h"
#include "ble_advertising.h"
#include "ble_advdata.h"
#include "ble_hids.h"
#include "ble_bas.h"
#include "ble_dis.h"
#include "ble_nus.h"
#include "ble_link_ctx_manager.h"
#include "ble_conn_params.h"
#include "sensorsim.h"
#include "bsp.h"
#include "bsp_btn_ble.h"
#include "app_scheduler.h"
#include "nrf_sdh.h"
#include "nrf_sdh_soc.h"
#include "nrf_sdh_ble.h"
#include "app_timer.h"
#include "peer_manager.h"
#include "fds.h"
#include "ble_conn_state.h"
#include "nrf_ble_gatt.h"
#include "nrf_ble_qwr.h"
#include "nrf_pwr_mgmt.h"
#include "peer_manager_handler.h"
#include "app_uart.h"
#include "nrf_delay.h"
#include "bsp.h"
#include "nrf_drv_uart.h"
#if defined (UART_PRESENT)
#include "nrf_uart.h"
#endif
#if defined (UARTE_PRESENT)
#include "nrf_uarte.h"
#endif

#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"

#include "keyboard_lookup_table.h"
#include "ascii2hid.h"

//Libs not included by example
#include "nrf_delay.h"
#include "boards.h"

//ascii2hid
#include "ascii2hid.h"

//FLASH STUFF
#include "nrf_soc.h"
#include "app_util.h"
#include "nrf_fstorage.h"

#ifdef SOFTDEVICE_PRESENT
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_fstorage_sd.h"
#else
#include "nrf_drv_clock.h"
#include "nrf_fstorage_nvmc.h"
#endif

#define BUTTON_DETECTION_DELAY  APP_TIMER_TICKS(50)
#define APP_BLE_CONN_CFG_TAG    1
#define KEY_LEN 2000

static void idle_state_handle(void);

/* Defined in cli.c */
//extern void cli_init(void);
//extern void cli_start(void);
//extern void cli_process(void);

static void fstorage_evt_handler(nrf_fstorage_evt_t * p_evt);


NRF_FSTORAGE_DEF(nrf_fstorage_t fstorage) =
{
    /* Set a handler for fstorage events. */
    .evt_handler = fstorage_evt_handler,

    /* These below are the boundaries of the flash space assigned to this instance of fstorage.
     * You must set these manually, even at runtime, before nrf_fstorage_init() is called.
     * The function nrf5_flash_end_addr_get() can be used to retrieve the last address on the
     * last page of flash available to write data. */
    .start_addr = 0x3e000,
    .end_addr   = 0x3ffff,
};

/**@brief   Helper function to obtain the last address on the last page of the on-chip flash that
 *          can be used to write user data.
 */
static uint32_t nrf5_flash_end_addr_get()
{
    uint32_t const bootloader_addr = NRF_UICR->NRFFW[0];
    uint32_t const page_sz         = NRF_FICR->CODEPAGESIZE;
    uint32_t const code_sz         = NRF_FICR->CODESIZE;

    return (bootloader_addr != 0xFFFFFFFF ?
            bootloader_addr : (code_sz * page_sz));
}

static void fstorage_evt_handler(nrf_fstorage_evt_t * p_evt)
{
    if (p_evt->result != NRF_SUCCESS)
    {
        NRF_LOG_INFO("--> Event received: ERROR while executing an fstorage operation.");
        return;
    }

    switch (p_evt->id)
    {
        case NRF_FSTORAGE_EVT_WRITE_RESULT:
        {
            NRF_LOG_INFO("--> Event received: wrote %d bytes at address 0x%x.",
                         p_evt->len, p_evt->addr);
        } break;

        case NRF_FSTORAGE_EVT_ERASE_RESULT:
        {
            NRF_LOG_INFO("--> Event received: erased %d page from address 0x%x.",
                         p_evt->len, p_evt->addr);
        } break;

        default:
            break;
    }
}


static void print_flash_info(nrf_fstorage_t * p_fstorage)
{
    NRF_LOG_INFO("========| flash info |========");
    NRF_LOG_INFO("erase unit: \t%d bytes",      p_fstorage->p_flash_info->erase_unit);
    NRF_LOG_INFO("program unit: \t%d bytes",    p_fstorage->p_flash_info->program_unit);
    NRF_LOG_INFO("==============================");
}

/**@brief   Sleep until an event is received. */
static void power_manage(void)
{
#ifdef SOFTDEVICE_PRESENT
    (void) sd_app_evt_wait();
#else
    __WFE();
#endif
}

void wait_for_flash_ready(nrf_fstorage_t const * p_fstorage)
{
    /* While fstorage is busy, sleep and wait for an event. */
    while (nrf_fstorage_is_busy(p_fstorage))
    {
        idle_state_handle();
        //power_manage();
    }
}

#define DEVICE_NAME                         "ScriptedKeys Ver.1.4"                          /**< Name of device. Will be included in the advertising data. */
#define MANUFACTURER_NAME                   "PurdueECE477Team4"                      /**< Manufacturer. Will be passed to Device Information Service. */

#define APP_BLE_OBSERVER_PRIO               3                                          /**< Application's BLE observer priority. You shouldn't need to modify this value. */
#define APP_BLE_CONN_CFG_TAG                1                                          /**< A tag identifying the SoftDevice BLE configuration. */

#define BATTERY_LEVEL_MEAS_INTERVAL         APP_TIMER_TICKS(2000)                      /**< Battery level measurement interval (ticks). */
#define MIN_BATTERY_LEVEL                   81                                         /**< Minimum simulated battery level. */
#define MAX_BATTERY_LEVEL                   100                                        /**< Maximum simulated battery level. */
#define BATTERY_LEVEL_INCREMENT             1                                          /**< Increment between each simulated battery level measurement. */

#define PNP_ID_VENDOR_ID_SOURCE             0x02                                       /**< Vendor ID Source. */
#define PNP_ID_VENDOR_ID                    0x1915                                     /**< Vendor ID. */
#define PNP_ID_PRODUCT_ID                   0xEEEE                                     /**< Product ID. */
#define PNP_ID_PRODUCT_VERSION              0x0001                                     /**< Product Version. */

#define APP_ADV_FAST_INTERVAL               0x0028                                     /**< Fast advertising interval (in units of 0.625 ms. This value corresponds to 25 ms.). */
#define APP_ADV_SLOW_INTERVAL               0x0C80                                     /**< Slow advertising interval (in units of 0.625 ms. This value corrsponds to 2 seconds). */

#define APP_ADV_FAST_DURATION               3000                                       /**< The advertising duration of fast advertising in units of 10 milliseconds. */
#define APP_ADV_SLOW_DURATION               18000                                      /**< The advertising duration of slow advertising in units of 10 milliseconds. */


/*lint -emacro(524, MIN_CONN_INTERVAL) // Loss of precision */
#define MIN_CONN_INTERVAL                   MSEC_TO_UNITS(7.5, UNIT_1_25_MS)           /**< Minimum connection interval (7.5 ms) */
#define MAX_CONN_INTERVAL                   MSEC_TO_UNITS(30, UNIT_1_25_MS)            /**< Maximum connection interval (30 ms). */
#define SLAVE_LATENCY                       6                                          /**< Slave latency. */
#define CONN_SUP_TIMEOUT                    MSEC_TO_UNITS(430, UNIT_10_MS)             /**< Connection supervisory timeout (430 ms). */

#define FIRST_CONN_PARAMS_UPDATE_DELAY      APP_TIMER_TICKS(5000)                      /**< Time from initiating event (connect or start of notification) to first time sd_ble_gap_conn_param_update is called (5 seconds). */
#define NEXT_CONN_PARAMS_UPDATE_DELAY       APP_TIMER_TICKS(30000)                     /**< Time between each call to sd_ble_gap_conn_param_update after the first call (30 seconds). */
#define MAX_CONN_PARAMS_UPDATE_COUNT        3                                          /**< Number of attempts before giving up the connection parameter negotiation. */

#define SEC_PARAM_BOND                      1                                          /**< Perform bonding. */
#define SEC_PARAM_MITM                      0                                          /**< Man In The Middle protection not required. */
#define SEC_PARAM_LESC                      0                                          /**< LE Secure Connections not enabled. */
#define SEC_PARAM_KEYPRESS                  0                                          /**< Keypress notifications not enabled. */
#define SEC_PARAM_IO_CAPABILITIES           BLE_GAP_IO_CAPS_NONE                       /**< No I/O capabilities. */
#define SEC_PARAM_OOB                       0                                          /**< Out Of Band data not available. */
#define SEC_PARAM_MIN_KEY_SIZE              7                                          /**< Minimum encryption key size. */
#define SEC_PARAM_MAX_KEY_SIZE              16                                         /**< Maximum encryption key size. */

#define OUTPUT_REPORT_INDEX                 0                                          /**< Index of Output Report. */
#define OUTPUT_REPORT_MAX_LEN               1                                          /**< Maximum length of Output Report. */
#define INPUT_REPORT_KEYS_INDEX             0                                          /**< Index of Input Report. */
#define OUTPUT_REPORT_BIT_MASK_CAPS_LOCK    0x02                                       /**< CAPS LOCK bit in Output Report (based on 'LED Page (0x08)' of the Universal Serial Bus HID Usage Tables). */
#define INPUT_REP_REF_ID                    0                                          /**< Id of reference to Keyboard Input Report. */
#define OUTPUT_REP_REF_ID                   0                                          /**< Id of reference to Keyboard Output Report. */
#define FEATURE_REP_REF_ID                  0                                          /**< ID of reference to Keyboard Feature Report. */
#define FEATURE_REPORT_MAX_LEN              2                                          /**< Maximum length of Feature Report. */
#define FEATURE_REPORT_INDEX                0                                          /**< Index of Feature Report. */

#define MAX_BUFFER_ENTRIES                  1000                                          /**< Number of elements that can be enqueued */

#define BASE_USB_HID_SPEC_VERSION           0x0101                                     /**< Version number of base USB HID Specification implemented by this application. */

#define INPUT_REPORT_KEYS_MAX_LEN           8                                          /**< Maximum length of the Input Report characteristic. */

#define DEAD_BEEF                           0xDEADBEEF                                 /**< Value used as error code on stack dump, can be used to identify stack location on stack unwind. */

#define SCHED_MAX_EVENT_DATA_SIZE           APP_TIMER_SCHED_EVENT_DATA_SIZE            /**< Maximum size of scheduler events. */
#ifdef SVCALL_AS_NORMAL_FUNCTION
#define SCHED_QUEUE_SIZE                    20                                         /**< Maximum number of events in the scheduler queue. More is needed in case of Serialization. */
#else
#define SCHED_QUEUE_SIZE                    10                                         /**< Maximum number of events in the scheduler queue. */
#endif

#define MODIFIER_KEY_POS                    0                                          /**< Position of the modifier byte in the Input Report. */
#define SCAN_CODE_POS                       2                                          /**< The start position of the key scan code in a HID Report. */
#define SHIFT_KEY_CODE                      0x02                                       /**< Key code indicating the press of the Shift Key. */                    

#define MAX_KEYS_IN_ONE_REPORT              (INPUT_REPORT_KEYS_MAX_LEN - SCAN_CODE_POS)/**< Maximum number of key presses that can be sent in one Input Report. */

#define row_length                          8
#define col_length                          8


#define COL0                                3
#define COL1                                4
#define COL2                                5
#define COL3                                14
#define COL4                                7
#define COL5                                8
#define COL6                                9
#define COL7                                10
uint8_t COLS[col_length] =  {COL0, COL1, COL2, COL3, COL4, COL5, COL6, COL7};

#define ROW0                                25
#define ROW1                                26
#define ROW2                                27
#define ROW3                                28
#define ROW4                                29
#define ROW5                                30
#define ROW6                                31
#define ROW7                                2
uint8_t ROWS[row_length] = {ROW0, ROW1, ROW2, ROW3, ROW4, ROW5, ROW6, ROW7};

#define HIGH                                1
#define LOW                                 0

#define GARBAGE_KEY                         64

#define LED_RIGHT                           12
#define LED_LEFT                            13
#define SWITCH_LEFT                         11
#define SWITCH_RIGHT                        22
//#define SCANNER_RX                          0 //Change it to 23
//#define SCANNER_TX                          1 //Change it to 24

#define INIT_HOLD_COOLDOWN                  250
#define SEC_HOLD_COOLDOWN                   50

#define NUM_MACROS                          12

#define NUM_TABLES                          32
#define TABLE_LENGTH                        64
#define MAX_TABLE_ENTRY                     2*TABLE_LENGTH*NUM_TABLES
#define MACRO_MAX_LENGTH                    256

#define MAX_FILE_BUFFER_LENGTH              64


#define MAX_TEST_DATA_BYTES     (15U)                /**< max number of test bytes to be used for tx and rx. */
#define UART_TX_BUF_SIZE 256                         /**< UART TX buffer size. */
#define UART_RX_BUF_SIZE 256                         /**< UART RX buffer size. */
#define UART_HWFC APP_UART_FLOW_CONTROL_DISABLED

//Some functions
void manage_send_keypress(uint8_t, uint16_t, uint8_t, bool);
uint32_t scanMatrix(uint8_t*);
uint16_t check_for_modifiers(uint8_t);
uint8_t update_prev_key (uint8_t*, uint8_t, uint8_t, uint8_t*, uint8_t);
void set_key_press(uint8_t, uint8_t);
void set_modifiers(uint8_t, uint8_t, bool);

void process_next_char(char);
uint8_t load_from_table(uint8_t, uint8_t, uint32_t, uint16_t);


#define UART_TX_BUF_SIZE 256
#define UART_RX_BUF_SIZE 256

#define KEY_LEN 300

BLE_NUS_DEF(m_nus, NRF_SDH_BLE_TOTAL_LINK_COUNT);                                   /**< BLE NUS service instance. */
static uint16_t m_ble_nus_max_data_len = BLE_GATT_ATT_MTU_DEFAULT - 3;     /**< Maximum length of data (in bytes) that can be transmitted to the peer by the Nordic UART service module. */

/**Buffer queue access macros
 *
 * @{ */
/** Initialization of buffer list */
#define BUFFER_LIST_INIT()     \
    do                         \
    {                          \
        buffer_list.rp    = 0; \
        buffer_list.wp    = 0; \
        buffer_list.count = 0; \
    } while (0)

/** Provide status of data list is full or not */
#define BUFFER_LIST_FULL() \
    ((MAX_BUFFER_ENTRIES == buffer_list.count - 1) ? true : false)

/** Provides status of buffer list is empty or not */
#define BUFFER_LIST_EMPTY() \
    ((0 == buffer_list.count) ? true : false)

#define BUFFER_ELEMENT_INIT(i)                 \
    do                                         \
    {                                          \
        buffer_list.buffer[(i)].p_data = NULL; \
    } while (0)

/** @} */

/** Abstracts buffer element */
typedef struct hid_key_buffer
{
    uint8_t      data_offset; /**< Max Data that can be buffered for all entries */
    uint8_t      data_len;    /**< Total length of data */
    uint8_t    * p_data;      /**< Scanned key pattern */
    ble_hids_t * p_instance;  /**< Identifies peer and service instance */
} buffer_entry_t;

STATIC_ASSERT(sizeof(buffer_entry_t) % 4 == 0);

/** Circular buffer list */
typedef struct
{
    buffer_entry_t buffer[MAX_BUFFER_ENTRIES]; /**< Maximum number of entries that can enqueued in the list */
    uint8_t        rp;                         /**< Index to the read location */
    uint8_t        wp;                         /**< Index to write location */
    uint8_t        count;                      /**< Number of elements in the list */
} buffer_list_t;

STATIC_ASSERT(sizeof(buffer_list_t) % 4 == 0);


APP_TIMER_DEF(m_battery_timer_id);                                  /**< Battery timer. */
BLE_HIDS_DEF(m_hids,                                                /**< Structure used to identify the HID service. */
             NRF_SDH_BLE_TOTAL_LINK_COUNT,
             INPUT_REPORT_KEYS_MAX_LEN,
             OUTPUT_REPORT_MAX_LEN,
             FEATURE_REPORT_MAX_LEN);
BLE_BAS_DEF(m_bas);                                                 /**< Structure used to identify the battery service. */
NRF_BLE_GATT_DEF(m_gatt);                                           /**< GATT module instance. */
NRF_BLE_QWR_DEF(m_qwr);                                             /**< Context for the Queued Write module.*/
BLE_ADVERTISING_DEF(m_advertising);                                 /**< Advertising module instance. */

static bool              m_in_boot_mode = false;                    /**< Current protocol mode. */
static uint16_t          m_conn_handle  = BLE_CONN_HANDLE_INVALID;  /**< Handle of the current connection. */
static sensorsim_cfg_t   m_battery_sim_cfg;                         /**< Battery Level sensor simulator configuration. */
static sensorsim_state_t m_battery_sim_state;                       /**< Battery Level sensor simulator state. */
static bool              m_caps_on = false;                         /**< Variable to indicate if Caps Lock is turned on. */
static pm_peer_id_t      m_peer_id;                                 /**< Device reference handle to the current bonded central. */
static buffer_list_t     buffer_list;                               /**< List to enqueue not just data to be sent, but also related information like the handle, connection handle etc */

//-=-=-=-=-=-=-=-=-=-==-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-==-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-==-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
//Team4 defined values
uint8_t modifiers = 0;
uint8_t mode = 0;
bool caps_lock = false; //0x39
bool scroll_lock = false;  //0x47
bool num_lock = true;  //0x53
bool fn_lock = false;  //0xE9

uint8_t macro_active = 0;  //0 - no macro active, 1 - 12 - The corresponding macro is active

//Stuff for loading from Terraterm
uint8_t load_active = 0; //0 - normal, 1 - ready to load, 2 - loading
uint32_t load_table_index = 0;
uint8_t current_value = 0;
bool value_ready = false;

bool processing_macro = false;
uint8_t curr_macro = 0;

uint8_t macro_addr_offset;


//-=-=-=-=-=-=-=-=-=-==-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-==-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-==-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

static ble_uuid_t m_adv_uuids[] = {{BLE_UUID_HUMAN_INTERFACE_DEVICE_SERVICE, BLE_UUID_TYPE_BLE}};

static uint8_t m_sample_key_press_scan_str[] = /**< Key pattern to be sent when the key press button has been pushed. */
{
    0x0b,       /* Key h */
    0x08,       /* Key e */
    0x0f,       /* Key l */
    0x0f,       /* Key l */
    0x12,       /* Key o */
    0x28        /* Key Return */
};

static uint8_t m_caps_on_key_scan_str[] = /**< Key pattern to be sent when the output report has been written with the CAPS LOCK bit set. */
{
    0x06,       /* Key C */
    0x04,       /* Key a */
    0x13,       /* Key p */
    0x16,       /* Key s */
    0x12,       /* Key o */
    0x11,       /* Key n */
};

static uint8_t m_caps_off_key_scan_str[] = /**< Key pattern to be sent when the output report has been written with the CAPS LOCK bit cleared. */
{
    0x06,       /* Key C */
    0x04,       /* Key a */
    0x13,       /* Key p */
    0x16,       /* Key s */
    0x12,       /* Key o */
    0x09,       /* Key f */
};

void uart_error_handle(app_uart_evt_t * p_event)
{
    /*if (p_event->evt_type == APP_UART_COMMUNICATION_ERROR)
    {
        APP_ERROR_HANDLER(p_event->data.error_communication);
    }
    else if (p_event->evt_type == APP_UART_FIFO_ERROR)
    {
        APP_ERROR_HANDLER(p_event->data.error_code);
    }*/
}

void uart_init()
{
  uint32_t err_code;

    bsp_board_init(BSP_INIT_LEDS);

    const app_uart_comm_params_t comm_params =
      {
          RX_PIN_NUMBER,
          TX_PIN_NUMBER,
          RTS_PIN_NUMBER,
          CTS_PIN_NUMBER,
          UART_HWFC,
          false,
#if defined (UART_PRESENT)
          NRF_UART_BAUDRATE_9600
#endif
      };

    APP_UART_FIFO_INIT(&comm_params,
                         UART_RX_BUF_SIZE,
                         UART_TX_BUF_SIZE,
                         uart_error_handle,
                         APP_IRQ_PRIORITY_LOWEST,
                         err_code);

    APP_ERROR_CHECK(err_code);
}

static void on_hids_evt(ble_hids_t * p_hids, ble_hids_evt_t * p_evt);

/**@brief Callback function for asserts in the SoftDevice.
 *
 * @details This function will be called in case of an assert in the SoftDevice.
 *
 * @warning This handler is an example only and does not fit a final product. You need to analyze
 *          how your product is supposed to react in case of Assert.
 * @warning On assert from the SoftDevice, the system can only recover on reset.
 *
 * @param[in]   line_num   Line number of the failing ASSERT call.
 * @param[in]   file_name  File name of the failing ASSERT call.
 */
void assert_nrf_callback(uint16_t line_num, const uint8_t * p_file_name)
{
    app_error_handler(DEAD_BEEF, line_num, p_file_name);
}


/**@brief Function for setting filtered whitelist.
 *
 * @param[in] skip  Filter passed to @ref pm_peer_id_list.
 */
static void whitelist_set(pm_peer_id_list_skip_t skip)
{
    pm_peer_id_t peer_ids[BLE_GAP_WHITELIST_ADDR_MAX_COUNT];
    uint32_t     peer_id_count = BLE_GAP_WHITELIST_ADDR_MAX_COUNT;

    ret_code_t err_code = pm_peer_id_list(peer_ids, &peer_id_count, PM_PEER_ID_INVALID, skip);
    APP_ERROR_CHECK(err_code);

    NRF_LOG_INFO("\tm_whitelist_peer_cnt %d, MAX_PEERS_WLIST %d",
                   peer_id_count + 1,
                   BLE_GAP_WHITELIST_ADDR_MAX_COUNT);

    err_code = pm_whitelist_set(peer_ids, peer_id_count);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for setting filtered device identities.
 *
 * @param[in] skip  Filter passed to @ref pm_peer_id_list.
 */
static void identities_set(pm_peer_id_list_skip_t skip)
{
    pm_peer_id_t peer_ids[BLE_GAP_DEVICE_IDENTITIES_MAX_COUNT];
    uint32_t     peer_id_count = BLE_GAP_DEVICE_IDENTITIES_MAX_COUNT;

    ret_code_t err_code = pm_peer_id_list(peer_ids, &peer_id_count, PM_PEER_ID_INVALID, skip);
    APP_ERROR_CHECK(err_code);

    err_code = pm_device_identities_list_set(peer_ids, peer_id_count);
    APP_ERROR_CHECK(err_code);
}


/**@brief Clear bond information from persistent storage.
 */
static void delete_bonds(void)
{
    ret_code_t err_code;

    NRF_LOG_INFO("Erase bonds!");

    err_code = pm_peers_delete();
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for starting advertising.
 */
static void advertising_start(bool erase_bonds)
{
    if (erase_bonds == true)
    {
        delete_bonds();
        // Advertising is started by PM_EVT_PEERS_DELETE_SUCCEEDED event.
    }
    else
    {
        whitelist_set(PM_PEER_ID_LIST_SKIP_NO_ID_ADDR);

        ret_code_t ret = ble_advertising_start(&m_advertising, BLE_ADV_MODE_FAST);
        APP_ERROR_CHECK(ret);
    }
}


/**@brief Function for handling Peer Manager events.
 *
 * @param[in] p_evt  Peer Manager event.
 */
static void pm_evt_handler(pm_evt_t const * p_evt)
{
    pm_handler_on_pm_evt(p_evt);
    pm_handler_flash_clean(p_evt);

    switch (p_evt->evt_id)
    {
        case PM_EVT_PEERS_DELETE_SUCCEEDED:
            advertising_start(false);
            break;

        case PM_EVT_PEER_DATA_UPDATE_SUCCEEDED:
            if (     p_evt->params.peer_data_update_succeeded.flash_changed
                 && (p_evt->params.peer_data_update_succeeded.data_id == PM_PEER_DATA_ID_BONDING))
            {
                NRF_LOG_INFO("New Bond, add the peer to the whitelist if possible");
                // Note: You should check on what kind of white list policy your application should use.

                whitelist_set(PM_PEER_ID_LIST_SKIP_NO_ID_ADDR);
            }
            break;

        default:
            break;
    }
}


/**@brief Function for handling Service errors.
 *
 * @details A pointer to this function will be passed to each service which may need to inform the
 *          application about an error.
 *
 * @param[in]   nrf_error   Error code containing information about what went wrong.
 */
static void service_error_handler(uint32_t nrf_error)
{
    APP_ERROR_HANDLER(nrf_error);
}


/**@brief Function for handling advertising errors.
 *
 * @param[in] nrf_error  Error code containing information about what went wrong.
 */
static void ble_advertising_error_handler(uint32_t nrf_error)
{
    APP_ERROR_HANDLER(nrf_error);
}


/**@brief Function for performing a battery measurement, and update the Battery Level characteristic in the Battery Service.
 */
static void battery_level_update(void)
{
    ret_code_t err_code;
    uint8_t  battery_level;

    battery_level = (uint8_t)sensorsim_measure(&m_battery_sim_state, &m_battery_sim_cfg);

    err_code = ble_bas_battery_level_update(&m_bas, battery_level, BLE_CONN_HANDLE_ALL);
    if ((err_code != NRF_SUCCESS) &&
        (err_code != NRF_ERROR_BUSY) &&
        (err_code != NRF_ERROR_RESOURCES) &&
        (err_code != NRF_ERROR_FORBIDDEN) &&
        (err_code != NRF_ERROR_INVALID_STATE) &&
        (err_code != BLE_ERROR_GATTS_SYS_ATTR_MISSING)
       )
    {
        APP_ERROR_HANDLER(err_code);
    }
}


/**@brief Function for handling the Battery measurement timer timeout.
 *
 * @details This function will be called each time the battery level measurement timer expires.
 *
 * @param[in]   p_context   Pointer used for passing some arbitrary information (context) from the
 *                          app_start_timer() call to the timeout handler.
 */
static void battery_level_meas_timeout_handler(void * p_context)
{
    UNUSED_PARAMETER(p_context);
    battery_level_update();
}


/**@brief Function for the Timer initialization.
 *
 * @details Initializes the timer module.
 */
static void timers_init(void)
{
    ret_code_t err_code;

    err_code = app_timer_init();
    APP_ERROR_CHECK(err_code);

    // Create battery timer.
    err_code = app_timer_create(&m_battery_timer_id,
                                APP_TIMER_MODE_REPEATED,
                                battery_level_meas_timeout_handler);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for the GAP initialization.
 *
 * @details This function sets up all the necessary GAP (Generic Access Profile) parameters of the
 *          device including the device name, appearance, and the preferred connection parameters.
 */
static void gap_params_init(void)
{
    ret_code_t              err_code;
    ble_gap_conn_params_t   gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

    err_code = sd_ble_gap_device_name_set(&sec_mode,
                                          (const uint8_t *)DEVICE_NAME,
                                          strlen(DEVICE_NAME));
    APP_ERROR_CHECK(err_code);

    err_code = sd_ble_gap_appearance_set(BLE_APPEARANCE_HID_KEYBOARD);
    APP_ERROR_CHECK(err_code);

    memset(&gap_conn_params, 0, sizeof(gap_conn_params));

    gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;
    gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;
    gap_conn_params.slave_latency     = SLAVE_LATENCY;
    gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;

    err_code = sd_ble_gap_ppcp_set(&gap_conn_params);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for initializing the GATT module.
 */
static void gatt_init(void)
{
    ret_code_t err_code = nrf_ble_gatt_init(&m_gatt, NULL);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for handling Queued Write Module errors.
 *
 * @details A pointer to this function will be passed to each service which may need to inform the
 *          application about an error.
 *
 * @param[in]   nrf_error   Error code containing information about what went wrong.
 */
static void nrf_qwr_error_handler(uint32_t nrf_error)
{
    APP_ERROR_HANDLER(nrf_error);
}


/**@brief Function for initializing the Queued Write Module.
 */
static void qwr_init(void)
{
    ret_code_t         err_code;
    nrf_ble_qwr_init_t qwr_init_obj = {0};

    qwr_init_obj.error_handler = nrf_qwr_error_handler;

    err_code = nrf_ble_qwr_init(&m_qwr, &qwr_init_obj);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for initializing Device Information Service.
 */
static void dis_init(void)
{
    ret_code_t       err_code;
    ble_dis_init_t   dis_init_obj;
    ble_dis_pnp_id_t pnp_id;

    pnp_id.vendor_id_source = PNP_ID_VENDOR_ID_SOURCE;
    pnp_id.vendor_id        = PNP_ID_VENDOR_ID;
    pnp_id.product_id       = PNP_ID_PRODUCT_ID;
    pnp_id.product_version  = PNP_ID_PRODUCT_VERSION;

    memset(&dis_init_obj, 0, sizeof(dis_init_obj));

    ble_srv_ascii_to_utf8(&dis_init_obj.manufact_name_str, MANUFACTURER_NAME);
    dis_init_obj.p_pnp_id = &pnp_id;

    dis_init_obj.dis_char_rd_sec = SEC_JUST_WORKS;

    err_code = ble_dis_init(&dis_init_obj);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for initializing Battery Service.
 */
static void bas_init(void)
{
    ret_code_t     err_code;
    ble_bas_init_t bas_init_obj;

    memset(&bas_init_obj, 0, sizeof(bas_init_obj));

    bas_init_obj.evt_handler          = NULL;
    bas_init_obj.support_notification = true;
    bas_init_obj.p_report_ref         = NULL;
    bas_init_obj.initial_batt_level   = 100;

    bas_init_obj.bl_rd_sec        = SEC_JUST_WORKS;
    bas_init_obj.bl_cccd_wr_sec   = SEC_JUST_WORKS;
    bas_init_obj.bl_report_rd_sec = SEC_JUST_WORKS;

    err_code = ble_bas_init(&m_bas, &bas_init_obj);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for initializing HID Service.
 */
static void hids_init(void)
{
    ret_code_t                    err_code;
    ble_hids_init_t               hids_init_obj;
    ble_hids_inp_rep_init_t     * p_input_report;
    ble_hids_outp_rep_init_t    * p_output_report;
    ble_hids_feature_rep_init_t * p_feature_report;
    uint8_t                       hid_info_flags;

    static ble_hids_inp_rep_init_t     input_report_array[1];
    static ble_hids_outp_rep_init_t    output_report_array[1];
    static ble_hids_feature_rep_init_t feature_report_array[1];
    static uint8_t                     report_map_data[] =
    {
        0x05, 0x01,       // Usage Page (Generic Desktop)
        0x09, 0x06,       // Usage (Keyboard)
        0xA1, 0x01,       // Collection (Application)
        0x05, 0x07,       // Usage Page (Key Codes)
        0x19, 0xe0,       // Usage Minimum (224)
        0x29, 0xe7,       // Usage Maximum (231)
        0x15, 0x00,       // Logical Minimum (0)
        0x25, 0x01,       // Logical Maximum (1)
        0x75, 0x01,       // Report Size (1)
        0x95, 0x08,       // Report Count (8)
        0x81, 0x02,       // Input (Data, Variable, Absolute)

        0x95, 0x01,       // Report Count (1)
        0x75, 0x08,       // Report Size (8)
        0x81, 0x01,       // Input (Constant) reserved byte(1)

        0x95, 0x05,       // Report Count (5)
        0x75, 0x01,       // Report Size (1)
        0x05, 0x08,       // Usage Page (Page# for LEDs)
        0x19, 0x01,       // Usage Minimum (1)
        0x29, 0x05,       // Usage Maximum (5)
        0x91, 0x02,       // Output (Data, Variable, Absolute), Led report
        0x95, 0x01,       // Report Count (1)
        0x75, 0x03,       // Report Size (3)
        0x91, 0x01,       // Output (Data, Variable, Absolute), Led report padding

        0x95, 0x06,       // Report Count (6)
        0x75, 0x08,       // Report Size (8)
        0x15, 0x00,       // Logical Minimum (0)
        0x25, 0x65,       // Logical Maximum (101)
        0x05, 0x07,       // Usage Page (Key codes)
        0x19, 0x00,       // Usage Minimum (0)
        0x29, 0x65,       // Usage Maximum (101)
        0x81, 0x00,       // Input (Data, Array) Key array(6 bytes)

        0x09, 0x05,       // Usage (Vendor Defined)
        0x15, 0x00,       // Logical Minimum (0)
        0x26, 0xFF, 0x00, // Logical Maximum (255)
        0x75, 0x08,       // Report Size (8 bit)
        0x95, 0x02,       // Report Count (2)
        0xB1, 0x02,       // Feature (Data, Variable, Absolute)

        0xC0              // End Collection (Application)
    };

    memset((void *)input_report_array, 0, sizeof(ble_hids_inp_rep_init_t));
    memset((void *)output_report_array, 0, sizeof(ble_hids_outp_rep_init_t));
    memset((void *)feature_report_array, 0, sizeof(ble_hids_feature_rep_init_t));

    // Initialize HID Service
    p_input_report                      = &input_report_array[INPUT_REPORT_KEYS_INDEX];
    p_input_report->max_len             = INPUT_REPORT_KEYS_MAX_LEN;
    p_input_report->rep_ref.report_id   = INPUT_REP_REF_ID;
    p_input_report->rep_ref.report_type = BLE_HIDS_REP_TYPE_INPUT;

    p_input_report->sec.cccd_wr = SEC_JUST_WORKS;
    p_input_report->sec.wr      = SEC_JUST_WORKS;
    p_input_report->sec.rd      = SEC_JUST_WORKS;

    p_output_report                      = &output_report_array[OUTPUT_REPORT_INDEX];
    p_output_report->max_len             = OUTPUT_REPORT_MAX_LEN;
    p_output_report->rep_ref.report_id   = OUTPUT_REP_REF_ID;
    p_output_report->rep_ref.report_type = BLE_HIDS_REP_TYPE_OUTPUT;

    p_output_report->sec.wr = SEC_JUST_WORKS;
    p_output_report->sec.rd = SEC_JUST_WORKS;

    p_feature_report                      = &feature_report_array[FEATURE_REPORT_INDEX];
    p_feature_report->max_len             = FEATURE_REPORT_MAX_LEN;
    p_feature_report->rep_ref.report_id   = FEATURE_REP_REF_ID;
    p_feature_report->rep_ref.report_type = BLE_HIDS_REP_TYPE_FEATURE;

    p_feature_report->sec.rd              = SEC_JUST_WORKS;
    p_feature_report->sec.wr              = SEC_JUST_WORKS;

    hid_info_flags = HID_INFO_FLAG_REMOTE_WAKE_MSK | HID_INFO_FLAG_NORMALLY_CONNECTABLE_MSK;

    memset(&hids_init_obj, 0, sizeof(hids_init_obj));

    hids_init_obj.evt_handler                    = on_hids_evt;
    hids_init_obj.error_handler                  = service_error_handler;
    hids_init_obj.is_kb                          = true;
    hids_init_obj.is_mouse                       = false;
    hids_init_obj.inp_rep_count                  = 1;
    hids_init_obj.p_inp_rep_array                = input_report_array;
    hids_init_obj.outp_rep_count                 = 1;
    hids_init_obj.p_outp_rep_array               = output_report_array;
    hids_init_obj.feature_rep_count              = 1;
    hids_init_obj.p_feature_rep_array            = feature_report_array;
    hids_init_obj.rep_map.data_len               = sizeof(report_map_data);
    hids_init_obj.rep_map.p_data                 = report_map_data;
    hids_init_obj.hid_information.bcd_hid        = BASE_USB_HID_SPEC_VERSION;
    hids_init_obj.hid_information.b_country_code = 0;
    hids_init_obj.hid_information.flags          = hid_info_flags;
    hids_init_obj.included_services_count        = 0;
    hids_init_obj.p_included_services_array      = NULL;

    hids_init_obj.rep_map.rd_sec         = SEC_JUST_WORKS;
    hids_init_obj.hid_information.rd_sec = SEC_JUST_WORKS;

    hids_init_obj.boot_kb_inp_rep_sec.cccd_wr = SEC_JUST_WORKS;
    hids_init_obj.boot_kb_inp_rep_sec.rd      = SEC_JUST_WORKS;

    hids_init_obj.boot_kb_outp_rep_sec.rd = SEC_JUST_WORKS;
    hids_init_obj.boot_kb_outp_rep_sec.wr = SEC_JUST_WORKS;

    hids_init_obj.protocol_mode_rd_sec = SEC_JUST_WORKS;
    hids_init_obj.protocol_mode_wr_sec = SEC_JUST_WORKS;
    hids_init_obj.ctrl_point_wr_sec    = SEC_JUST_WORKS;

    err_code = ble_hids_init(&m_hids, &hids_init_obj);
    APP_ERROR_CHECK(err_code);
}

static void nus_data_handler(ble_nus_evt_t * p_evt)
{

    if (p_evt->type == BLE_NUS_EVT_RX_DATA)
    {
        uint32_t err_code;

        NRF_LOG_DEBUG("Received data from BLE NUS. Writing data on UART.");
        NRF_LOG_HEXDUMP_DEBUG(p_evt->params.rx_data.p_data, p_evt->params.rx_data.length);

        for (uint32_t i = 0; i < p_evt->params.rx_data.length; i++)
        {
            do
            {
                err_code = app_uart_put(p_evt->params.rx_data.p_data[i]);
                if ((err_code != NRF_SUCCESS) && (err_code != NRF_ERROR_BUSY))
                {
                    NRF_LOG_ERROR("Failed receiving NUS message. Error 0x%x. ", err_code);
                    APP_ERROR_CHECK(err_code);
                }
            } while (err_code == NRF_ERROR_BUSY);
        }
        if (p_evt->params.rx_data.p_data[p_evt->params.rx_data.length - 1] == '\r')
        {
            while (app_uart_put('\n') == NRF_ERROR_BUSY);
        }
    }

}

/**@brief Function for initializing services that will be used by the application.
 */
static void services_init(void)
{
    ret_code_t err_code;
    qwr_init();
    dis_init();
    bas_init();
    hids_init();
    ble_nus_init_t     nus_init;

    // Initialize NUS.
    memset(&nus_init, 0, sizeof(nus_init));

    nus_init.data_handler = nus_data_handler;

    err_code = ble_nus_init(&m_nus, &nus_init);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for initializing the battery sensor simulator.
 */
static void sensor_simulator_init(void)
{
    m_battery_sim_cfg.min          = MIN_BATTERY_LEVEL;
    m_battery_sim_cfg.max          = MAX_BATTERY_LEVEL;
    m_battery_sim_cfg.incr         = BATTERY_LEVEL_INCREMENT;
    m_battery_sim_cfg.start_at_max = true;

    sensorsim_init(&m_battery_sim_state, &m_battery_sim_cfg);
}


/**@brief Function for handling a Connection Parameters error.
 *
 * @param[in]   nrf_error   Error code containing information about what went wrong.
 */
static void conn_params_error_handler(uint32_t nrf_error)
{
    APP_ERROR_HANDLER(nrf_error);
}


/**@brief Function for initializing the Connection Parameters module.
 */
static void conn_params_init(void)
{
    ret_code_t             err_code;
    ble_conn_params_init_t cp_init;

    memset(&cp_init, 0, sizeof(cp_init));

    cp_init.p_conn_params                  = NULL;
    cp_init.first_conn_params_update_delay = FIRST_CONN_PARAMS_UPDATE_DELAY;
    cp_init.next_conn_params_update_delay  = NEXT_CONN_PARAMS_UPDATE_DELAY;
    cp_init.max_conn_params_update_count   = MAX_CONN_PARAMS_UPDATE_COUNT;
    cp_init.start_on_notify_cccd_handle    = BLE_GATT_HANDLE_INVALID;
    cp_init.disconnect_on_fail             = false;
    cp_init.evt_handler                    = NULL;
    cp_init.error_handler                  = conn_params_error_handler;

    err_code = ble_conn_params_init(&cp_init);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for starting timers.
 */
static void timers_start(void)
{
    ret_code_t err_code;

    err_code = app_timer_start(m_battery_timer_id, BATTERY_LEVEL_MEAS_INTERVAL, NULL);
    APP_ERROR_CHECK(err_code);
}


/**@brief   Function for transmitting a key scan Press & Release Notification.
 *
 * @warning This handler is an example only. You need to analyze how you wish to send the key
 *          release.
 *
 * @param[in]  p_instance     Identifies the service for which Key Notifications are requested.
 * @param[in]  p_key_pattern  Pointer to key pattern.
 * @param[in]  pattern_len    Length of key pattern. 0 < pattern_len < 7.
 * @param[in]  pattern_offset Offset applied to Key Pattern for transmission.
 * @param[out] actual_len     Provides actual length of Key Pattern transmitted, making buffering of
 *                            rest possible if needed.
 * @return     NRF_SUCCESS on success, NRF_ERROR_RESOURCES in case transmission could not be
 *             completed due to lack of transmission buffer or other error codes indicating reason
 *             for failure.
 *
 * @note       In case of NRF_ERROR_RESOURCES, remaining pattern that could not be transmitted
 *             can be enqueued \ref buffer_enqueue function.
 *             In case a pattern of 'cofFEe' is the p_key_pattern, with pattern_len as 6 and
 *             pattern_offset as 0, the notifications as observed on the peer side would be
 *             1>    'c', 'o', 'f', 'F', 'E', 'e'
 *             2>    -  , 'o', 'f', 'F', 'E', 'e'
 *             3>    -  ,   -, 'f', 'F', 'E', 'e'
 *             4>    -  ,   -,   -, 'F', 'E', 'e'
 *             5>    -  ,   -,   -,   -, 'E', 'e'
 *             6>    -  ,   -,   -,   -,   -, 'e'
 *             7>    -  ,   -,   -,   -,   -,  -
 *             Here, '-' refers to release, 'c' refers to the key character being transmitted.
 *             Therefore 7 notifications will be sent.
 *             In case an offset of 4 was provided, the pattern notifications sent will be from 5-7
 *             will be transmitted.
 */
static uint32_t send_key_scan_press_release(ble_hids_t * p_hids,
                                            uint8_t    * p_key_pattern,
                                            uint16_t     pattern_len,
                                            uint16_t     pattern_offset,
                                            uint16_t   * p_actual_len)
{
    ret_code_t err_code;
    uint16_t offset;
    uint16_t data_len;
    uint8_t  data[INPUT_REPORT_KEYS_MAX_LEN];

    // HID Report Descriptor enumerates an array of size 6, the pattern hence shall not be any
    // longer than this.
    STATIC_ASSERT((INPUT_REPORT_KEYS_MAX_LEN - 2) == 6);

    ASSERT(pattern_len <= (INPUT_REPORT_KEYS_MAX_LEN - 2));

    offset   = pattern_offset;
    data_len = pattern_len;

    do
    {
        // Reset the data buffer.
        memset(data, 0, sizeof(data));

        // Copy the scan code.
        memcpy(data + SCAN_CODE_POS + offset, p_key_pattern + offset, data_len - offset);

        if(modifiers != 0) {
          data[MODIFIER_KEY_POS] = modifiers;
        }

        if (!m_in_boot_mode)
        {
            err_code = ble_hids_inp_rep_send(p_hids,
                                             INPUT_REPORT_KEYS_INDEX,
                                             INPUT_REPORT_KEYS_MAX_LEN,
                                             data,
                                             m_conn_handle);
        }
        else
        {
            err_code = ble_hids_boot_kb_inp_rep_send(p_hids,
                                                     INPUT_REPORT_KEYS_MAX_LEN,
                                                     data,
                                                     m_conn_handle);
        }

        if (err_code != NRF_SUCCESS)
        {
            break;
        }

        offset++;
    }while (offset <= data_len);

    *p_actual_len = offset;

    return err_code;
}


/**@brief   Function for initializing the buffer queue used to key events that could not be
 *          transmitted
 *
 * @warning This handler is an example only. You need to analyze how you wish to buffer or buffer at
 *          all.
 *
 * @note    In case of HID keyboard, a temporary buffering could be employed to handle scenarios
 *          where encryption is not yet enabled or there was a momentary link loss or there were no
 *          Transmit buffers.
 */
static void buffer_init(void)
{
    uint32_t buffer_count;

    BUFFER_LIST_INIT();

    for (buffer_count = 0; buffer_count < MAX_BUFFER_ENTRIES; buffer_count++)
    {
        BUFFER_ELEMENT_INIT(buffer_count);
    }
}


/**@brief Function for enqueuing key scan patterns that could not be transmitted either completely
 *        or partially.
 *
 * @warning This handler is an example only. You need to analyze how you wish to send the key
 *          release.
 *
 * @param[in]  p_hids         Identifies the service for which Key Notifications are buffered.
 * @param[in]  p_key_pattern  Pointer to key pattern.
 * @param[in]  pattern_len    Length of key pattern.
 * @param[in]  offset         Offset applied to Key Pattern when requesting a transmission on
 *                            dequeue, @ref buffer_dequeue.
 * @return     NRF_SUCCESS on success, else an error code indicating reason for failure.
 */
static uint32_t buffer_enqueue(ble_hids_t * p_hids,
                               uint8_t    * p_key_pattern,
                               uint16_t     pattern_len,
                               uint16_t     offset)
{
    buffer_entry_t * element;
    uint32_t         err_code = NRF_SUCCESS;

    if (BUFFER_LIST_FULL())
    {
        // Element cannot be buffered.
        err_code = NRF_ERROR_NO_MEM;
    }
    else
    {
        // Make entry of buffer element and copy data.
        element              = &buffer_list.buffer[(buffer_list.wp)];
        element->p_instance  = p_hids;
        element->p_data      = p_key_pattern;
        element->data_offset = offset;
        element->data_len    = pattern_len;

        buffer_list.count++;
        buffer_list.wp++;

        if (buffer_list.wp == MAX_BUFFER_ENTRIES)
        {
            buffer_list.wp = 0;
        }
    }

    return err_code;
}


/**@brief   Function to dequeue key scan patterns that could not be transmitted either completely of
 *          partially.
 *
 * @warning This handler is an example only. You need to analyze how you wish to send the key
 *          release.
 *
 * @param[in]  tx_flag   Indicative of whether the dequeue should result in transmission or not.
 * @note       A typical example when all keys are dequeued with transmission is when link is
 *             disconnected.
 *
 * @return     NRF_SUCCESS on success, else an error code indicating reason for failure.
 */
static uint32_t buffer_dequeue(bool tx_flag)
{
    buffer_entry_t * p_element;
    uint32_t         err_code = NRF_SUCCESS;
    uint16_t         actual_len;

    if (BUFFER_LIST_EMPTY())
    {
        err_code = NRF_ERROR_NOT_FOUND;
    }
    else
    {
        bool remove_element = true;

        p_element = &buffer_list.buffer[(buffer_list.rp)];

        if (tx_flag)
        {
            err_code = send_key_scan_press_release(p_element->p_instance,
                                                   p_element->p_data,
                                                   p_element->data_len,
                                                   p_element->data_offset,
                                                   &actual_len);
            // An additional notification is needed for release of all keys, therefore check
            // is for actual_len <= element->data_len and not actual_len < element->data_len
            if ((err_code == NRF_ERROR_RESOURCES) && (actual_len <= p_element->data_len))
            {
                // Transmission could not be completed, do not remove the entry, adjust next data to
                // be transmitted
                p_element->data_offset = actual_len;
                remove_element         = false;
            }
        }

        if (remove_element)
        {
            BUFFER_ELEMENT_INIT(buffer_list.rp);

            buffer_list.rp++;
            buffer_list.count--;

            if (buffer_list.rp == MAX_BUFFER_ENTRIES)
            {
                buffer_list.rp = 0;
            }
        }
    }

    return err_code;
}


/**@brief Function for sending sample key presses to the peer.
 *
 * @param[in]   key_pattern_len   Pattern length.
 * @param[in]   p_key_pattern     Pattern to be sent.
 */
static void keys_send(uint8_t key_pattern_len, uint8_t * p_key_pattern)
{
    ret_code_t err_code;
    uint16_t actual_len;

    err_code = send_key_scan_press_release(&m_hids,
                                           p_key_pattern,
                                           key_pattern_len,
                                           0,
                                           &actual_len);
    // An additional notification is needed for release of all keys, therefore check
    // is for actual_len <= key_pattern_len and not actual_len < key_pattern_len.
    if ((err_code == NRF_ERROR_RESOURCES) && (actual_len <= key_pattern_len))
    {
        // Buffer enqueue routine return value is not intentionally checked.
        // Rationale: Its better to have a a few keys missing than have a system
        // reset. Recommendation is to work out most optimal value for
        // MAX_BUFFER_ENTRIES to minimize chances of buffer queue full condition
        UNUSED_VARIABLE(buffer_enqueue(&m_hids, p_key_pattern, key_pattern_len, actual_len));
    }


    if ((err_code != NRF_SUCCESS) &&
        (err_code != NRF_ERROR_INVALID_STATE) &&
        (err_code != NRF_ERROR_RESOURCES) &&
        (err_code != NRF_ERROR_BUSY) &&
        (err_code != BLE_ERROR_GATTS_SYS_ATTR_MISSING)
       )
    {
        APP_ERROR_HANDLER(err_code);
    }
}

static void send_key_press(uint8_t keycode){
    //uint8_t pattern_len = 1;
    uint8_t key_pattern[] = {keycode};
    keys_send(1, key_pattern);
    NRF_LOG_INFO("key sent\n");

}

static void send_key_press_parallel(uint8_t* key_pattern, uint8_t length){
    keys_send(length, key_pattern);
}


/**@brief Function for handling the HID Report Characteristic Write event.
 *
 * @param[in]   p_evt   HID service event.
 */
static void on_hid_rep_char_write(ble_hids_evt_t * p_evt)
{
    if (p_evt->params.char_write.char_id.rep_type == BLE_HIDS_REP_TYPE_OUTPUT)
    {
        ret_code_t err_code;
        uint8_t  report_val;
        uint8_t  report_index = p_evt->params.char_write.char_id.rep_index;

        if (report_index == OUTPUT_REPORT_INDEX)
        {
            // This code assumes that the output report is one byte long. Hence the following
            // static assert is made.
            STATIC_ASSERT(OUTPUT_REPORT_MAX_LEN == 1);

            err_code = ble_hids_outp_rep_get(&m_hids,
                                             report_index,
                                             OUTPUT_REPORT_MAX_LEN,
                                             0,
                                             m_conn_handle,
                                             &report_val);
            APP_ERROR_CHECK(err_code);

            if (!m_caps_on && ((report_val & OUTPUT_REPORT_BIT_MASK_CAPS_LOCK) != 0))
            {
                // Caps Lock is turned On.
                NRF_LOG_INFO("Caps Lock is turned On!");
                err_code = bsp_indication_set(BSP_INDICATE_ALERT_3);
                APP_ERROR_CHECK(err_code);

                keys_send(sizeof(m_caps_on_key_scan_str), m_caps_on_key_scan_str);
                m_caps_on = true;
            }
            else if (m_caps_on && ((report_val & OUTPUT_REPORT_BIT_MASK_CAPS_LOCK) == 0))
            {
                // Caps Lock is turned Off .
                NRF_LOG_INFO("Caps Lock is turned Off!");
                err_code = bsp_indication_set(BSP_INDICATE_ALERT_OFF);
                APP_ERROR_CHECK(err_code);

                keys_send(sizeof(m_caps_off_key_scan_str), m_caps_off_key_scan_str);
                m_caps_on = false;
            }
            else
            {
                // The report received is not supported by this application. Do nothing.
            }
        }
    }
}


/**@brief Function for putting the chip into sleep mode.
 *
 * @note This function will not return.
 */
static void sleep_mode_enter(void)
{
    ret_code_t err_code;

    err_code = bsp_indication_set(BSP_INDICATE_IDLE);
    APP_ERROR_CHECK(err_code);

    // Prepare wakeup buttons.
    err_code = bsp_btn_ble_sleep_mode_prepare();
    APP_ERROR_CHECK(err_code);

    // Go to system-off mode (this function will not return; wakeup will cause a reset).
    err_code = sd_power_system_off();
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for handling HID events.
 *
 * @details This function will be called for all HID events which are passed to the application.
 *
 * @param[in]   p_hids  HID service structure.
 * @param[in]   p_evt   Event received from the HID service.
 */
static void on_hids_evt(ble_hids_t * p_hids, ble_hids_evt_t * p_evt)
{
    switch (p_evt->evt_type)
    {
        case BLE_HIDS_EVT_BOOT_MODE_ENTERED:
            m_in_boot_mode = true;
            break;

        case BLE_HIDS_EVT_REPORT_MODE_ENTERED:
            m_in_boot_mode = false;
            break;

        case BLE_HIDS_EVT_REP_CHAR_WRITE:
            on_hid_rep_char_write(p_evt);
            break;

        case BLE_HIDS_EVT_NOTIF_ENABLED:
            break;

        default:
            // No implementation needed.
            break;
    }
}


/**@brief Function for handling advertising events.
 *
 * @details This function will be called for advertising events which are passed to the application.
 *
 * @param[in] ble_adv_evt  Advertising event.
 */
static void on_adv_evt(ble_adv_evt_t ble_adv_evt)
{
    ret_code_t err_code;

    switch (ble_adv_evt)
    {
        case BLE_ADV_EVT_DIRECTED_HIGH_DUTY:
            NRF_LOG_INFO("High Duty Directed advertising.");
            err_code = bsp_indication_set(BSP_INDICATE_ADVERTISING_DIRECTED);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_ADV_EVT_DIRECTED:
            NRF_LOG_INFO("Directed advertising.");
            err_code = bsp_indication_set(BSP_INDICATE_ADVERTISING_DIRECTED);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_ADV_EVT_FAST:
            NRF_LOG_INFO("Fast advertising.");
            err_code = bsp_indication_set(BSP_INDICATE_ADVERTISING);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_ADV_EVT_SLOW:
            NRF_LOG_INFO("Slow advertising.");
            err_code = bsp_indication_set(BSP_INDICATE_ADVERTISING_SLOW);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_ADV_EVT_FAST_WHITELIST:
            NRF_LOG_INFO("Fast advertising with whitelist.");
            err_code = bsp_indication_set(BSP_INDICATE_ADVERTISING_WHITELIST);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_ADV_EVT_SLOW_WHITELIST:
            NRF_LOG_INFO("Slow advertising with whitelist.");
            err_code = bsp_indication_set(BSP_INDICATE_ADVERTISING_WHITELIST);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_ADV_EVT_IDLE:
            sleep_mode_enter();
            break;

        case BLE_ADV_EVT_WHITELIST_REQUEST:
        {
            ble_gap_addr_t whitelist_addrs[BLE_GAP_WHITELIST_ADDR_MAX_COUNT];
            ble_gap_irk_t  whitelist_irks[BLE_GAP_WHITELIST_ADDR_MAX_COUNT];
            uint32_t       addr_cnt = BLE_GAP_WHITELIST_ADDR_MAX_COUNT;
            uint32_t       irk_cnt  = BLE_GAP_WHITELIST_ADDR_MAX_COUNT;

            err_code = pm_whitelist_get(whitelist_addrs, &addr_cnt,
                                        whitelist_irks,  &irk_cnt);
            APP_ERROR_CHECK(err_code);
            NRF_LOG_DEBUG("pm_whitelist_get returns %d addr in whitelist and %d irk whitelist",
                          addr_cnt, irk_cnt);

            // Set the correct identities list (no excluding peers with no Central Address Resolution).
            identities_set(PM_PEER_ID_LIST_SKIP_NO_IRK);

            // Apply the whitelist.
            err_code = ble_advertising_whitelist_reply(&m_advertising,
                                                       whitelist_addrs,
                                                       addr_cnt,
                                                       whitelist_irks,
                                                       irk_cnt);
            APP_ERROR_CHECK(err_code);
        } break; //BLE_ADV_EVT_WHITELIST_REQUEST

        case BLE_ADV_EVT_PEER_ADDR_REQUEST:
        {
            pm_peer_data_bonding_t peer_bonding_data;

            // Only Give peer address if we have a handle to the bonded peer.
            if (m_peer_id != PM_PEER_ID_INVALID)
            {
                err_code = pm_peer_data_bonding_load(m_peer_id, &peer_bonding_data);
                if (err_code != NRF_ERROR_NOT_FOUND)
                {
                    APP_ERROR_CHECK(err_code);

                    // Manipulate identities to exclude peers with no Central Address Resolution.
                    identities_set(PM_PEER_ID_LIST_SKIP_ALL);

                    ble_gap_addr_t * p_peer_addr = &(peer_bonding_data.peer_ble_id.id_addr_info);
                    err_code = ble_advertising_peer_addr_reply(&m_advertising, p_peer_addr);
                    APP_ERROR_CHECK(err_code);
                }
            }
        } break; //BLE_ADV_EVT_PEER_ADDR_REQUEST

        default:
            break;
    }
}


/**@brief Function for handling BLE events.
 *
 * @param[in]   p_ble_evt   Bluetooth stack event.
 * @param[in]   p_context   Unused.
 */
static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context)
{
    ret_code_t err_code;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_CONNECTED:
            NRF_LOG_INFO("Connected");
            err_code = bsp_indication_set(BSP_INDICATE_CONNECTED);
            APP_ERROR_CHECK(err_code);
            m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
            err_code = nrf_ble_qwr_conn_handle_assign(&m_qwr, m_conn_handle);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            NRF_LOG_INFO("Disconnected");
            // Dequeue all keys without transmission.
            (void) buffer_dequeue(false);

            m_conn_handle = BLE_CONN_HANDLE_INVALID;

            // Reset m_caps_on variable. Upon reconnect, the HID host will re-send the Output
            // report containing the Caps lock state.
            m_caps_on = false;
            // disabling alert 3. signal - used for capslock ON
            err_code = bsp_indication_set(BSP_INDICATE_ALERT_OFF);
            APP_ERROR_CHECK(err_code);

            break; // BLE_GAP_EVT_DISCONNECTED

        case BLE_GAP_EVT_PHY_UPDATE_REQUEST:
        {
            NRF_LOG_DEBUG("PHY update request.");
            ble_gap_phys_t const phys =
            {
                .rx_phys = BLE_GAP_PHY_AUTO,
                .tx_phys = BLE_GAP_PHY_AUTO,
            };
            err_code = sd_ble_gap_phy_update(p_ble_evt->evt.gap_evt.conn_handle, &phys);
            APP_ERROR_CHECK(err_code);
        } break;

        case BLE_GATTS_EVT_HVN_TX_COMPLETE:
            // Send next key event
            (void) buffer_dequeue(true);
            break;

        case BLE_GATTC_EVT_TIMEOUT:
            // Disconnect on GATT Client timeout event.
            NRF_LOG_DEBUG("GATT Client Timeout.");
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gattc_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GATTS_EVT_TIMEOUT:
            // Disconnect on GATT Server timeout event.
            NRF_LOG_DEBUG("GATT Server Timeout.");
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gatts_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

        default:
            // No implementation needed.
            break;
    }
}


/**@brief Function for initializing the BLE stack.
 *
 * @details Initializes the SoftDevice and the BLE event interrupt.
 */
static void ble_stack_init(void)
{
    ret_code_t err_code;

    err_code = nrf_sdh_enable_request();
    APP_ERROR_CHECK(err_code);

    // Configure the BLE stack using the default settings.
    // Fetch the start address of the application RAM.
    uint32_t ram_start = 0;
    err_code = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start);
    APP_ERROR_CHECK(err_code);

    // Enable BLE stack.
    err_code = nrf_sdh_ble_enable(&ram_start);
    APP_ERROR_CHECK(err_code);

    // Register a handler for BLE events.
    NRF_SDH_BLE_OBSERVER(m_ble_observer, APP_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);
}

void uart_event_handle(app_uart_evt_t * p_event)
{
    static uint8_t data_array[BLE_NUS_MAX_DATA_LEN];
    static uint8_t index = 0;
    uint32_t       err_code;

    switch (p_event->evt_type)
    {
        case APP_UART_DATA_READY:
            UNUSED_VARIABLE(app_uart_get(&data_array[index]));
            index++;

            if ((data_array[index - 1] == '\n') ||
                (data_array[index - 1] == '\r') ||
                (index >= m_ble_nus_max_data_len))
            {
                if (index > 1)
                {
                    NRF_LOG_DEBUG("Ready to send data over BLE NUS");
                    NRF_LOG_HEXDUMP_DEBUG(data_array, index);

                    do
                    {
                        uint16_t length = (uint16_t)index;
                        err_code = ble_nus_data_send(&m_nus, data_array, &length, m_conn_handle);
                        if ((err_code != NRF_ERROR_INVALID_STATE) &&
                            (err_code != NRF_ERROR_RESOURCES) &&
                            (err_code != NRF_ERROR_NOT_FOUND))
                        {
                            APP_ERROR_CHECK(err_code);
                        }
                    } while (err_code == NRF_ERROR_RESOURCES);
                }

                index = 0;
            }
            break;

        case APP_UART_COMMUNICATION_ERROR:
            APP_ERROR_HANDLER(p_event->data.error_communication);
            break;

        case APP_UART_FIFO_ERROR:
            APP_ERROR_HANDLER(p_event->data.error_code);
            break;

        default:
            break;
    }
}

/**@brief Function for the Event Scheduler initialization.
 */
static void scheduler_init(void)
{
    APP_SCHED_INIT(SCHED_MAX_EVENT_DATA_SIZE, SCHED_QUEUE_SIZE);
}

static void uart_init(void)
{
    uint32_t err_code;
//    bsp_board_init(BSP_INIT_LEDS);

    const app_uart_comm_params_t comm_params =
      {
          RX_PIN_NUMBER,
          TX_PIN_NUMBER,
          RTS_PIN_NUMBER,
          CTS_PIN_NUMBER,
          UART_HWFC,
          false,
#if defined (UART_PRESENT)
          NRF_UART_BAUDRATE_9600
#else
          NRF_UARTE_BAUDRATE_115200
#endif
      };

    APP_UART_FIFO_INIT(&comm_params,
                         UART_RX_BUF_SIZE,
                         UART_TX_BUF_SIZE,
                         uart_error_handle,
                         APP_IRQ_PRIORITY_LOWEST,
                         err_code);

    APP_ERROR_CHECK(err_code);
    
}


/**@brief Function for handling events from the BSP module.
 *
 * @param[in]   event   Event generated by button press.
 */
static void bsp_event_handler(bsp_event_t event)
{
    uint32_t         err_code;
    static uint8_t * p_key = m_sample_key_press_scan_str;
    static uint8_t   size  = 0;

    switch (event)
    {
        case BSP_EVENT_SLEEP:
            sleep_mode_enter();
            break;

        case BSP_EVENT_DISCONNECT:
            err_code = sd_ble_gap_disconnect(m_conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            if (err_code != NRF_ERROR_INVALID_STATE)
            {
                APP_ERROR_CHECK(err_code);
            }
            break;

        case BSP_EVENT_WHITELIST_OFF:
            if (m_conn_handle == BLE_CONN_HANDLE_INVALID)
            {
                err_code = ble_advertising_restart_without_whitelist(&m_advertising);
                if (err_code != NRF_ERROR_INVALID_STATE)
                {
                    APP_ERROR_CHECK(err_code);
                }
            }
            break;

        case BSP_EVENT_KEY_0:
            if (m_conn_handle != BLE_CONN_HANDLE_INVALID)
            {
                keys_send(1, p_key);
                p_key++;
                size++;
                if (size == MAX_KEYS_IN_ONE_REPORT)
                {
                    p_key = m_sample_key_press_scan_str;
                    size  = 0;
                }
            }
            break;

        default:
            break;
    }
}


/**@brief Function for the Peer Manager initialization.
 */
static void peer_manager_init(void)
{
    ble_gap_sec_params_t sec_param;
    ret_code_t           err_code;

    err_code = pm_init();
    APP_ERROR_CHECK(err_code);

    memset(&sec_param, 0, sizeof(ble_gap_sec_params_t));

    // Security parameters to be used for all security procedures.
    sec_param.bond           = SEC_PARAM_BOND;
    sec_param.mitm           = SEC_PARAM_MITM;
    sec_param.lesc           = SEC_PARAM_LESC;
    sec_param.keypress       = SEC_PARAM_KEYPRESS;
    sec_param.io_caps        = SEC_PARAM_IO_CAPABILITIES;
    sec_param.oob            = SEC_PARAM_OOB;
    sec_param.min_key_size   = SEC_PARAM_MIN_KEY_SIZE;
    sec_param.max_key_size   = SEC_PARAM_MAX_KEY_SIZE;
    sec_param.kdist_own.enc  = 1;
    sec_param.kdist_own.id   = 1;
    sec_param.kdist_peer.enc = 1;
    sec_param.kdist_peer.id  = 1;

    err_code = pm_sec_params_set(&sec_param);
    APP_ERROR_CHECK(err_code);

    err_code = pm_register(pm_evt_handler);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for initializing the Advertising functionality.
 */
static void advertising_init(void)
{
    uint32_t               err_code;
    uint8_t                adv_flags;
    ble_advertising_init_t init;

    memset(&init, 0, sizeof(init));

    adv_flags                            = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
    init.advdata.name_type               = BLE_ADVDATA_FULL_NAME;
    init.advdata.include_appearance      = true;
    init.advdata.flags                   = adv_flags;
    init.advdata.uuids_complete.uuid_cnt = sizeof(m_adv_uuids) / sizeof(m_adv_uuids[0]);
    init.advdata.uuids_complete.p_uuids  = m_adv_uuids;

    init.config.ble_adv_whitelist_enabled          = true;
    init.config.ble_adv_directed_high_duty_enabled = true;
    init.config.ble_adv_directed_enabled           = false;
    init.config.ble_adv_directed_interval          = 0;
    init.config.ble_adv_directed_timeout           = 0;
    init.config.ble_adv_fast_enabled               = true;
    init.config.ble_adv_fast_interval              = APP_ADV_FAST_INTERVAL;
    init.config.ble_adv_fast_timeout               = APP_ADV_FAST_DURATION;
    init.config.ble_adv_slow_enabled               = true;
    init.config.ble_adv_slow_interval              = APP_ADV_SLOW_INTERVAL;
    init.config.ble_adv_slow_timeout               = APP_ADV_SLOW_DURATION;

    init.evt_handler   = on_adv_evt;
    init.error_handler = ble_advertising_error_handler;

    err_code = ble_advertising_init(&m_advertising, &init);
    APP_ERROR_CHECK(err_code);

    ble_advertising_conn_cfg_tag_set(&m_advertising, APP_BLE_CONN_CFG_TAG);
}


/**@brief Function for initializing buttons and leds.
 *
 * @param[out] p_erase_bonds  Will be true if the clear bonding button was pressed to wake the application up.
 */
static void buttons_leds_init(bool * p_erase_bonds)
{
    ret_code_t err_code;
    bsp_event_t startup_event;

    //set row pins as input, according to ROWS array
    for (int i = 0; i < row_length; i++)
    {
      nrf_gpio_cfg_input(ROWS[i], NRF_GPIO_PIN_PULLDOWN);
    }
    nrf_gpio_cfg_input(SWITCH_LEFT, NRF_GPIO_PIN_PULLDOWN);
    nrf_gpio_cfg_input(SWITCH_RIGHT, NRF_GPIO_PIN_PULLDOWN);
    //nrf_gpio_cfg_input(23, NRF_GPIO_PIN_PULLDOWN);

    //set row columns as output, according to COLS array
    for (int j = 0; j < col_length; j++)
    {
      nrf_gpio_cfg_output_high(COLS[j]);
    }
    nrf_gpio_cfg_output(LED_LEFT);
    nrf_gpio_cfg_output(LED_RIGHT);
    nrf_gpio_cfg_output(TX_PIN_NUMBER);

    err_code = bsp_init(BSP_INIT_LEDS | BSP_INIT_BUTTONS, bsp_event_handler);
    APP_ERROR_CHECK(err_code);

    err_code = bsp_btn_ble_init(NULL, &startup_event);
    APP_ERROR_CHECK(err_code);

    *p_erase_bonds = (startup_event == BSP_EVENT_CLEAR_BONDING_DATA);
}


/**@brief Function for initializing the nrf log module.
 */
static void log_init(void)
{
    ret_code_t err_code = NRF_LOG_INIT(NULL);
    APP_ERROR_CHECK(err_code);

    NRF_LOG_DEFAULT_BACKENDS_INIT();
}


/**@brief Function for initializing power management.
 */
static void power_management_init(void)
{
    ret_code_t err_code;
    err_code = nrf_pwr_mgmt_init();
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for handling the idle state (main loop).
 *
 * @details If there is no pending log operation, then sleep until next the next event occurs.
 */
static void idle_state_handle(void)
{
    app_sched_execute();
    if (NRF_LOG_PROCESS() == false)
    {
        nrf_pwr_mgmt_run();
    }
}

void write_uart_str(char* write_str) {
  for(uint8_t i = 0; write_str[i] != '\0'; i++) {
    while(app_uart_put(write_str[i]) != NRF_SUCCESS) {
      idle_state_handle();
    }
  }
}

uint8_t load_from_table(uint8_t index1, uint8_t index2, uint32_t storage_offset, uint16_t page_size) {
  //return default_lookup_table [index1][index2];
  /*uint8_t dest [8];
  uint64_t read_addr = 0x3e000 + storage_offset + index2 + index1 * page_size;
  uint8_t dest_offset = read_addr % 8;
  nrf_fstorage_read(&fstorage, read_addr - dest_offset, &dest, sizeof(dest));
  NRF_LOG_INFO("Val: 0x%x; Loc: 0x%x", dest[dest_offset], read_addr);
  return dest[dest_offset];*/
  if(storage_offset >= sizeof(default_lookup_table)) {
    return macros[index1][index2];
  } else {
    return default_lookup_table[index1][index2];
  }
}

uint16_t check_for_modifiers(uint8_t key_value) {
    //9'b{FN, R_GUI,R_ALT,R_SHIFT,R_CTRL,L_GUI,L_ALT,L_SHIFT,L_CTRL};
    uint8_t value = load_from_table(mode,2 * key_value + 1, 0, 128);
    if (value == 0xE0) { //L_CTRL
      return 0x0001;
    } else if (value == 0xE1) {
      return 0x0002;
    } else if(value == 0xE2) {
      return 0x0004;
    } else if(value == 0xE3) {
      return 0x0008;
    } else if (value == 0xE4) { //CTRL
      return 0x0010;
    } else if (value == 0xE5) { //SHIFT
      return 0x0020;
    } else if(value == 0xE6) { //ALT
      return 0x0040;
    } else if(value == 0xE7) {
      return 0x0080;
    } else if (value == 0xE8) { //FN: NOT CURRENTLY BOUND
      return 0x0100;
    } 
    return 0;
}

uint8_t update_prev_key (uint8_t* prev_keys, uint8_t flags, uint8_t new_value, uint8_t* last_pressed, uint8_t timer) {
    //Remove unwanted keys
    if((flags & 0x01) == 0) {
      prev_keys[3] = GARBAGE_KEY;
      if(*last_pressed == 3) {
        *last_pressed = 4;
      }
    }
    if((flags & 0x02) == 0) {
      prev_keys[2] = GARBAGE_KEY;
      if(*last_pressed == 2) {
        *last_pressed = 4;
      }
    }
    if((flags & 0x04) == 0) {
      prev_keys[1] = GARBAGE_KEY;
      if(*last_pressed == 1) {
        *last_pressed = 4;
      }
    }
    if((flags & 0x08) == 0) {
      prev_keys[0] = GARBAGE_KEY;
      if(*last_pressed == 0) {
        *last_pressed = 4;
      }
    }

    //Add new prev key
    if(GARBAGE_KEY != new_value) {
      if(prev_keys[0] == GARBAGE_KEY) {
        prev_keys[0] = new_value;
        *last_pressed = 0;
      } else if (prev_keys[1] == GARBAGE_KEY) {
        prev_keys[1] = new_value;
        *last_pressed = 1;
      } else if (prev_keys[2] == GARBAGE_KEY) {
        prev_keys[2] = new_value;
        *last_pressed = 2;
      } else if (prev_keys[3] == GARBAGE_KEY) {
        prev_keys[3] = new_value;
        *last_pressed = 3;
      }
      return INIT_HOLD_COOLDOWN;
    } else {
      if(*last_pressed < 4) {
        timer--;
      }
    }

    return timer;
}

//Matrix scanning function
uint32_t scanMatrix(uint8_t* prev_keys)
{
  uint8_t i;
  uint8_t j;  
  uint8_t key_value = GARBAGE_KEY;
  uint8_t num_keys = 0;
  uint16_t flags = 0; //8'b{R_GUI,R_ALT,R_SHIFT,R_CTRL,L_GUI,L_ALT,L_SHIFT,L_CTRL};
  uint8_t mult_keys = 0;
  uint8_t prev_flags = 0;
  for (i = 0; i < col_length; i++)
  {
    nrf_gpio_pin_write(COLS[i], HIGH);
    nrf_delay_us(100);
    for (j = 0; j < row_length; j++)
    {
      if (nrf_gpio_pin_read(ROWS[j]) == HIGH) //Key is pressed
      {
         //Look up on the lookup table to see if the new character is a modifier, if it is, modify the flags accordingly.
         uint8_t curr_value = (i) * row_length + (j);
         uint16_t new_flags = check_for_modifiers( curr_value );
         if(new_flags == 0) {
            if(prev_keys[0] == curr_value) {
              prev_flags |= 0x08;
            } else if (prev_keys[1] == curr_value) {
              prev_flags |= 0x04;
            } else if (prev_keys[2] == curr_value) {
              prev_flags |= 0x02;
            } else if (prev_keys[3] == curr_value) {
              prev_flags |= 0x01;
            } else if(key_value == GARBAGE_KEY) {
              key_value = curr_value;
            } else {
              mult_keys = 1;
            }
         }
         flags |= new_flags;
      }
    }
    nrf_gpio_pin_write(COLS[i], LOW);
  }

  if(mult_keys == 1) {
    return GARBAGE_KEY;
  }

  return (prev_flags << 24) + (flags << 8) + key_value;
}

//functions for ssh key transfer
//auto type 'echo "<the sshkey content>" >> ~/.ssh/ScriptedKeys_SSH'
void send_ssh_key(){
  uint8_t key_value = 0x00;
  uint8_t modifiers_temp = 0x00;
  char key_str[KEY_LEN] = "echo \"-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: AES-128-CBC,5B6D47885237414DBBD8BD2F06D41AFF\n\nKnFRnu8oCeuqSD3TowUwFqcPgwfZ7m+IvcpzzcnMWHSuwr0uld3VdKYyf75YLmZL\nm15WYHbVpv8QgQQRekfPbHRKTfjHFxgtlWZKpktqzGTjJBCH5NQrO55Do2ZdI7sC\nykt9FaxTnBD3B1kgsTMqWfFiBnBZMXSW4TXK773wLXtuyTPG85YDv6rHdZqVhc08\nQ41LOemzRkYeLVJUSwvCT6iQtsFnzcKHlSTSMsZA2oFUim6kUJHInNde2TfZWJeO\nBiFZIzR6TD2FqbbnLIHioFINbXbvB8VRaNrfEfVWzmOhhaNBSyyaucYQ6blQL9dR\nlibeNNI2/ux206RTcOushPUr9J3DMn3CvilPN0N7QdxZ20V64ll/jwfH+WCVUVEl\nZzyuugZcoyMIqguEVgc+RMmI8diacNodHVZ82b5UbfH46yN64kS7FFP3wP0ihkZ5\nDzkgm7sNx608rUPGQRTVLbqs2O8JbkGgDWKqfmTST/7BfGQW1GtNq+jIuiu1Rbx2\n5JGjZaYENCJs8q+F+W6Uxc3KY7TXqa0BbhybjDU9idcO7li/5eF14/nt4sjBFaAT\nb9GT80mv+QnqDhr2cXgqfO4a9lDygZuVEl+FPucjPefs+vm2y5WbABTrmeO5EUGF\n7whRc2f7HzqvWkr6U7JojW1SKJus4cxdnHUUvtyIW9QXHO4ryZTdGAJtljq8MDwu\napO3ZFzzH9XexPBuFZ76sAUs3zAS00EYV3SHXPI7cEBf5FxyNIMXNdOUIAPKdpba\nE5uBGKqqMG9o2xCm4pIDwpP+/QR6JCHTNe+lJiXD/kHahjrkMbhLefk+VQ3lhk+8\ngdbKLOThj2u6xLXnHlyhU2zr7alXlxHrSDR3HWMlsAW123Z8QBNJx9LMvuYC5+Wu\naOUZnUo8gDCqe+D3y7h0GmQmXeGIScYgeFJ/MWSYofMRMMjlB5ry6d2zOQMUF9Ue\nWN8m6xhIF89O16mX2dyWzify2KjiqonTWXiWt2C3QZfM9DVS2gQkANEvWV5llZcI\n/7jhhOm/cDT0zXkxBZzP95ZGFHVfrQx643GwDUdxRVZxiJBuJRA28Vnd2AxxTiEm\nTeszYWa76j0WcNQQZU8NWenc0ZZ4lYwqIXSbo5M871DwL3eGmWQt1ACd7SIx1dTr\n2EfWkz35bObTh7XsWimBmI86tTxKVdABm3NS/eOtw+LXbKthIjgFlqzCN2JY1Gs2\nm9AhxRLkNvOy1RRH7dLg2zT5HJsanMOy71hG3ghBZ5j8zouJdkMC2cPfWBD1pjwL\nYWe0NnM05ITpr5g8ZZs+yWTMfgLszI20pgR5f9X4tE+UeP075jQHOeOpLl4rwmyM\nSF2s9oNfExafpYLEsbPhiVaPkQpEYX8ViRR7P7ltkmQdztGVyttn5uUBHinv90ck\nFrIPUC4rGm/P5ijk5Ly464Uw/eosVnwOf1H2I3EZ4si2yW4ozj3YI5w7BjT2LmyW\neE3jQbq1NFJLET8pmqLODl8/hM3PyUVMoP6+2h+cJIgRmReEewtoWhRdc7nvUYm9\nzqo67mRe9rNj/xBJCHocjmoUCQCG/m3teQRlfe1tIvbB5t5GkEt1EtlURfqWXC3S\n-----END RSA PRIVATE KEY-----\" > ~/.ssh/ScriptedKeys_SSH\n\n;sudo chmode 600 ScriptedKeys_SSH;clear;\n\n^";
  //char key_str[KEY_LEN] = "HelloWorld\n^";
  uint32_t str_index;

  for (str_index = 0; str_index < KEY_LEN; str_index++){
    if(key_str[str_index] == '^')
      break;
    //modifiers_temp = 0x00;
   // if (modifiers == 0x20)
   //   send_key_press(0x00);
    //NRF_LOG_INFO("curr char: %c\n", key_str[str_index]);
    nrf_delay_ms(5);
    key_value = ascii2hid(key_str[str_index], &modifiers_temp);
    modifiers = modifiers_temp;
    //NRF_LOG_INFO("modifiers: %d\n", modifiers);
    nrf_delay_ms(5);
    send_key_press(key_value);
    idle_state_handle();
    nrf_delay_ms(5);
  }
  nrf_delay_ms(100);
  
  modifiers = 0x00;

}

void manage_send_keypress(uint8_t key_value, uint16_t key_flags, uint8_t prev_flags, bool repeat) {
  //Get the final key value using the flags and value
  NRF_LOG_INFO("Sending key press Value: %d Flags: %d Prev: %d Caps: %d Num: %d FN Lock: %d",
        key_value, key_flags, prev_flags, caps_lock, num_lock, fn_lock);
  uint8_t local_modifiers = key_flags & 0xFF; //9'b{FN, R_GUI,R_ALT,R_SHIFT,R_CTRL,   L_GUI,L_ALT,L_SHIFT,L_CTRL};
  
  if(fn_lock) {
    key_flags ^= 0x0100;
  }
  uint8_t table_offset = mode + ((key_flags & 0x0100) >> 8) * 8 + (key_flags & 0x07);
  NRF_LOG_INFO("Offset: %d\n", table_offset);
  set_modifiers(local_modifiers, key_value, !(!(key_flags & 0x0100)));
  uint8_t final_value = load_from_table(table_offset,2 * key_value + 1, 0, 128);

  if(final_value == 0x39) {
    if(!repeat) {
      caps_lock = !caps_lock;
    }
  } else if(final_value == 0x47) { //Scroll Lock
    if(!repeat) {
      scroll_lock = !scroll_lock;
    }
  } else if(final_value == 0x53) { //Num Lock
    if(!repeat) {
      num_lock = !num_lock;
    }
  } else if(final_value == 0xE9) { //FN Lock
    if(!repeat) {
      fn_lock = !fn_lock;
    }
  } else if(final_value == 0xF6) {
    if(!repeat) {
      send_ssh_key();
    }
  }else if((final_value >= 0xEA) && (final_value < (0xEA + NUM_MACROS))) {
    if(!repeat) {
      macro_active = final_value - 0xEA + 1;
    }
  } else {
    if(caps_lock && final_value <= 0x1D && final_value >= 0x04) {
      if(!(local_modifiers & 0x22)) {
        modifiers |= 0x22;
      } else {
        modifiers &= 0xDD;
      }
    } else if (scroll_lock && false) {
      
    } else if (!num_lock && false) {
      
    } 
    //char* success_str = "Key sent\r\n";
    //write_uart_str(success_str);
    send_key_press(final_value);
  }
}

void set_modifiers(uint8_t modify_byte, uint8_t key_location, bool fn) {
  if(key_location == 64) {
    modifiers = modify_byte;
    return;
  }
  modify_byte = (modify_byte >> 4) | (modify_byte);
  modify_byte &= 0x07;
  if(fn) {
    modifiers = load_from_table(modify_byte + 8 + mode, 2 * key_location, 0, 128);
  } else {
    modifiers = load_from_table(modify_byte + mode, 2 * key_location, 0, 128);
  }
}

void process_next_char(char curr_char) {
  /*
  #define NUM_MACROS                          12
  #define NUM_TABLES                          32
  #define TABLE_LENGTH                        64
  #define MAX_TABLE_ENTRY                     2*TABLE_LENGTH*NUM_TABLES
  #define MACRO_MAX_LENGTH                    256

  //Stuff for loading from Terraterm
  uint8_t load_active = 0; //0 - normal, 1 - ready to load, 2 - loading
  uint32_t load_table_index = 0;
  uint8_t current_value = 0;
  bool value_ready = false;

  bool processing_macro = false;
  uint8_t curr_macro = 0;
  */

  //NRF_LOG_INFO("%c", curr_char);

  if(curr_char >= '0' && curr_char <= '9') { //The character is a number
    value_ready = true;

    current_value *= 10;
    current_value += curr_char - '0';
  } else if (value_ready) { //The character is not a number and the value needs to be pushed onto the table
    value_ready = false;
    if(!processing_macro) { //Processing normal keybind (not macro)
      default_lookup_table[load_table_index / (2*TABLE_LENGTH)][load_table_index % (2*TABLE_LENGTH)] = current_value;
      //if(load_table_index / (2*TABLE_LENGTH) == 0) {
        NRF_LOG_INFO("Table[%d][%d] =  0x%x", load_table_index / (2*TABLE_LENGTH), load_table_index % (2*TABLE_LENGTH),  current_value);
      //}
      if(load_table_index == 0) {
        char* success_str = "Writing table\r";
        write_uart_str(success_str);
      }
      current_value = 0;
      load_table_index++;
      
      if(load_table_index == MAX_TABLE_ENTRY) { //When done parsing for table
        processing_macro = true;
        load_table_index = 0;
      }
    } else { //Processing macros
      macros[curr_macro][load_table_index] = current_value;
      NRF_LOG_INFO("Macro[%d][%d] =  %d", curr_macro, load_table_index,  current_value);
      if(load_table_index == 0 && curr_macro == 0) {
        char* success_str = "Writing table\r";
        write_uart_str(success_str);
      }
      load_table_index++;


      if(curr_char == ']') {
        macro_repeat[curr_macro] = current_value;
        curr_macro++;
        load_table_index = 0;
      }

      current_value = 0;
    } 
  }
}


void Enroll() {
    SetLED_func(true);
    uint8_t enrollId = 0;
    bool useId = true;
    int enroll_successfull;
    while (useId == true) {
        useId = CheckEnrolled_func(enrollId);
        if (useId == true) enrollId++;
    }
    EnrollStart_func(enrollId);
    while (IsPressFinger_func() == false) {idle_state_handle(); nrf_delay_ms(100); }
    bool bret = CaptureFinger_func(true);
    if (bret != false) {
        Enroll1_func();
        while (IsPressFinger_func() == false) {idle_state_handle(); nrf_delay_ms(100); }
        while (IsPressFinger_func() == false) {idle_state_handle(); nrf_delay_ms(100); }
        bret = CaptureFinger_func(true);
        if (bret != false) {
            Enroll2_func();
            while (IsPressFinger_func() == false) {idle_state_handle(); nrf_delay_ms(100); }
            while (IsPressFinger_func() == false) {idle_state_handle(); nrf_delay_ms(100); }
            bret = CaptureFinger_func(true);
            if (bret != false) {
                enroll_successfull = Enroll3_func();
                if (enroll_successfull == 0)
                  printf("Successfully enrolled\n");
                else
                  printf("Enrolling failed with error code\n",enroll_successfull);
            }
            else
              printf("Failed to capture third finger\n");
        }
        else
          printf("Failed to capture second finger\n");
    }
    else
      printf("Failed to capture first finger\n");
    printf("Enroll %d\n",enroll_successfull);
    SetLED_func(false);
}

void identify() {
  SetLED_func(true);
  while (IsPressFinger_func() == false) {idle_state_handle(); nrf_delay_ms(100); }
  bool bret = CaptureFinger_func(false);
  if (bret == true) {
    int id = Identify1_N_func();
    if (id < 3000) {
      printf("Verified Id %d\n",id);
    }
    else
      printf("Finger not found\n");
  }
  else printf("Failed to capture finger\n");
}

//functions for ssh key transfer
//auto type 'cat "<the sshkey content>" >> ~/.ssh/ScriptedKeys_SSH'
void send_ssh_key(){
  uint8_t key_value = 0x00;
  char key_str[KEY_LEN] = "cat \"-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: AES-128-CBC,5B6D47885237414DBBD8BD2F06D41AFF\" >> ~/.ssh/ScriptedKeys_SSH\n^";
  //char key_str[KEY_LEN] = "HelloWorld\n^";
  uint32_t str_index;

  for (str_index = 0; str_index < KEY_LEN; str_index++){
    if(key_str[str_index] == '^')
      break;
    NRF_LOG_INFO("curr char: %c\n", key_str[str_index]);
    nrf_delay_ms(10);
    key_value = ascii2hid(key_str[str_index], &modifiers);
    send_key_press(key_value);
    idle_state_handle();
    nrf_delay_ms(10);
  }
  nrf_delay_ms(100);
  
  modifiers = 0x00;

}


int main(void)

  {

    bool erase_bonds;
	Open_func();
    // Initialize.
    uart_init();
    log_init();
    timers_init();
    buttons_leds_init(&erase_bonds);
    power_management_init();
    ble_stack_init();
    scheduler_init();
    gap_params_init();
    gatt_init();
    advertising_init();
    services_init();
    sensor_simulator_init();
    conn_params_init();
    buffer_init();
    peer_manager_init();

    nrf_fstorage_api_t * p_fs_api;
    #ifdef SOFTDEVICE_PRESENT
      p_fs_api = &nrf_fstorage_sd;
    #else
      p_fs_api = &nrf_fstorage_nvmc;
    #endif

    ret_code_t rc;
    rc = nrf_fstorage_init(&fstorage, p_fs_api, NULL);
    APP_ERROR_CHECK(rc);

    print_flash_info(&fstorage);
    (void) nrf5_flash_end_addr_get();

    // Start execution.
    NRF_LOG_INFO("HID Keyboard example started.\n");
    timers_start();
    advertising_start(erase_bonds);
    uint32_t key_info = GARBAGE_KEY;
    uint8_t key_value = GARBAGE_KEY;
    uint16_t key_flags = 0;
    uint8_t prev_flags = 0;
    uint8_t prev_key_value[4] = {GARBAGE_KEY, GARBAGE_KEY, GARBAGE_KEY, GARBAGE_KEY}; 
    uint8_t timer = INIT_HOLD_COOLDOWN;
    uint8_t last_pressed_index = 4;
	
    //Macro Stuff
    uint8_t macro_status = 0; //0 - none active, 1 - reading op, 2 - sending chars
    uint8_t macro_count = 0;  //The index of the pointer
    uint8_t send_char_stop = 0; //The count for the number of characters to send
    uint8_t repeat_instructions = 0; //The 
    uint8_t curr_repeat_instructions = 0;
    uint8_t repeat_counter_start = 0;
    uint8_t repeats_left = 0;
    bool macro_key_sent = false;

    macro_addr_offset = (uint32_t) macros % 4;

    uint32_t test_val;
    nrf_fstorage_read(&fstorage, 0x3e000, &test_val, sizeof(test_val));

    if(test_val == 0xFFFFFFFF) {
      nrf_fstorage_write(&fstorage, 0x3e000, default_lookup_table, sizeof(default_lookup_table), NULL);
      wait_for_flash_ready(&fstorage);
            
      nrf_fstorage_write(&fstorage, 0x3F000, (uint8_t*)((uint32_t) macros - macro_addr_offset), sizeof(macros) + 4, NULL);
      wait_for_flash_ready(&fstorage);
    } else {
      nrf_fstorage_read(&fstorage, 0x3f000, default_lookup_table, sizeof(default_lookup_table));
      uint16_t default_lookup_table_index = macro_addr_offset;
      for(uint16_t macro_num_index = 0; macro_num_index < NUM_MACROS; macro_num_index++) {
        for(uint16_t macro_location_index = 0; macro_location_index < 256; macro_location_index++) {
          macros[macro_num_index][macro_location_index] = default_lookup_table[default_lookup_table_index / 128][default_lookup_table_index % 128];
          default_lookup_table_index++;
        }
      }
      nrf_fstorage_read(&fstorage, 0x3e000, default_lookup_table, sizeof(default_lookup_table));
    
    }
    uint8_t test_byte;
    
    //Enroll();
    bool switch_output;

    
    
    // Enter main loop.
    for (;;)
    {
        //send_ssh_key();
        idle_state_handle();
        
        if (load_active == 1) {
          uint8_t loaded_value;

          while(app_uart_get(&loaded_value) != NRF_SUCCESS && nrf_gpio_pin_read(SWITCH_LEFT) == HIGH) {
            idle_state_handle();
          }
        
          if(nrf_gpio_pin_read(SWITCH_LEFT) == HIGH) {
            process_next_char((char) loaded_value);   
          }

          if(curr_macro == NUM_MACROS) {
            load_active = 0;
            nrf_fstorage_erase(&fstorage, 0x3e000, 2, NULL);
            //nrf_fstorage_erase(&fstorage, 0x3F000, sizeof(macros) + 4, NULL);

            nrf_fstorage_write(&fstorage, 0x3e000, default_lookup_table, sizeof(default_lookup_table), NULL);
            wait_for_flash_ready(&fstorage);
         
            nrf_fstorage_write(&fstorage, 0x3F000, (uint8_t*)((uint32_t) macros - macro_addr_offset), sizeof(macros) + 4, NULL);
            wait_for_flash_ready(&fstorage);

            char* success_str = "Write success!\r";
            write_uart_str(success_str);
          }

        } else if(macro_active == 0 && load_active == 0) {
          key_info = scanMatrix(prev_key_value);
          key_value = key_info & 0xFF;
          key_flags = (key_info >> 8) & 0x01FF;
          prev_flags = (key_info >> 24) & 0x0F;

          NRF_LOG_INFO("Key press: Value: %d Flags: %d Prev: %d Caps: %d Num: %d FN Lock: %d",
            key_value, key_flags, prev_flags, caps_lock, num_lock, fn_lock);

          if(key_value != GARBAGE_KEY)
          {
            //Get the final key value using the flags and value
            manage_send_keypress(key_value, key_flags, prev_flags, false);
          }
          timer = update_prev_key(prev_key_value, prev_flags, key_value, &last_pressed_index, timer);
          if(timer <= 0) {
            manage_send_keypress(prev_key_value[last_pressed_index], key_flags, prev_flags, true);
            timer = SEC_HOLD_COOLDOWN;
          }

          if(macro_active > 0) {
            macro_count = 0;
            macro_status = 1;
          }

        } else if (load_active == 0) {
          
          uint8_t macro_ptr = macro_active - 1;
          if(macro_status == 1) { //Parsing Ops
            //If the number of instructions run is the same as the number of instructions to repeat, move back to the beginning, decrement the # of repeats
            if(repeat_instructions > 0 && curr_repeat_instructions == repeat_instructions) {
              curr_repeat_instructions = 0;
              repeats_left--;

              //If number of repeats left is 0, then done repeating
              if(repeats_left <= 0) {
                curr_repeat_instructions = 0;
                repeat_counter_start = 0;
                repeat_instructions = 0;
              } else {
                macro_count = repeat_counter_start;
              }
            }

            //If the number of instructions to repeat is more than 0, then increment the count
            if(repeat_instructions > 0) {
              curr_repeat_instructions++;
            }

            NRF_LOG_INFO("Cycle run\n"); //Lags if this isn't here

            uint8_t op_byte = load_from_table(macro_ptr, macro_count, 4096 + macro_addr_offset, 256);
            if(op_byte == 0xFF) {
              op_byte = 0;
            }
            uint8_t op = (op_byte & 0xC0) >> 6;

            if(op == 0) {
              macro_status = 0;
            } else if (op == 1) {
              macro_status = 2;
              send_char_stop = (load_from_table(macro_ptr, macro_count, 4096 + macro_addr_offset, 256) & 0x3F) * 2 + macro_count + 1;
              macro_count = macro_count + 1;
            } else if (op == 2) {
              macro_status = 1;
              repeat_instructions = load_from_table(macro_ptr, macro_count, 4096 + macro_addr_offset, 256) & 0x3F;
              repeats_left = load_from_table(macro_ptr, macro_count + 1, 4096 + macro_addr_offset, 256);
              curr_repeat_instructions = 0;
              repeat_counter_start = macro_count + 2;
              macro_count = macro_count + 2;
            } else if (op == 3) {
              uint8_t num_parallel = load_from_table(macro_ptr, macro_count, 4096 + macro_addr_offset, 256) & 0x3F;
              if(num_parallel <= 8 && num_parallel >= 2) {
                uint8_t parallel_keys[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
                for(uint8_t loop_index = 0; loop_index < num_parallel; loop_index++) {
                  parallel_keys[loop_index] = load_from_table(macro_ptr, macro_count + 2 + loop_index, 4096 + macro_addr_offset, 256);
                }
                set_modifiers(load_from_table(macro_ptr, macro_count + 1, 4096 + macro_addr_offset, 256), 64, false);
                send_key_press_parallel(parallel_keys, num_parallel);
              }
              macro_count += num_parallel + 2;
            } else {
              macro_status = 0;
            }
    
          } else { //Sending Chars
            uint8_t mod_value = load_from_table(macro_ptr, macro_count, 4096 + macro_addr_offset, 256);
            uint8_t key_val = load_from_table(macro_ptr, macro_count + 1, 4096 + macro_addr_offset, 256);
            NRF_LOG_INFO("Mod: 0x%x; Val: 0x%x", mod_value, key_val);
            set_modifiers(mod_value, 64, false);
            send_key_press(key_val);
            macro_key_sent = true;
            macro_count = macro_count + 2;
            if(macro_count >= send_char_stop) {
              macro_status = 1;
            }
          }

          if(macro_status == 0) {
            macro_active = 0;
          }
          if(macro_key_sent) {
            nrf_delay_ms(50);
            macro_key_sent = false;
          }
        }

        if(nrf_gpio_pin_read(SWITCH_LEFT) == HIGH && load_active == 0) { //LOAD SWITCH
          load_active = 1;
          load_table_index = 0;
          current_value = 0;
          value_ready = false;

          processing_macro = false;
          curr_macro = 0;
          char* success_str = "Ready to load.\r";
          write_uart_str(success_str);
        } else if(nrf_gpio_pin_read(SWITCH_LEFT) == LOW && load_active > 0) {
          load_active = 0;
        }

        if(nrf_gpio_pin_read(SWITCH_RIGHT) == HIGH) { //MODE SWITCH
          nrf_gpio_pin_write(LED_RIGHT, HIGH);
          mode = 16;
        } else {
          nrf_gpio_pin_write(LED_RIGHT, LOW);
          mode = 0;
        }
         
        if(caps_lock == true) { //TEMP, WILL BE LOAD SWITCH
          nrf_gpio_pin_write(LED_LEFT, HIGH);
        } else {
          nrf_gpio_pin_write(LED_LEFT, LOW);
        }
    }
}


/**
 * @}
 */