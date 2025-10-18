#include "bluetooth.h"

#include <gccore.h>
#include "hci.h"

#define LAP_GIAC 0x009E8B33
#define LAP_LIAC 0x009E8B00

#define MAX_SCAN_RESULTS 10

typedef struct {
    BtScanCb callback;
    void *cb_data;
} ScanData;

typedef struct {
    BtReadRemoteNameCb callback;
    void *cb_data;
} ReadRemoteNameData;

static ScanData s_scan_data;
static ReadRemoteNameData s_read_remote_name_data;
static BtDeviceAddr s_bt_devices[MAX_SCAN_RESULTS];

static err_t inquiry_cb(void *arg, struct hci_pcb *pcb, struct hci_inq_res *ires, u16_t result)
{
    ScanData *data = arg;
    BtScanResult scan_result;
    u32 level;

    memset(&scan_result, 0, sizeof(scan_result));
    scan_result.error_code = result;
    if (result != HCI_SUCCESS) {
        _CPU_ISR_Disable(level);
        data->callback(&scan_result, data->cb_data);
        _CPU_ISR_Restore(level);
        return HCI_SUCCESS;
    }

    for (struct hci_inq_res *p = ires; p != NULL; p = p->next) {
        if (scan_result.num_devices >= MAX_SCAN_RESULTS) break;

        BtDeviceAddr *device = &s_bt_devices[scan_result.num_devices++];
        memcpy(device->bdaddr, &p->bdaddr, 6);
        device->class_major = p->cod[1] & 0x1f;
        device->class_minor = p->cod[0] >> 2;
    }
    if (scan_result.num_devices > 0) {
        scan_result.devices = s_bt_devices;
    }

    _CPU_ISR_Disable(level);
    data->callback(&scan_result, data->cb_data);
    _CPU_ISR_Restore(level);
    return HCI_SUCCESS;
}

void bt_scan(BtScanCb callback, void *cb_data)
{
    u32 level;
    int max_cnt = MAX_SCAN_RESULTS;

    s_scan_data.callback = callback;
    s_scan_data.cb_data = cb_data;

    _CPU_ISR_Disable(level);
    hci_arg(&s_scan_data);
    hci_inquiry(LAP_GIAC, 0x03, max_cnt, inquiry_cb);
    _CPU_ISR_Restore(level);

}

static err_t read_remote_name_cb(void *arg, struct bd_addr *bdaddr, u8_t *name, u8_t result)
{
    ReadRemoteNameData *data = arg;
    BtReadRemoteNameResult read_result;
    u32 level;

    memset(&read_result, 0, sizeof(read_result));
    read_result.error_code = result;
    if (result == HCI_SUCCESS) {
        memcpy(read_result.name, name, sizeof(read_result.name));
    }

    _CPU_ISR_Disable(level);
    data->callback(&read_result, data->cb_data);
    _CPU_ISR_Restore(level);
    return HCI_SUCCESS;
}

void bt_read_remote_name(const u8 *device_addr,
                         BtReadRemoteNameCb callback, void *cb_data)
{
    u32 level;

    s_read_remote_name_data.callback = callback;
    s_read_remote_name_data.cb_data = cb_data;

    _CPU_ISR_Disable(level);
    hci_arg(&s_read_remote_name_data);
    hci_remote_name_req_complete(read_remote_name_cb);
    hci_read_remote_name((struct bd_addr *)device_addr);
    _CPU_ISR_Restore(level);
}
