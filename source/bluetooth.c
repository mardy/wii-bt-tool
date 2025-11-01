#include "bluetooth.h"

#include <gccore.h>
#include "hci.h"
#include "l2cap.h"
#include "btpbuf.h"

#define MAX_SCAN_RESULTS 10

struct _bt_l2cap_handle {
    struct l2cap_pcb *pcb;
    BtL2capNotify notify_cb;
    void *notify_cb_data;
};

typedef struct {
    BtScanCb callback;
    void *cb_data;
} ScanData;

typedef struct {
    BtReadRemoteNameCb callback;
    void *cb_data;
} ReadRemoteNameData;

typedef struct {
    BtConnectCb callback;
    void *cb_data;
    struct l2cap_pcb *pcb;
} ConnectData;

typedef struct {
    BtConnectionRequestCb callback;
    void *cb_data;
} ConnectionRequestData;

static ScanData s_scan_data;
static ReadRemoteNameData s_read_remote_name_data;
static ConnectData s_connect_data;
static ConnectionRequestData s_connection_request_data;
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

void bt_scan(u32 lap, BtScanCb callback, void *cb_data)
{
    u32 level;
    int max_cnt = MAX_SCAN_RESULTS;

    s_scan_data.callback = callback;
    s_scan_data.cb_data = cb_data;

    _CPU_ISR_Disable(level);
    hci_arg(&s_scan_data);
    hci_inquiry(lap, 0x03, max_cnt, inquiry_cb);
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

static BtL2capHandle *bt_l2cap_handle_new(struct l2cap_pcb *pcb)
{
    BtL2capHandle *handle = malloc(sizeof(BtL2capHandle));
    memset(handle, 0, sizeof(*handle));
    handle->pcb = pcb;
    return handle;
}

void bt_l2cap_handle_notify(BtL2capHandle *handle,
                            BtL2capNotify callback, void *cb_data)
{
    handle->notify_cb = callback;
    handle->notify_cb_data = cb_data;
}

int bt_l2cap_handle_write(BtL2capHandle *handle, const void *data, size_t len)
{
    if (!handle || !handle->pcb) return -1;

    struct pbuf *p = btpbuf_alloc(PBUF_RAW, len, PBUF_RAM);
    if (!p) return -2;

    memcpy(p->payload, data, len);
    err_t err = l2ca_datawrite(handle->pcb, p);
    btpbuf_free(p);

    return err;
}

void bt_l2cap_handle_close(BtL2capHandle *handle)
{
    if (!handle) return;
    if (handle->pcb) {
        l2cap_close(handle->pcb);
    }
    free(handle);
}

static err_t process_input(void *arg, struct l2cap_pcb *pcb, struct pbuf *p, err_t err)
{
    BtL2capHandle *handle = arg;
    if (!handle->notify_cb) return ERR_OK;

    handle->notify_cb(handle, p->payload, p->tot_len, handle->notify_cb_data);
    return ERR_OK;
}

static err_t connect_cb(void *arg, struct l2cap_pcb *lpcb,
                        u16_t result, u16_t status)
{
    ConnectData *data = arg;

    BtConnectResult r = {
        result,
        status,
        bt_l2cap_handle_new(lpcb),
    };
    if (result == L2CAP_CONN_SUCCESS) {
        l2cap_arg(lpcb, r.handle);
        l2cap_recv(lpcb, process_input);
    }
    data->callback(&r, data->cb_data);
    data->pcb = NULL;
    return ERR_OK;
}

void bt_connect(const u8 *device_addr,
                bool allow_role_switch,
                u16 psm,
                BtConnectCb callback, void *cb_data)
{
    u32 level;

    if (s_connect_data.pcb) {
        l2cap_close(s_connect_data.pcb);
        s_connect_data.pcb = NULL;
    }
    s_connect_data.callback = callback;
    s_connect_data.cb_data = cb_data;
    s_connect_data.pcb = l2cap_new();
    s_connect_data.pcb->callback_arg = &s_connect_data;

    _CPU_ISR_Disable(level);
    l2ca_connect_req(s_connect_data.pcb,
                     (struct bd_addr *)device_addr,
                     psm,
                     allow_role_switch ? HCI_ALLOW_ROLE_SWITCH : 0,
                     connect_cb);
    _CPU_ISR_Restore(level);
}

static err_t connection_request_cb(void *arg, struct bd_addr *bdaddr,
                                   u8_t *cod, u8_t link_type)
{
    BtConnectionRequestData data;
    u32 level;

    memcpy(data.bdaddr, bdaddr, sizeof(data.bdaddr));
    data.device_class[0] = cod[0];
    data.device_class[1] = cod[1];
    data.device_class[2] = cod[2];
    data.link_type = link_type;
    _CPU_ISR_Disable(level);
    bool accept = s_connection_request_data.callback(
        &data, s_connection_request_data.cb_data);
    _CPU_ISR_Restore(level);
    return accept ? ERR_OK : ERR_CONN;
}

void bt_on_connection_request(BtConnectionRequestCb callback,
                              void *cb_data)
{
    s_connection_request_data.callback = callback;
    s_connection_request_data.cb_data = cb_data;
    hci_conn_req(connection_request_cb);
}

void bt_set_visible(BtVisibilityType type)
{
    hci_write_scan_enable((u8)type);
}

void bt_set_local_name(const char *name)
{
    hci_write_local_name((u8*)name, strlen(name) + 1);
}
