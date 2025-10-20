#include "bluetooth.h"

#include <gccore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wiiuse/wpad.h>

#define MAX_SEARCH_DEVICES 10

static int s_screen_w, s_screen_h;
static bool s_quit_requested = false;

typedef enum {
    SCREEN_TITLE,
    SCREEN_PAIRED_DEVICES,
    SCREEN_GUEST_DEVICES,
    SCREEN_SEARCH_DEVICES,
    SCREEN_LAST,
} ScreenId;

typedef enum {
    ACTION_FIRST = SCREEN_LAST,
    ACTION_QUIT = ACTION_FIRST,
} ActionId;

static ScreenId s_screen_stack[10] = { SCREEN_TITLE };
static int s_screen_index = 0;

static conf_pads s_paired_devices;
static conf_pad_guests s_guest_devices;

typedef struct {
    void (*draw)(void);
    void (*process_input)(u32 buttons);
} ScreenMethods;

typedef struct {
    ActionId action_id;
    const char *label;
} ActionItem;

static const ActionItem s_title_screen_items[] = {
    { SCREEN_PAIRED_DEVICES, "See paired devices", },
    { SCREEN_GUEST_DEVICES, "See guest devices", },
    { SCREEN_SEARCH_DEVICES, "Search nearby devices", },
    { ACTION_QUIT, "Quit", },
};
#define TITLE_NUM_SCREENS \
    (sizeof(s_title_screen_items) / sizeof(s_title_screen_items[0]))
static int s_title_item_index = 0;

typedef struct {
    u8 bdaddr[6];
    u8 class_major;
    u8 class_minor;
    char name[0x40];
    bool querying_name;
    bool queried_name;
} DeviceEntry;

typedef struct {
    u32 lap;
    bool search_running;
    int error_code;
    int num_devices;
    DeviceEntry devices[MAX_SEARCH_DEVICES];
} SearchDeviceData;

static SearchDeviceData s_search_device_data = {
    BT_LAP_GIAC,
    false,
    0,
    0,
};

static void retrive_device_names(SearchDeviceData *data);

static int sprintf_bdaddr(char *dest, const u8 *bdaddr)
{
    return sprintf(dest, "%02x:%02x:%02x:%02x:%02x:%02x",
                   bdaddr[0], bdaddr[1], bdaddr[2], bdaddr[3], bdaddr[4],
                   bdaddr[5]);
}

static const char *describe_device(u8 major, u8 minor)
{
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
    consoleClear();
    s_screen_stack[++s_screen_index] = id;
}

static ScreenId current_screen(void)
{
    return s_screen_stack[s_screen_index];
}

static void pop_screen()
{
    consoleClear();
    if (s_screen_index > 0) s_screen_index--;
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

        int bold, bgcolor;
        if (i == s_title_item_index) {
            bold = 1;
            bgcolor = CONSOLE_COLOR_BLUE;
        } else {
            bold = 0;
            bgcolor = CONSOLE_COLOR_BLACK;
        }
        printf("\x1b[%d;%dm%s\n", bgcolor + 40, bold, item->label);
    }

    printf(CONSOLE_WHITE CONSOLE_RESET "\x1b[%d;0H", s_screen_h - 4);
    printf("_________________________________\n");
    printf("Move with arrows, select with "
           CONSOLE_WHITE "2");
}

void screen_title_process_input(u32 buttons)
{
    if (buttons & WPAD_BUTTON_2) {
        ActionId action_id = s_title_screen_items[s_title_item_index].action_id;
        if (action_id < ACTION_FIRST) {
            push_screen(action_id);
        } else if (action_id == ACTION_QUIT) {
            s_quit_requested = true;
        }
    } else if (buttons & (WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT)) {
        if (s_title_item_index + 1 < TITLE_NUM_SCREENS)
            s_title_item_index++;
    } else if (buttons & (WPAD_BUTTON_UP | WPAD_BUTTON_RIGHT)) {
        if (s_title_item_index > 0)
            s_title_item_index--;
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
        sprintf_bdaddr(addr_buffer, device->bdaddr);
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

void screen_paired_devices_process_input(u32 buttons)
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

        char addr_buffer[20];
        sprintf_bdaddr(addr_buffer, device->bdaddr);
        printf("% 2d) %s - \"%.64s\"\n", i + 1, addr_buffer, device->name);
    }

    if (s_guest_devices.num_guests == 0)
        printf("No guest devices registered");

    printf(CONSOLE_WHITE CONSOLE_RESET "\x1b[%d;0H", s_screen_h - 4);
    printf("_________________________________\n");
    printf(CONSOLE_WHITE "1 - " CONSOLE_RESET "Back  ");
}

void screen_guest_devices_process_input(u32 buttons)
{
    if (buttons & WPAD_BUTTON_1) {
        pop_screen();
    }
}

static void screen_search_devices_draw()
{
    consoleClear();
    printf(CONSOLE_RESET "\x1b[2;0H" CONSOLE_YELLOW);
    printf("SEARCH DEVICES");

    printf(CONSOLE_WHITE);
    printf("\x1b[4;0H");

    const SearchDeviceData *data = &s_search_device_data;

    static const char wait_seq[] = "\\|/-";
    static int last_char = 0;
    char anim_char = wait_seq[last_char++ % 4];

    if (data->search_running) {
        printf("  Searching... %c\n", anim_char);
    } else {
        if (data->error_code) {
            printf("Error code: %d\n\n", data->error_code);
        }

        for (int i = 0; i < data->num_devices; i++) {
            const DeviceEntry *device = &data->devices[i];

            char addr_buffer[20];
            sprintf_bdaddr(addr_buffer, device->bdaddr);

            char text[100];
            if (device->queried_name) {
                sprintf(text, CONSOLE_WHITE "%.64s" CONSOLE_RESET, device->name);
            } else if (device->querying_name) {
                sprintf(text, "Retrieving name... %c", anim_char);
            } else {
                sprintf(text, "Queued for name retrieval");
            }
            const char *class_desc = describe_device(device->class_major,
                                                     device->class_minor);
            printf("% 2d) %s - (%s) %s\n", i + 1, addr_buffer, class_desc, text);
        }

        if (data->num_devices == 0)
            printf("No devices found");
    }

    const char *search_type = (data->lap == BT_LAP_GIAC) ? "General" : "Limited";
    printf(CONSOLE_WHITE CONSOLE_RESET "\x1b[%d;0H", s_screen_h - 4);
    printf("_________________________________\n");
    printf(CONSOLE_WHITE "1 - " CONSOLE_RESET "Back  ");
    printf(CONSOLE_WHITE "2 - " CONSOLE_RESET "Search (%s)\n", search_type);
    printf(CONSOLE_WHITE "A - " CONSOLE_RESET "Switch search type");
}

static bool device_in_list(const u8 *bdaddr, const DeviceEntry *devices, int num_devices)
{
    for (int i = 0; i < num_devices; i++) {
        if (memcmp(bdaddr, devices[i].bdaddr, 6) == 0)
            return true;
    }
    return false;
}

static void on_name_retrieved(const BtReadRemoteNameResult *result, void *cb_data)
{
    SearchDeviceData *data = cb_data;

    for (int i = 0; i < data->num_devices; i++) {
        DeviceEntry *device = &data->devices[i];
        if (device->querying_name) {
            memcpy(device->name, result->name, sizeof(device->name));
            device->queried_name = true;
            device->querying_name = false;
            break;
        }
    }

    retrive_device_names(data);
}

static void retrive_device_names(SearchDeviceData *data)
{
    for (int i = 0; i < data->num_devices; i++) {
        DeviceEntry *device = &data->devices[i];
        if (device->querying_name) {
            /* Let's wait for this operation to complete */
            break;
        }

        if (!device->queried_name) {
            device->querying_name = true;
            bt_read_remote_name(device->bdaddr, on_name_retrieved, data);
            break;
        }
    }
}

static void search_devices_cb(const BtScanResult *result, void *cb_data)
{
    SearchDeviceData *data = cb_data;
    data->error_code = result->error_code;
    int num_devices = 0;
    data->num_devices = result->num_devices;
    if (data->num_devices > MAX_SEARCH_DEVICES)
        data->num_devices = MAX_SEARCH_DEVICES;
    for (int i = 0; i < result->num_devices; i++) {
        const u8 *bdaddr = result->devices[i].bdaddr;
        if (device_in_list(bdaddr, data->devices, num_devices)) {
            /* Duplicate, ignoring */
            continue;
        }
        memset(&data->devices[num_devices], 0, sizeof(data->devices[num_devices]));
        memcpy(data->devices[num_devices].bdaddr, bdaddr, 6);
        data->devices[num_devices].class_major = result->devices[i].class_major;
        data->devices[num_devices].class_minor = result->devices[i].class_minor;
        num_devices++;
        if (num_devices >= MAX_SEARCH_DEVICES) break;
    }

    data->num_devices = num_devices;
    data->search_running = false;
    retrive_device_names(data);
}

static void screen_search_devices_process_input(u32 buttons)
{
    if (buttons & WPAD_BUTTON_1) {
        pop_screen();
    } else if (buttons & WPAD_BUTTON_2) {
        s_search_device_data.search_running = true;
        bt_scan(s_search_device_data.lap,
                search_devices_cb, &s_search_device_data);
    } else if (buttons & WPAD_BUTTON_A) {
        s_search_device_data.lap = (s_search_device_data.lap == BT_LAP_GIAC) ?
            BT_LAP_LIAC : BT_LAP_GIAC;
    }
}

static const ScreenMethods s_screens[SCREEN_LAST] = {
    [SCREEN_TITLE] = {
        screen_title_draw,
        screen_title_process_input,
    },
    [SCREEN_PAIRED_DEVICES] = {
        screen_paired_devices_draw,
        screen_paired_devices_process_input,
    },
    [SCREEN_GUEST_DEVICES] = {
        screen_guest_devices_draw,
        screen_guest_devices_process_input,
    },
    [SCREEN_SEARCH_DEVICES] = {
        screen_search_devices_draw,
        screen_search_devices_process_input,
    },
};

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

	while (!s_quit_requested) {
		WPAD_ScanPads();
		u32 pressed = WPAD_ButtonsDown(0);
		if (pressed & WPAD_BUTTON_HOME)
            s_quit_requested = true;

        const ScreenMethods *screen = &s_screens[current_screen()];
        screen->draw();
        if (screen->process_input) screen->process_input(pressed);

		VIDEO_WaitVSync();
	}

	return EXIT_SUCCESS;
}
