#ifndef BTT_BLUETOOTH_H
#define BTT_BLUETOOTH_H

#include <gccore.h>

#define BT_LAP_GIAC 0x009E8B33
#define BT_LAP_LIAC 0x009E8B00

#define BT_PSM_SDP 0x0001
#define BT_PSM_HID_CONTROL 0x0011
#define BT_PSM_HID_INTR 0x0013

typedef struct {
    u8 bdaddr[6];
    u8 class_major;
    u8 class_minor;
} BtDeviceAddr;

typedef struct _bt_l2cap_handle BtL2capHandle;

typedef struct {
    int error_code;
    int num_devices;
    /* Array is valid until next scan */
    BtDeviceAddr *devices;
} BtScanResult;

typedef void (*BtScanCb)(const BtScanResult *result, void *cb_data);
void bt_scan(u32 lap, BtScanCb callback, void *cb_data);

typedef struct {
    int error_code;
    char name[64];
} BtReadRemoteNameResult;

typedef void (*BtReadRemoteNameCb)(const BtReadRemoteNameResult *result, void *cb_data);
void bt_read_remote_name(const u8 *device_addr, BtReadRemoteNameCb callback, void *cb_data);

typedef struct {
    int error_code;
    int status;
    BtL2capHandle *handle;
} BtConnectResult;

typedef void (*BtConnectCb)(const BtConnectResult *result, void *cb_data);
void bt_connect(const u8 *device_addr, bool allow_role_switch, u16 psm,
                BtConnectCb callback, void *cb_data);

typedef struct {
    u8 bdaddr[6];
    u8 device_class[3];
    u8 link_type;
} BtConnectionRequestData;

typedef bool (*BtConnectionRequestCb)(const BtConnectionRequestData *event,
                                      void *cb_data);
void bt_on_connection_request(BtConnectionRequestCb callback, void *cb_data);

typedef enum {
    BT_VISIBILITY_NONE = 0,
    BT_VISIBILITY_INQUIRY = 1 << 0,
    BT_VISIBILITY_PAGE = 1 << 1,
    BT_VISIBILITY_ALL = BT_VISIBILITY_INQUIRY | BT_VISIBILITY_PAGE,
} BtVisibilityType;

void bt_set_visible(BtVisibilityType type);
void bt_set_local_name(const char *name);

typedef void (*BtL2capNotify)(BtL2capHandle *handle, void *data, size_t len,
                              void *cb_data);
void bt_l2cap_handle_notify(BtL2capHandle *handle,
                            BtL2capNotify callback, void *cb_data);
int bt_l2cap_handle_write(BtL2capHandle *handle, const void *data, size_t len);
void bt_l2cap_handle_close(BtL2capHandle *handle);

#endif // BTT_BLUETOOTH_H
