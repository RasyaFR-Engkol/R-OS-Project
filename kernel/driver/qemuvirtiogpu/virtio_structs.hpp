#pragma once
#include <rosval.h>

#define VIRTIO_VENDOR_ID            0x1AF4
#define VIRTIO_GPU_DEVICE_ID        0x1050

// use this for capatiblity types in QEMU VIRTIO GPU
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4
#define VIRTIO_PCI_CAP_PCI_CFG      5

// BIT Status to check Driver Condition
#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FEATURES_OK   8

// GPU Command
enum VirtioGpuCtrlType{
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
    VIRTIO_GPU_CMD_RESOURCE_UNREF,
    VIRTIO_GPU_CMD_SET_SCANOUT,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING,
    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET,
    VIRTIO_GPU_RESP_OK_EDID,
};

// formating
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM 3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM 4

#pragma pack(push, 1)

// PCI Capatibility Modern
struct VirtioPciCap {
    U8 cap_vndr;    // Generic PCI field: PCI_CAP_ID_VNDR
    U8 cap_next;    // Generic PCI field: next ptr.
    U8 cap_len;     // Generic PCI field: capability length
    U8 cfg_type;    // Identifies the structure.
    U8 bar;         // Where to find it.
    U8 padding[3];  // Pad to full dword.
    U32 offset;     // Offset within bar.
    U32 length;     // Length of the structure, in bytes.
};

// Common Config Structure (Mapped in BAR)
struct VirtioPciCommonCfg {
    U32 device_feature_select;
    U32 device_feature;
    U32 driver_feature_select;
    U32 driver_feature;
    U16 msix_config;
    U16 num_queues;
    U8 device_status;
    U8 config_generation;
    
    // Queue Config
    U16 queue_select;
    U16 queue_size;
    U16 queue_msix_vector;
    U16 queue_enable;
    U16 queue_notify_off;
    U32 queue_desc_lo;
    U32 queue_desc_hi;
    U32 queue_avail_lo;
    U32 queue_avail_hi;
    U32 queue_used_lo;
    U32 queue_used_hi;
};

// --- VIRTQUEUE STRUCTURES ---
struct VRingDesc {
    U64 addr;
    U32 len;
    U16 flags;
    U16 next;
};

struct VRingAvail {
    U16 flags;
    U16 idx;
    // [FIX]: Ganti [] jadi [1] agar valid di C++ ISO (-pedantic)
    // Kita tetap bisa akses index > 0 karena kita alokasi memori manual.
    U16 ring[1]; 
};

struct VRingUsedElem {
    U32 id;
    U32 len;
};

struct VRingUsed {
    U16 flags;
    U16 idx;
    // [FIX]: Ganti [] jadi [1] agar valid di C++ ISO (-pedantic)
    VRingUsedElem ring[1];
};

// --- GPU COMMAND HEADERS ---

struct VirtioGpuCtrlHeader {
    U32 type;
    U32 flags;
    U64 fence_id;
    U32 ctx_id;
    U32 padding;
};

struct VirtioGpuRect {
    U32 x, y, width, height;
};

struct VirtioGpuResourceCreate2D {
    VirtioGpuCtrlHeader hdr;
    U32 resource_id;
    U32 format;
    U32 width;
    U32 height;
};

struct VirtioGpuResourceAttachBacking {
    VirtioGpuCtrlHeader hdr;
    U32 resource_id;
    U32 nr_entries;
};

struct VirtioGpuMemEntry {
    U64 addr;
    U32 length;
    U32 padding;
};

struct VirtioGpuSetScanout {
    VirtioGpuCtrlHeader hdr;
    VirtioGpuRect r;
    U32 scanout_id;
    U32 resource_id;
};

struct VirtioGpuTransferToHost2D {
    VirtioGpuCtrlHeader hdr;
    VirtioGpuRect r;
    U64 offset;
    U32 resource_id;
    U32 padding;
};

struct VirtioGpuRespDisplayInfo {
    VirtioGpuCtrlHeader hdr;
    struct {
        VirtioGpuRect r;
        U32 enabled;
        U32 flags;
    } pmodes[16];
};

struct VirtioGpuResourceFlush {
    VirtioGpuCtrlHeader hdr;
    VirtioGpuRect r;
    U32 resource_id;
    U32 padding;
};

#pragma pack(pop)