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

typedef void (*BtL2capNotify)(BtL2capHandle *handle, void *data, size_t len,
                              void *cb_data);
void bt_l2cap_handle_notify(BtL2capHandle *handle,
                            BtL2capNotify callback, void *cb_data);
int bt_l2cap_handle_write(BtL2capHandle *handle, const void *data, size_t len);
void bt_l2cap_handle_close(BtL2capHandle *handle);

#endif // BTT_BLUETOOTH_H
