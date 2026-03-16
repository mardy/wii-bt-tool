#include "btstack_util.h"
#include "sdp_util.h"

#include <bt-embedded/client.h>
#include <bt-embedded/hci.h>
#include <bt-embedded/l2cap.h>
#include <bt-embedded/services/sdp.h>
#include <gccore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wiiuse/wpad.h>

#define MAX_SEARCH_DEVICES 10

#define BD_ADDR_FMT "%02x:%02x:%02x:%02x:%02x:%02x"
#define BD_ADDR_DATA(b) \
    (b)->bytes[5], (b)->bytes[4], (b)->bytes[3], \
    (b)->bytes[2], (b)->bytes[1], (b)->bytes[0]

static int s_screen_w, s_screen_h;
static bool s_quit_requested = false;

typedef enum {
    SCREEN_TITLE,
    SCREEN_PAIRED_DEVICES,
    SCREEN_GUEST_DEVICES,
    SCREEN_SEARCH_DEVICES,
    SCREEN_LISTEN,
    SCREEN_DEVICE,
    SCREEN_CONNECT,
    SCREEN_SDP,
    SCREEN_SDP_HID,
    SCREEN_HID,
    SCREEN_PAIR,
    SCREEN_LAST,
} ScreenId;

typedef enum {
    ACTION_FIRST = SCREEN_LAST,
    ACTION_QUIT = ACTION_FIRST,
} ActionId;

static ScreenId s_screen_stack[10] = { SCREEN_TITLE };
#define FRAMES_BETWEEN_ANIMATION 5
static int s_screen_index = 0;
static bool s_screen_needs_refresh = true;
static bool s_screen_runs_animation = false;

static conf_pads s_paired_devices;
static conf_pad_guests s_guest_devices;

typedef struct {
    void (*reset)(void);
    void (*draw)(void);
    void (*process_input)(u32 pressed, u32 held);
    void (*pop)(void);
} ScreenMethods;

typedef struct {
    ActionId action_id;
    const char *label;
} ActionItem;

static const ActionItem s_title_screen_items[] = {
    { SCREEN_PAIRED_DEVICES, "See paired devices", },
    { SCREEN_GUEST_DEVICES, "See guest devices", },
    { SCREEN_SEARCH_DEVICES, "Search nearby devices", },
    { SCREEN_LISTEN, "Listen for events", },
    { ACTION_QUIT, "Quit", },
};
#define TITLE_NUM_SCREENS \
    (sizeof(s_title_screen_items) / sizeof(s_title_screen_items[0]))
static int s_title_item_index = 0;

typedef struct {
    BteBdAddr bdaddr;
    BteClassOfDevice cod;
    uint8_t page_scan_rep_mode;
    uint16_t clock_offset;
    char name[0x40];
    bool querying_name;
    bool queried_name;
} DeviceEntry;

typedef struct {
    int item_index;
    BteLap lap;
    bool search_running;
    int error_code;
    int num_devices;
    DeviceEntry devices[MAX_SEARCH_DEVICES];
} SearchDeviceData;

static SearchDeviceData s_search_device_data = {
    BTE_LAP_GIAC,
    false,
    0,
    0,
};

#define MAX_CONNECTION_REQUESTS 8
typedef struct {
    BteBdAddr address;
    BteClassOfDevice cod;
    BteLinkType link_type;
} BtConnectionRequestData;

typedef struct {
    BtConnectionRequestData requests[MAX_CONNECTION_REQUESTS];
    int num_requests;
} ListenData;

typedef enum {
    CONN_STATUS_DISCONNECTED = 0,
    CONN_STATUS_CONNECTING,
    CONN_STATUS_CONNECTED,
    CONN_STATUS_SDP_BROWSE_COMPLETE,
    CONN_STATUS_NULL_RESPONSE,
} ConnectionStatus;

typedef struct {
    DeviceEntry device;
    int item_index;
    int current_row;
    ConnectionStatus conn_status;
    int error_code;
    int l2cap_status;
    char error_msg[128];
    BteSdpClient *sdp_handle;
    BteL2cap *hid_ctrl;
    BteL2cap *hid_intr;
    bool has_pending_call;
    uint8_t sdp_response[4096];
    uint16_t sdp_num_services;
    uint32_t sdp_hid_service_id;
    int sdp_response_len;
    int num_link_key_requests;
    int num_link_key_notifications;
    int num_pin_code_requests;
    int num_authentication_completes;
    uint8_t link_key[16];
} DeviceData;

static DeviceData s_device_data;
static ListenData s_listen_data;
static bool s_sdp_dump_raw = false;
static BteClient *s_client = NULL;
static bool s_hci_initialized = false;

static const ActionItem s_device_actions[] = {
    { SCREEN_CONNECT, "Connect", },
    { SCREEN_SDP, "Read SDP data", },
    { SCREEN_SDP_HID, "Read SDP HID data", },
    { SCREEN_HID, "Run HID test", },
    { SCREEN_PAIR, "Pair device", },
};
#define DEVICE_NUM_ACTIONS \
    (sizeof(s_device_actions) / sizeof(s_device_actions[0]))

static void retrive_device_names(SearchDeviceData *data);
static const ScreenMethods *current_screen();

static void queue_refresh()
{
    s_screen_needs_refresh = true;
}

static void set_animating(bool animating)
{
    s_screen_runs_animation = animating;
}

static inline void bd_address_to_conf(const BteBdAddr *a, u8 *conf)
{
	const uint8_t *b = a->bytes;
	conf[0] = b[5];
	conf[1]	= b[4];
	conf[2]	= b[3];
	conf[3] = b[2];
	conf[4] = b[1];
	conf[5] = b[0];
}

static inline BteBdAddr bd_address_from_conf(const u8 *conf)
{
	BteBdAddr addr = {{ conf[5], conf[4], conf[3], conf[2], conf[1], conf[0] }};
	return addr;
}

static int sprintf_bdaddr(char *dest, const BteBdAddr *bdaddr)
{
    return sprintf(dest,  BD_ADDR_FMT, BD_ADDR_DATA(bdaddr));
}

static void color_selected(bool selected)
{
    printf("\x1b[%d;%dm",
           selected ?  (40 + CONSOLE_COLOR_BLUE) : (40 + CONSOLE_COLOR_BLACK),
           selected);
}

static const char *describe_device(BteClassOfDevice cod)
{
    u8 major = bte_cod_get_major_dev_class(cod);
    u8 minor = bte_cod_get_minor_dev_class(cod);
    switch (major) {
    case 0: return "Misc";
    case 1: return "Computer";
    case 2: return "Phone";
    case 3: return "LAN AP";
    case 4: return "Audio/Video";
    case 5:
        switch (minor >> 4) {
        case 0:
            switch (minor & 0xf) {
            case 1: return "Joystick";
            case 2: return "Gamepad";
            case 3: return "Remote";
            case 4: return "Sensor";
            case 5: return "Tablet";
            default: return "Peripheral";
            }
        case 1: return "Keyboard";
        case 2: return "Mouse";
        case 3: return "Mouse+KB";
        default: return "Peripheral";
        }
    case 6: return "Imaging";
    case 7: return "Wearable";
    case 8: return "Toy";
    case 9: return "Health";
    default: return "Unrecognized";
    }
}

static void push_screen(ScreenId id)
{
    queue_refresh();
    set_animating(false);
    s_screen_stack[++s_screen_index] = id;

    const ScreenMethods *screen = current_screen();
    if (screen->reset) screen->reset();
}

static ScreenId current_screen_id(void)
{
    return s_screen_stack[s_screen_index];
}

static void pop_screen()
{
    set_animating(false);
    queue_refresh();
    if (s_screen_index > 0) s_screen_index--;
}

static char get_anim_char()
{
    static const char wait_seq[] = "\\|/-";
    static int last_char = 0;
    static int frames_since_last_update = 0;
    if (s_screen_runs_animation &&
        frames_since_last_update % FRAMES_BETWEEN_ANIMATION == 0) {
        last_char++;
    }
    return wait_seq[last_char % 4];
}

static bool is_active(const u8 *bdaddr) {
    for (int i = 0; i < CONF_PAD_MAX_ACTIVE; i++) {
        const conf_pad_device *device = &s_paired_devices.active[i];

        if (memcmp(bdaddr, device->bdaddr, sizeof(device->bdaddr)) == 0)
            return true;
    }

    return false;
}

static void screen_title_draw()
{
    printf(CONSOLE_RESET "\x1b[2;0H" CONSOLE_YELLOW);
    printf("Wii Bluetooth tool");

    printf("\x1b[10;0H" CONSOLE_WHITE);

    for (int i = 0; i < TITLE_NUM_SCREENS; i++) {
        const ActionItem *item = &s_title_screen_items[i];

        color_selected(i == s_title_item_index);
        printf("%s\n", item->label);
    }

    printf(CONSOLE_WHITE CONSOLE_RESET "\x1b[%d;0H", s_screen_h - 4);
    printf("_________________________________\n");
    printf("Move with arrows, select with "
           CONSOLE_WHITE "2");
}

void screen_title_process_input(u32 buttons, u32 held)
{
    if (buttons & WPAD_BUTTON_2) {
        ActionId action_id = s_title_screen_items[s_title_item_index].action_id;
        if (action_id < ACTION_FIRST) {
            push_screen(action_id);
        } else if (action_id == ACTION_QUIT) {
            s_quit_requested = true;
        }
    } else if (buttons & (WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT)) {
        if (s_title_item_index + 1 < TITLE_NUM_SCREENS) {
            queue_refresh();
            s_title_item_index++;
        }
    } else if (buttons & (WPAD_BUTTON_UP | WPAD_BUTTON_RIGHT)) {
        if (s_title_item_index > 0) {
            queue_refresh();
            s_title_item_index--;
        }
    }
}

static void screen_paired_devices_draw()
{
    CONF_GetPadDevices(&s_paired_devices);

    printf(CONSOLE_RESET "\x1b[2;0H" CONSOLE_YELLOW);
    printf("PAIRED DEVICES");

    printf("\x1b[4;0H");

    for (int i = 0; i < s_paired_devices.num_registered; i++) {
        const conf_pad_device *device = &s_paired_devices.registered[i];

        char addr_buffer[20];
        BteBdAddr address = bd_address_from_conf(device->bdaddr);
        sprintf_bdaddr(addr_buffer, &address);
        int color = is_active(device->bdaddr) ? 32 : 37;
        printf(CONSOLE_ESC(37;0m) "% 2d) \x1b[%d;1m%s - \"%.64s\"\n", i + 1, color, addr_buffer, device->name);
    }

    if (s_paired_devices.num_registered == 0)
        printf("No devices registered");

    printf(CONSOLE_WHITE CONSOLE_RESET "\x1b[%d;0H", s_screen_h - 4);
    printf("_________________________________\n");
    printf(CONSOLE_WHITE "1 - " CONSOLE_RESET "Back  "
           CONSOLE_WHITE "2 - " CONSOLE_RESET "Guest devices" );
}

void screen_paired_devices_process_input(u32 buttons, u32 held)
{
    if (buttons & WPAD_BUTTON_2) {
        push_screen(SCREEN_GUEST_DEVICES);
    } else if (buttons & WPAD_BUTTON_1) {
        pop_screen();
    }
}

static void screen_guest_devices_draw()
{
    CONF_GetPadGuestDevices(&s_guest_devices);

    printf(CONSOLE_RESET "\x1b[2;0H" CONSOLE_YELLOW);
    printf("GUEST DEVICES");

    printf(CONSOLE_WHITE);
    printf("\x1b[4;0H");

    for (int i = 0; i < s_guest_devices.num_guests; i++) {
        const conf_pad_guest_device *device = &s_guest_devices.guests[i];

        BteBdAddr address = bd_address_from_conf(device->bdaddr);
        printf("% 2d) " BD_ADDR_FMT " - \"%.64s\"\n", i + 1, BD_ADDR_DATA(&address), device->name);
    }

    if (s_guest_devices.num_guests == 0)
        printf("No guest devices registered");

    printf(CONSOLE_WHITE CONSOLE_RESET "\x1b[%d;0H", s_screen_h - 4);
    printf("_________________________________\n");
    printf(CONSOLE_WHITE "1 - " CONSOLE_RESET "Back  ");
}

void screen_guest_devices_process_input(u32 buttons, u32 held)
{
    if (buttons & WPAD_BUTTON_1) {
        pop_screen();
    }
}

static void screen_search_devices_reset()
{
    SearchDeviceData *data = &s_search_device_data;
    memset(data, 0, sizeof(*data));
    data->lap = BTE_LAP_GIAC;
}

static void screen_search_devices_draw()
{
    printf(CONSOLE_RESET "\x1b[2;0H" CONSOLE_YELLOW);
    printf("SEARCH DEVICES");

    printf(CONSOLE_WHITE);
    printf("\x1b[4;0H");

    const SearchDeviceData *data = &s_search_device_data;
    const char *search_type = (data->lap == BTE_LAP_GIAC) ? "General" : "Limited";

    char anim_char = get_anim_char();

    if (data->search_running) {
        printf("  Searching... %c\n", anim_char);
    } else {
        if (data->error_code) {
            printf("Error code: %d\n\n", data->error_code);
        }

        color_selected(data->item_index == 0);
        printf("Search again (%s)\n\n", search_type);

        for (int i = 0; i < data->num_devices; i++) {
            const DeviceEntry *device = &data->devices[i];

            char addr_buffer[20];
            sprintf_bdaddr(addr_buffer, &device->bdaddr);

            char text[100];
            if (device->queried_name) {
                sprintf(text, CONSOLE_ESC(1m) "%.64s" CONSOLE_RESET, device->name);
            } else if (device->querying_name) {
                sprintf(text, "Retrieving name... %c", anim_char);
            } else {
                sprintf(text, "Queued for name retrieval");
            }
            const char *class_desc = describe_device(device->cod);
            color_selected(data->item_index == i + 1);
            printf("% 2d) %s - (%s) %s\n", i + 1, addr_buffer, class_desc, text);
        }

        color_selected(false);
        if (data->num_devices == 0)
            printf("No devices found");
    }

    printf(CONSOLE_WHITE CONSOLE_RESET "\x1b[%d;0H", s_screen_h - 4);
    printf("_________________________________\n");
    printf(CONSOLE_WHITE "1 - " CONSOLE_RESET "Back  ");
    const char *action_text = (data->item_index == 0) ?
        "Run search" : "Enter device page";
    printf(CONSOLE_WHITE "2 - " CONSOLE_RESET "%s\n", action_text);
    printf(CONSOLE_WHITE "A - " CONSOLE_RESET "Switch search type");
}

static bool device_in_list(const BteBdAddr *bdaddr, const DeviceEntry *devices, int num_devices)
{
    for (int i = 0; i < num_devices; i++) {
        if (memcmp(bdaddr, &devices[i].bdaddr, 6) == 0)
            return true;
    }
    return false;
}

static void on_name_retrieved(BteHci *hci, const BteHciReadRemoteNameReply *reply, void *cb_data)
{
    SearchDeviceData *data = cb_data;

    for (int i = 0; i < data->num_devices; i++) {
        DeviceEntry *device = &data->devices[i];
        if (device->querying_name) {
            snprintf(device->name, sizeof(device->name), "%s", reply->name);
            device->queried_name = true;
            device->querying_name = false;
            break;
        }
    }

    queue_refresh();
    retrive_device_names(data);
}

static void retrive_device_names(SearchDeviceData *data)
{
    bool has_pending_operation = false;

    for (int i = 0; i < data->num_devices; i++) {
        DeviceEntry *device = &data->devices[i];
        if (device->querying_name) {
            /* Let's wait for this operation to complete */
            has_pending_operation = true;
            break;
        }

        if (!device->queried_name) {
            has_pending_operation = true;
            device->querying_name = true;
            bte_hci_read_remote_name(bte_hci_get(s_client),
                                     &device->bdaddr,
                                     device->page_scan_rep_mode,
                                     device->clock_offset,
                                     NULL, on_name_retrieved, data);
            break;
        }
    }

    set_animating(has_pending_operation);
}

static void search_devices_cb(BteHci *hci, const BteHciInquiryReply *reply, void *cb_data)
{
    SearchDeviceData *data = cb_data;
    data->error_code = reply->status;
    int num_devices = 0;
    data->num_devices = reply->num_responses;
    if (data->num_devices > MAX_SEARCH_DEVICES)
        data->num_devices = MAX_SEARCH_DEVICES;
    for (int i = 0; i < reply->num_responses; i++) {
        const BteBdAddr *bdaddr = &reply->responses[i].address;
        if (device_in_list(bdaddr, data->devices, num_devices)) {
            /* Duplicate, ignoring */
            continue;
        }
        memset(&data->devices[num_devices], 0, sizeof(data->devices[num_devices]));
        data->devices[num_devices].bdaddr = *bdaddr;
        data->devices[num_devices].cod = reply->responses[i].class_of_device;
        num_devices++;
        if (num_devices >= MAX_SEARCH_DEVICES) break;
    }

    data->num_devices = num_devices;
    data->search_running = false;
    retrive_device_names(data);
}

static void screen_search_devices_process_input(u32 buttons, u32 held)
{
    SearchDeviceData *data = &s_search_device_data;
    if (buttons & WPAD_BUTTON_1) {
        pop_screen();
    } else if (buttons & WPAD_BUTTON_2) {
        if (data->item_index == 0) {
            data->search_running = true;
            set_animating(true);
            queue_refresh();
            bte_hci_inquiry(bte_hci_get(s_client),
                            data->lap, 3, MAX_SEARCH_DEVICES,
                            NULL, search_devices_cb, data);
        } else {
            push_screen(SCREEN_DEVICE);
        }
    } else if (buttons & WPAD_BUTTON_A) {
        queue_refresh();
        data->lap = (data->lap == BTE_LAP_GIAC) ?
            BTE_LAP_LIAC : BTE_LAP_GIAC;
    } else if (buttons & WPAD_BUTTON_LEFT) {
        if (data->item_index < data->num_devices) {
            queue_refresh();
            data->item_index++;
        }
    } else if (buttons & WPAD_BUTTON_RIGHT) {
        if (data->item_index > 0) {
            queue_refresh();
            data->item_index--;
        }
    }
}

static bool connection_request_cb(BteHci *hci,
                                  const BteBdAddr *address,
                                  const BteClassOfDevice *cod,
                                  BteLinkType link_type,
                                  void *cb_data)
{
    ListenData *data = cb_data;

    if (data->num_requests >= MAX_CONNECTION_REQUESTS)
        return false;

    BtConnectionRequestData event = {
        *address,
        *cod,
        link_type,
    };
    memcpy(&data->requests[data->num_requests++], &event, sizeof(event));
    queue_refresh();
    return true;
}

static void screen_listen_reset()
{
    ListenData *data = &s_listen_data;
    memset(data, 0, sizeof(*data));

    BteHci *hci = bte_hci_get(s_client);
    bte_client_set_userdata(s_client, data);
    bte_hci_on_connection_request(hci, connection_request_cb);
    bte_hci_write_local_name(hci, "Wii", NULL, NULL);
    bte_hci_write_scan_enable(hci, BTE_HCI_SCAN_ENABLE_INQ_PAGE, NULL, NULL);
    set_animating(true);
}

static void screen_listen_draw()
{
    printf(CONSOLE_RESET "\x1b[2;0H" CONSOLE_YELLOW);
    printf("LISTENING FOR EVENT");

    printf(CONSOLE_WHITE);
    printf("\x1b[4;0H");

    const ListenData *data = &s_listen_data;

    char anim_char = get_anim_char();

    printf("Got %d requests\n\n", data->num_requests);

    for (int i = 0; i < data->num_requests; i++) {
        const BtConnectionRequestData *req = &data->requests[i];

        char addr_buffer[20];
        sprintf_bdaddr(addr_buffer, &req->address);

        const char *class_desc = describe_device(req->cod);
        printf("% 2d) %s - (%s), link type: %02x\n", i + 1,
               addr_buffer, class_desc, req->link_type);
    }

    printf("\nListening... %c\n", anim_char);

    printf(CONSOLE_WHITE CONSOLE_RESET "\x1b[%d;0H", s_screen_h - 4);
    printf("_________________________________\n");
    printf(CONSOLE_WHITE "1 - " CONSOLE_RESET "Back  ");
}

static void screen_listen_process_input(u32 buttons, u32 held)
{
    if (buttons & WPAD_BUTTON_1) {
        pop_screen();
    }
}

static void screen_device_reset()
{
    DeviceData *data = &s_device_data;
    memset(data, 0, sizeof(*data));

    const SearchDeviceData *search_data = &s_search_device_data;

    bte_client_set_userdata(s_client, data);
    memcpy(&data->device, &search_data->devices[search_data->item_index - 1],
           sizeof(data->device));
}

static void screen_device_draw()
{
    const DeviceData *data = &s_device_data;

    printf(CONSOLE_RESET "\x1b[2;0H" CONSOLE_YELLOW);
    char bdaddr[20];
    sprintf_bdaddr(bdaddr, &data->device.bdaddr);
    printf("DEVICE %s - %.64s", bdaddr, data->device.name);

    printf(CONSOLE_WHITE);
    printf("\x1b[4;0H");

    for (int i = 0; i < DEVICE_NUM_ACTIONS; i++) {
        const ActionItem *item = &s_device_actions[i];

        color_selected(i == data->item_index);
        printf("%s\n", item->label);
    }

    printf(CONSOLE_WHITE CONSOLE_RESET "\x1b[%d;0H", s_screen_h - 4);
    printf("_________________________________\n");
    printf(CONSOLE_WHITE "1 - " CONSOLE_RESET "Back  ");
}

static void screen_device_process_input(u32 buttons, u32 held)
{
    DeviceData *data = &s_device_data;

    if (buttons & WPAD_BUTTON_1) {
        pop_screen();
    } else if (buttons & WPAD_BUTTON_2) {
        ActionId action_id = s_device_actions[data->item_index].action_id;
        if (action_id < ACTION_FIRST) {
            push_screen(action_id);
        }
    } else if (buttons & WPAD_BUTTON_LEFT) {
        if (data->item_index < DEVICE_NUM_ACTIONS) {
            queue_refresh();
            data->item_index++;
        }
    } else if (buttons & WPAD_BUTTON_RIGHT) {
        if (data->item_index > 0) {
            queue_refresh();
            data->item_index--;
        }
    }
}

static void connect_cb(BteL2cap *l2cap, const BteL2capConnectionResponse *reply, void *cb_data)
{
    DeviceData *data = cb_data;

    if (reply->result == BTE_L2CAP_CONN_RESP_RES_OK) {
        data->conn_status = CONN_STATUS_CONNECTED;
    } else if (reply->result == BTE_L2CAP_CONN_RESP_RES_PENDING) {
        /* TODO auth */
    } else {
        data->conn_status = CONN_STATUS_DISCONNECTED;
    }
    data->error_code = reply->result;
    data->l2cap_status = reply->status;
    set_animating(false);
}

static void screen_connect_reset()
{
    DeviceData *data = &s_device_data;

    data->error_code = 0;
    data->l2cap_status = 0;
    data->conn_status = CONN_STATUS_CONNECTING;
    set_animating(true);
    bte_l2cap_new_outgoing(s_client, &data->device.bdaddr,
                           BTE_L2CAP_PSM_HID_CTRL, NULL,
                           BTE_L2CAP_CONNECT_FLAG_NONE,
                           connect_cb, data);
}

static void screen_connect_draw()
{
    const DeviceData *data = &s_device_data;

    printf(CONSOLE_RESET "\x1b[2;0H" CONSOLE_YELLOW);
    printf("CONNECTION TO " BD_ADDR_FMT " - %.64s", BD_ADDR_DATA(&data->device.bdaddr), data->device.name);

    printf(CONSOLE_WHITE);
    printf("\x1b[4;0H");

    char anim_char = get_anim_char();

    if (data->conn_status == CONN_STATUS_CONNECTING) {
        printf("Connecting... %c\n", anim_char);
    } else {
        printf("%s      \n", data->conn_status == CONN_STATUS_CONNECTED ?
               "Connected" : "Disconnected");
        printf("Error code = %d, status = %d", data->error_code, data->l2cap_status);
    }

    printf(CONSOLE_WHITE CONSOLE_RESET "\x1b[%d;0H", s_screen_h - 4);
    printf("_________________________________\n");
    printf(CONSOLE_WHITE "1 - " CONSOLE_RESET "Back  ");
}

static void screen_connect_process_input(u32 buttons, u32 held)
{
    if (buttons & WPAD_BUTTON_1) {
        pop_screen();
    }
}

static void sdp_service_search_attr_cb(BteSdpClient *sdp, const BteSdpServiceAttrReply *reply,
                                       void *cb_data)
{
    DeviceData *data = cb_data;

    if (reply->error_code) {
        data->has_pending_call = false;
        data->conn_status = CONN_STATUS_SDP_BROWSE_COMPLETE;
        sprintf(data->error_msg, "Error reading attributes: %d", reply->error_code);
        set_animating(false);
        queue_refresh();
        return;
    }

    uint32_t size = bte_sdp_de_get_total_size(reply->attr_list_de);
    if (size > sizeof(data->sdp_response)) {
        data->has_pending_call = false;
        data->conn_status = CONN_STATUS_SDP_BROWSE_COMPLETE;
        sprintf(data->error_msg, "Response too long: %u", size);
        set_animating(false);
        queue_refresh();
        return;
    }

    memcpy(data->sdp_response, reply->attr_list_de, size);
    data->has_pending_call = false;
    data->conn_status = CONN_STATUS_SDP_BROWSE_COMPLETE;
    set_animating(false);
    queue_refresh();
}

static void sdp_connect_cb(BteL2cap *l2cap, const BteL2capNewConfiguredReply *reply, void *cb_data)
{
    DeviceData *data = cb_data;

    data->error_code = reply->result;
    if (data->error_code == 0) {
        data->conn_status = CONN_STATUS_CONNECTED;
    } else {
        data->conn_status = CONN_STATUS_DISCONNECTED;
        set_animating(false);
        queue_refresh();
        return;
    }
    BteSdpClient *sdp = data->sdp_handle = bte_sdp_client_new(l2cap);

    uint8_t pattern[32];
    bte_sdp_de_write(pattern, sizeof(pattern),
                     BTE_SDP_DE_TYPE_SEQUENCE,
                     BTE_SDP_DE_TYPE_UUID16, BTE_SDP_PROTO_L2CAP,
                     BTE_SDP_DE_END,
                     BTE_SDP_DE_END);
    uint8_t id_list[20];
    bte_sdp_de_write(id_list, sizeof(id_list),
                     BTE_SDP_DE_TYPE_SEQUENCE,
                     BTE_SDP_DE_TYPE_UINT32, 0x0000ffff,
                     BTE_SDP_DE_END,
                     BTE_SDP_DE_END);

    bool ok = bte_sdp_service_search_attr_req(sdp, pattern, 1000,
                                              id_list, sdp_service_search_attr_cb, data);
    if (!ok) {
        data->conn_status = CONN_STATUS_DISCONNECTED;
        sprintf(data->error_msg, "Failed to issue SDP request");
        set_animating(false);
    }
    queue_refresh();
}

static void screen_sdp_reset()
{
    DeviceData *data = &s_device_data;

    data->current_row = 0;
    data->error_code = 0;
    data->l2cap_status = 0;
    data->conn_status = CONN_STATUS_CONNECTING;
    data->sdp_response_len = 0;
    data->error_msg[0] = 0;
    if (data->sdp_handle) {
        bte_sdp_client_unref(data->sdp_handle);
        data->sdp_handle = NULL;
    }
    data->has_pending_call = true;
    set_animating(true);
    bte_l2cap_new_configured(s_client, &data->device.bdaddr, BTE_L2CAP_PSM_SDP,
                             NULL, BTE_L2CAP_CONNECT_FLAG_NONE, NULL,
                             sdp_connect_cb, data);
}

static void screen_sdp_pop()
{
    DeviceData *data = &s_device_data;

    if (data->sdp_handle) {
        bte_sdp_client_unref(data->sdp_handle);
        data->sdp_handle = NULL;
    }
}

static void screen_sdp_draw()
{
    const DeviceData *data = &s_device_data;

    printf(CONSOLE_RESET "\x1b[2;0H" CONSOLE_YELLOW);
    printf("SDP TO " BD_ADDR_FMT " - %.64s", BD_ADDR_DATA(&data->device.bdaddr), data->device.name);

    printf(CONSOLE_WHITE);
    printf("\x1b[4;0H");

    char anim_char = get_anim_char();

    if (data->conn_status == CONN_STATUS_CONNECTING) {
        printf("Connecting... %c\n", anim_char);
    } else if (data->conn_status == CONN_STATUS_DISCONNECTED) {
        printf("Disconnected     \n");
        printf("Error code = %d, status = %d\n", data->error_code, data->l2cap_status);
    } else if (data->conn_status == CONN_STATUS_CONNECTED) {
        printf("Connected.\n");
        printf("Browsing SDP services... %c\n", anim_char);
    } else if (data->conn_status == CONN_STATUS_SDP_BROWSE_COMPLETE) {
        printf("Got response, size = %d\n", data->sdp_response_len);
        if (s_sdp_dump_raw) {
            printf("Got response size %d\n", data->sdp_response_len);
            de_dump_data_element(data->sdp_response, data->current_row, 18);
        } else {
            sdp_print_attribute_list(data->sdp_response, data->current_row, 19);
        }
    } else {
        printf("Wierd status %d\n", data->conn_status);
    }

    if (data->error_msg[0] != 0) {
        printf(CONSOLE_RESET "\x1b[%d;0H" CONSOLE_RED "%s",
               s_screen_h - 5, data->error_msg);
    }
    printf(CONSOLE_WHITE CONSOLE_RESET "\x1b[%d;0H", s_screen_h - 4);
    printf("_________________________________\n");
    printf(CONSOLE_WHITE "1 - " CONSOLE_RESET "Back  ");
}

static void screen_sdp_process_input(u32 buttons, u32 held)
{
    DeviceData *data = &s_device_data;
    if (buttons & WPAD_BUTTON_1) {
        pop_screen();
    } else if (buttons & WPAD_BUTTON_A) {
        queue_refresh();
        s_sdp_dump_raw = !s_sdp_dump_raw;
    } else if ((buttons | held) & WPAD_BUTTON_LEFT) {
        queue_refresh();
        data->current_row++;
    } else if ((buttons | held) & WPAD_BUTTON_RIGHT) {
        if (data->current_row > 0) {
            queue_refresh();
            data->current_row--;
        }
    }
}

static void sdp_hid_connect_cb(BteL2cap *l2cap, const BteL2capNewConfiguredReply *reply, void *cb_data)
{
    DeviceData *data = cb_data;

    data->error_code = reply->result;
    if (data->error_code == 0) {
        data->conn_status = CONN_STATUS_CONNECTED;
    } else {
        data->conn_status = CONN_STATUS_DISCONNECTED;
        sprintf(data->error_msg, "Connection failed, error %04x", reply->result);
        set_animating(false);
        queue_refresh();
        return;
    }
    BteSdpClient *sdp = data->sdp_handle = bte_sdp_client_new(l2cap);

    uint8_t pattern[32];
    bte_sdp_de_write(pattern, sizeof(pattern),
                     BTE_SDP_DE_TYPE_SEQUENCE,
                     BTE_SDP_DE_TYPE_UUID16, BTE_SDP_SRV_CLASS_HDP,
                     BTE_SDP_DE_END,
                     BTE_SDP_DE_END);
    uint8_t id_list[20];
    bte_sdp_de_write(id_list, sizeof(id_list),
                     BTE_SDP_DE_TYPE_SEQUENCE,
                     BTE_SDP_DE_TYPE_UUID16, BTE_SDP_ATTR_ID_HID_DESC_LIST,
                     BTE_SDP_DE_END,
                     BTE_SDP_DE_END);

    bool ok = bte_sdp_service_search_attr_req(sdp, pattern, 1000,
                                              id_list, sdp_service_search_attr_cb, data);
    if (!ok) {
        data->conn_status = CONN_STATUS_DISCONNECTED;
        sprintf(data->error_msg, "Failed to issue SDP request");
        set_animating(false);
    }
    queue_refresh();
}

static void screen_sdp_hid_reset()
{
    DeviceData *data = &s_device_data;

    data->current_row = 0;
    data->error_code = 0;
    data->error_msg[0] = 0;
    data->l2cap_status = 0;
    data->conn_status = CONN_STATUS_CONNECTING;
    data->sdp_response_len = 0;
    data->has_pending_call = true;
    set_animating(true);
    bte_l2cap_new_configured(s_client, &data->device.bdaddr, BTE_L2CAP_PSM_SDP,
                             NULL, BTE_L2CAP_CONNECT_FLAG_NONE, NULL,
                             sdp_hid_connect_cb, data);
}

static void screen_sdp_hid_pop()
{
    screen_sdp_pop();
}

static void screen_sdp_hid_draw()
{
    const DeviceData *data = &s_device_data;

    printf(CONSOLE_RESET "\x1b[2;0H" CONSOLE_YELLOW);
    printf("SDP TO " BD_ADDR_FMT " - %.64s", BD_ADDR_DATA(&data->device.bdaddr), data->device.name);

    printf(CONSOLE_WHITE);
    printf("\x1b[4;0H");

    char anim_char = get_anim_char();
    if (!data->has_pending_call) {
        anim_char = 'x';
    }

    if (data->conn_status == CONN_STATUS_CONNECTING) {
        printf("Connecting... %c\n", anim_char);
    } else if (data->conn_status == CONN_STATUS_DISCONNECTED) {
        printf("Disconnected     \n");
        printf("Error code = %d, status = %d\n", data->error_code, data->l2cap_status);
    } else if (data->conn_status == CONN_STATUS_CONNECTED) {
        printf("Connected.\n");
        printf("Getting service ID... %c                  \n", anim_char);
    } else if (data->conn_status == CONN_STATUS_SDP_BROWSE_COMPLETE) {
        if (s_sdp_dump_raw) {
            printf("Got response size %d\n", data->sdp_response_len);
            de_dump_data_element(data->sdp_response, data->current_row, 18);
        } else {
            sdp_print_attribute_list(data->sdp_response, data->current_row, 19);
        }
    } else if (data->conn_status == CONN_STATUS_NULL_RESPONSE) {
        printf("Error code = %d, status = %d\n", data->error_code, data->l2cap_status);
        printf("Got an empty response\n");
    }

    if (data->error_msg[0] != 0) {
        printf(CONSOLE_RESET "\x1b[%d;0H" CONSOLE_RED "%s",
               s_screen_h - 5, data->error_msg);
    }
    printf(CONSOLE_WHITE CONSOLE_RESET "\x1b[%d;0H", s_screen_h - 4);
    printf("_________________________________\n");
    printf(CONSOLE_WHITE "1 - " CONSOLE_RESET "Back  ");
}

static void screen_sdp_hid_process_input(u32 buttons, u32 held)
{
    DeviceData *data = &s_device_data;
    if (buttons & WPAD_BUTTON_1) {
        pop_screen();
    } else if (buttons & WPAD_BUTTON_A) {
        queue_refresh();
        s_sdp_dump_raw = !s_sdp_dump_raw;
    } else if ((buttons | held) & WPAD_BUTTON_LEFT) {
        queue_refresh();
        data->current_row++;
    } else if ((buttons | held) & WPAD_BUTTON_RIGHT) {
        if (data->current_row > 0) {
            queue_refresh();
            data->current_row--;
        }
    }
}

static void hid_message_cb(BteL2cap *l2cap, BteBufferReader *reader, void *cb_data)
{
    DeviceData *data = cb_data;

    int size = bte_buffer_reader_read(reader, data->sdp_response, sizeof(data->sdp_response));
    data->sdp_response_len = size;
    queue_refresh();
}

static void hid_connect_intr_cb(BteL2cap *l2cap, const BteL2capNewConfiguredReply *reply, void *cb_data)
{
    DeviceData *data = cb_data;

    data->error_code = reply->result;
    if (data->error_code == 0) {
        data->conn_status = CONN_STATUS_CONNECTED;
    } else {
        data->conn_status = CONN_STATUS_DISCONNECTED;
        set_animating(false);
        printf("Cannot connect intr channel: %04x", reply->result);
        queue_refresh();
        return;
    }

    data->hid_intr = bte_l2cap_ref(l2cap);
    bte_l2cap_set_userdata(l2cap, data);

    bte_l2cap_on_message_received(data->hid_intr, hid_message_cb);
    bte_l2cap_on_message_received(data->hid_ctrl, hid_message_cb);
    queue_refresh();
    /* TODO: get some info */
}

static void hid_connect_ctrl_cb(BteL2cap *l2cap, const BteL2capNewConfiguredReply *reply, void *cb_data)
{
    DeviceData *data = cb_data;

    data->error_code = reply->result;
    if (data->error_code != 0) {
        data->conn_status = CONN_STATUS_DISCONNECTED;
        set_animating(false);
        printf("Cannot connect ctrl channel: %04x", reply->result);
        queue_refresh();
        return;
    }

    data->hid_ctrl = bte_l2cap_ref(l2cap);
    bte_l2cap_set_userdata(l2cap, data);
    bte_l2cap_new_configured(s_client, &data->device.bdaddr, BTE_L2CAP_PSM_HID_INTR,
                             NULL, BTE_L2CAP_CONNECT_FLAG_NONE, NULL,
                             hid_connect_intr_cb, data);
    queue_refresh();
}

static void screen_hid_reset()
{
    DeviceData *data = &s_device_data;

    data->error_code = 0;
    data->error_msg[0] = 0;
    data->l2cap_status = 0;
    data->conn_status = CONN_STATUS_CONNECTING;
    data->sdp_response_len = 0;
    if (data->hid_ctrl) {
        bte_l2cap_unref(data->hid_ctrl);
        data->hid_ctrl = NULL;
    }
    if (data->hid_intr) {
        bte_l2cap_unref(data->hid_intr);
        data->hid_intr = NULL;
    }
    set_animating(true);
    bte_l2cap_new_configured(s_client, &data->device.bdaddr, BTE_L2CAP_PSM_HID_CTRL,
                             NULL, BTE_L2CAP_CONNECT_FLAG_NONE, NULL,
                             hid_connect_ctrl_cb, data);
}

static void screen_hid_draw()
{
    const DeviceData *data = &s_device_data;

    printf(CONSOLE_RESET "\x1b[2;0H" CONSOLE_YELLOW);
    printf("HID for " BD_ADDR_FMT " - %.64s", BD_ADDR_DATA(&data->device.bdaddr), data->device.name);

    printf(CONSOLE_WHITE);
    printf("\x1b[4;0H");

    char anim_char = get_anim_char();

    if (data->conn_status == CONN_STATUS_CONNECTING) {
        printf("Connecting... %c\n", anim_char);
    } else {
        printf("%s      \n", data->conn_status == CONN_STATUS_CONNECTED ?
               "Connected" : "Disconnected");
        printf("Error code = %d, ctrl = %d, intr = %d", data->error_code, data->hid_ctrl != NULL, data->hid_intr != NULL);

        if (data->sdp_response_len > 0) {
            printf("Got message (size %d):\n", data->sdp_response_len);
            int size = data->sdp_response_len;
            if (size > 32) size = 32;
            for (int i = 0; i < size; i++)
                printf(" %02x", data->sdp_response[i]);
        }
    }

    if (data->error_msg[0] != 0) {
        printf(CONSOLE_RESET "\x1b[%d;0H" CONSOLE_RED "%s",
               s_screen_h - 5, data->error_msg);
    }
    printf(CONSOLE_WHITE CONSOLE_RESET "\x1b[%d;0H", s_screen_h - 4);
    printf("_________________________________\n");
    printf(CONSOLE_WHITE "1 - " CONSOLE_RESET "Back  ");
}

static void screen_hid_process_input(u32 buttons, u32 held)
{
    if (buttons & WPAD_BUTTON_1) {
        pop_screen();
    }
}

static bool link_key_request_cb(BteHci *hci,
                                const BteBdAddr *address,
                                void *cb_data)
{
    DeviceData *data = cb_data;

    queue_refresh();
    if (memcmp(address, &data->device.bdaddr, 6) != 0) return false;

    data->num_link_key_requests++;
    bte_hci_link_key_req_neg_reply(hci, address, NULL, NULL);
    return true;;
}

static bool link_key_notification_cb(BteHci *hci, const BteHciLinkKeyNotificationData *event,
                                     void *cb_data)
{
    DeviceData *data = cb_data;

    queue_refresh();
    if (memcmp(&event->address, &data->device.bdaddr, 6) != 0) return false;
    memcpy(data->link_key, &event->key, sizeof(data->link_key));
    data->num_link_key_notifications++;
    return true;
}

static bool pin_code_request_cb(BteHci *hci, const BteBdAddr *address,
                                void *cb_data)
{
    DeviceData *data = cb_data;

    queue_refresh();
    if (memcmp(address, &data->device.bdaddr, 6) != 0) return false;
    data->num_pin_code_requests++;
    bte_hci_pin_code_req_neg_reply(hci, address, NULL, NULL);
    return true;
}

static void pair_connect_cb(BteHci *hci, const BteHciCreateConnectionReply *reply, void *cb_data)
{
    DeviceData *data = cb_data;

    queue_refresh();

    data->error_code = reply->status;
    if (data->error_code == 0) {
        data->conn_status = CONN_STATUS_CONNECTED;
    } else {
        data->conn_status = CONN_STATUS_DISCONNECTED;
        set_animating(false);
        return;
    }
    //bt_request_authentication((BtAddress*)data->device.bdaddr);
}

static void screen_pair_reset()
{
    DeviceData *data = &s_device_data;

    data->error_code = 0;
    data->l2cap_status = 0;
    data->num_link_key_requests = 0;
    data->num_pin_code_requests = 0;
    data->num_link_key_notifications = 0;
    data->num_authentication_completes = 0;
    data->conn_status = CONN_STATUS_CONNECTING;
    set_animating(true);
    BteHci *hci = bte_hci_get(s_client);
    bte_hci_on_link_key_request(hci, link_key_request_cb);
    bte_hci_on_link_key_notification(hci, link_key_notification_cb);
    bte_hci_on_pin_code_request(hci, pin_code_request_cb);

    bte_hci_create_connection(hci, &data->device.bdaddr, NULL,
                              NULL, pair_connect_cb, data);
}

static void screen_pair_draw()
{
    const DeviceData *data = &s_device_data;

    printf(CONSOLE_RESET "\x1b[2;0H" CONSOLE_YELLOW);
    printf("Pairing to " BD_ADDR_FMT " - %.64s", BD_ADDR_DATA(&data->device.bdaddr), data->device.name);

    printf(CONSOLE_WHITE);
    printf("\x1b[4;0H");

    char anim_char = get_anim_char();

    if (data->conn_status == CONN_STATUS_CONNECTING) {
        printf("Connecting... %c\n", anim_char);
    } else {
        printf("%s      \n", data->conn_status == CONN_STATUS_CONNECTED ?
               "Connecting" : "Connected");
        printf("Error code = %d, status = %d\n", data->error_code, data->l2cap_status);
    }

    printf("Link key requested: %d\n", data->num_link_key_requests);
    printf("PIN code requested: %d\n", data->num_pin_code_requests);
    printf("Link keys received: %d\n", data->num_link_key_notifications);
    for (int i = 0; i < data->num_link_key_notifications; i++) {
        const uint8_t *k = data->link_key;
        printf("  %02x %02x %02x %02x %02x %02x %02x %02x\n"
               "  %02x %02x %02x %02x %02x %02x %02x %02x\n",
               k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7],
               k[8], k[9], k[10], k[11], k[12], k[13], k[14], k[15]);
    }
    printf("Authentication complete: %d\n", data->num_authentication_completes);

    printf(CONSOLE_WHITE CONSOLE_RESET "\x1b[%d;0H", s_screen_h - 4);
    printf("_________________________________\n");
    printf(CONSOLE_WHITE "1 - " CONSOLE_RESET "Back  ");
}

static void screen_pair_process_input(u32 buttons, u32 held)
{
    if (buttons & WPAD_BUTTON_1) {
        pop_screen();
    }
}

static const ScreenMethods s_screens[SCREEN_LAST] = {
    [SCREEN_TITLE] = {
        NULL,
        screen_title_draw,
        screen_title_process_input,
    },
    [SCREEN_PAIRED_DEVICES] = {
        NULL,
        screen_paired_devices_draw,
        screen_paired_devices_process_input,
    },
    [SCREEN_GUEST_DEVICES] = {
        NULL,
        screen_guest_devices_draw,
        screen_guest_devices_process_input,
    },
    [SCREEN_SEARCH_DEVICES] = {
        screen_search_devices_reset,
        screen_search_devices_draw,
        screen_search_devices_process_input,
    },
    [SCREEN_LISTEN] = {
        screen_listen_reset,
        screen_listen_draw,
        screen_listen_process_input,
    },
    [SCREEN_DEVICE] = {
        screen_device_reset,
        screen_device_draw,
        screen_device_process_input,
    },
    [SCREEN_CONNECT] = {
        screen_connect_reset,
        screen_connect_draw,
        screen_connect_process_input,
    },
    [SCREEN_SDP] = {
        screen_sdp_reset,
        screen_sdp_draw,
        screen_sdp_process_input,
        screen_sdp_pop,
    },
    [SCREEN_SDP_HID] = {
        screen_sdp_hid_reset,
        screen_sdp_hid_draw,
        screen_sdp_hid_process_input,
        screen_sdp_hid_pop,
    },
    [SCREEN_HID] = {
        screen_hid_reset,
        screen_hid_draw,
        screen_hid_process_input,
    },
    [SCREEN_PAIR] = {
        screen_pair_reset,
        screen_pair_draw,
        screen_pair_process_input,
    },
};

static const ScreenMethods *current_screen()
{
    return &s_screens[current_screen_id()];
}

void hci_initialized_cb(BteHci *hci, bool success, void *userdata)
{
    s_hci_initialized = true;
}

int main(int argc, char **argv) {

    VIDEO_Init();
    WPAD_Init();

    static GXRModeObj *rmode;
    static void *xfb;

    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    CON_InitEx(rmode, 0, 0, rmode->fbWidth,rmode->xfbHeight);
    CON_GetMetrics(&s_screen_w, &s_screen_h);

    SYS_STDIO_Report(false);
    s_client = bte_client_new();
    bte_hci_on_initialized(bte_hci_get(s_client), hci_initialized_cb, NULL);

    int frames_since_last_refresh = 0;
    while (!s_quit_requested) {
        WPAD_ScanPads();
        u32 pressed = WPAD_ButtonsDown(0);
        u32 held = WPAD_ButtonsHeld(0);
        if (pressed & WPAD_BUTTON_HOME)
            s_quit_requested = true;

        const ScreenMethods *screen = current_screen();
        if (s_screen_needs_refresh ||
            (s_screen_runs_animation &&
             frames_since_last_refresh > FRAMES_BETWEEN_ANIMATION)) {
            frames_since_last_refresh = 0;
            s_screen_needs_refresh = false;

            consoleClear();
            screen->draw();
        }
        if (s_hci_initialized && screen->process_input) screen->process_input(pressed, held);

        VIDEO_WaitVSync();
        frames_since_last_refresh++;
    }

    return EXIT_SUCCESS;
}
