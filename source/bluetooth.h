#ifndef BTT_BLUETOOTH_H
#define BTT_BLUETOOTH_H

#include <gccore.h>

#define BT_LAP_GIAC 0x009E8B33
#define BT_LAP_LIAC 0x009E8B00

typedef struct {
    u8 bdaddr[6];
    u8 class_major;
    u8 class_minor;
} BtDeviceAddr;

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

#endif // BTT_BLUETOOTH_H
