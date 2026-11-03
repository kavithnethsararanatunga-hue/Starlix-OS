/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Kavith Ranatunga
 */

#pragma once

#ifndef STARLIX_PRIMITIVE_TYPES_DEFINED
#define STARLIX_PRIMITIVE_TYPES_DEFINED
using int8_t    = signed char;
using uint8_t   = unsigned char;
using int16_t   = signed short;
using uint16_t  = unsigned short;
using int32_t   = signed int;
using uint32_t  = unsigned int;
using int64_t   = signed long long;
using uint64_t  = unsigned long long;
using uintptr_t = unsigned long long;
using size_t    = unsigned long long;
#endif

namespace starlix::drivers::ehci {

#pragma pack(push, 1)
struct EHCI_CAP_REGS {
    uint8_t  caplength;
    uint8_t  reserved;
    uint16_t hciversion;
    uint32_t hcsparams;
    uint32_t hccparams;
    uint32_t hcsp_portroute;
};

struct EHCI_OP_REGS {
    volatile uint32_t usbcmd;
    volatile uint32_t usbsts;
    volatile uint32_t usbintr;
    volatile uint32_t frindex;
    volatile uint32_t ctrldssegment;
    volatile uint32_t periodiclistbase;
    volatile uint32_t asynclistaddr;
    volatile uint32_t reserved0[9];
    volatile uint32_t configflag;
    volatile uint32_t portsc[16];
};

struct USB_SETUP_PACKET {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};

struct USB_DEVICE_DESCRIPTOR {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
};

struct USB_CONFIG_DESCRIPTOR {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
};
#pragma pack(pop)

/* The hardware qTD is exactly 32 bytes. Alignment is applied to storage,
   not to the type, so it can also appear at QH offset 0x10 as the overlay. */
struct EHCI_QTD {
    volatile uint32_t next_qtd;
    volatile uint32_t alt_next_qtd;
    volatile uint32_t token;
    volatile uint32_t buffer[5];
};

struct alignas(32) EHCI_QH {
    volatile uint32_t horizontal_link;
    volatile uint32_t ep_characteristics;
    volatile uint32_t ep_capabilities;
    volatile uint32_t current_qtd;
    EHCI_QTD overlay;
};

/* EHCI isochronous transfer descriptor: 32-byte aligned, 64-byte hardware ABI. */
struct alignas(32) EHCI_ITD {
    volatile uint32_t next_link;
    volatile uint32_t transaction[8];
    volatile uint32_t buffer[7];
};

static_assert(sizeof(EHCI_CAP_REGS) == 0x10, "EHCI capability register layout mismatch");
static_assert(sizeof(EHCI_QTD) == 32, "EHCI qTD must be 32 bytes");
static_assert(sizeof(EHCI_ITD) == 64, "EHCI iTD must be 64 bytes");
static_assert(alignof(EHCI_ITD) == 32, "EHCI iTD must be 32-byte aligned");
static_assert(__builtin_offsetof(EHCI_QH, overlay) == 0x10, "EHCI QH overlay offset mismatch");

constexpr uint32_t CMD_RUN          = 1u << 0;
constexpr uint32_t CMD_RESET        = 1u << 1;
constexpr uint32_t CMD_PERIODIC_EN  = 1u << 4;
constexpr uint32_t CMD_ASYNC_EN     = 1u << 5;
constexpr uint32_t CMD_ASYNC_ADVANCE= 1u << 6;

constexpr uint32_t STS_INT            = 1u << 0;
constexpr uint32_t STS_ERR            = 1u << 1;
constexpr uint32_t STS_PORT_CHANGE    = 1u << 2;
constexpr uint32_t STS_FRAME_ROLLOVER = 1u << 3;
constexpr uint32_t STS_HOST_ERROR     = 1u << 4;
constexpr uint32_t STS_ASYNC_ADVANCE  = 1u << 5;
constexpr uint32_t STS_PERIODIC       = 1u << 14;
constexpr uint32_t STS_ASYNC          = 1u << 15;
constexpr uint32_t STS_HALTED         = 1u << 12;
constexpr uint32_t STS_W1C_MASK       = 0x3Fu;

constexpr uint32_t HCS_N_PORTS_MASK   = 0x0Fu;
constexpr uint32_t HCS_PPC            = 1u << 4;
constexpr uint32_t HCS_N_CC_MASK      = 0x0F00u;
constexpr uint32_t HCS_N_CC_SHIFT     = 8;
constexpr uint32_t HCS_N_PCC_MASK     = 0xF000u;
constexpr uint32_t HCS_N_PCC_SHIFT    = 12;
constexpr uint32_t HCC_64BIT          = 1u << 0;
constexpr uint32_t HCC_EECP_MASK      = 0x0000FF00u;
constexpr uint32_t HCC_EECP_SHIFT     = 8;

constexpr uint32_t PORT_CONNECT       = 1u << 0;
constexpr uint32_t PORT_CONNECT_CHG   = 1u << 1;
constexpr uint32_t PORT_ENABLE        = 1u << 2;
constexpr uint32_t PORT_ENABLE_CHG    = 1u << 3;
constexpr uint32_t PORT_OVERCURRENT   = 1u << 4;
constexpr uint32_t PORT_OVERCURRENT_CHG = 1u << 5;
constexpr uint32_t PORT_RESUME        = 1u << 6;
constexpr uint32_t PORT_SUSPEND       = 1u << 7;
constexpr uint32_t PORT_RESET         = 1u << 8;
constexpr uint32_t PORT_LINE_STATUS_MASK = 3u << 10;
constexpr uint32_t PORT_POWER         = 1u << 12;
constexpr uint32_t PORT_OWNER         = 1u << 13;
constexpr uint32_t PORT_CHANGE_MASK   = PORT_CONNECT_CHG | PORT_ENABLE_CHG | PORT_OVERCURRENT_CHG;
constexpr uint32_t PORT_SAFE_CONTROL_MASK = PORT_ENABLE | PORT_RESUME | PORT_SUSPEND | PORT_POWER | PORT_OWNER | PORT_RESET;

constexpr uint32_t LINK_TERMINATE     = 1u;
constexpr uint32_t LINK_TYPE_QH       = 1u << 1;
constexpr uint32_t QH_HEAD            = 1u << 15;
constexpr uint32_t QH_TOGGLE_CONTROL  = 1u << 14;
constexpr uint32_t QH_SPEED_HIGH      = 2u << 12;
constexpr uint32_t QH_MAX_PACKET_SHIFT= 16;

constexpr uint32_t QTD_PID_OUT        = 0;
constexpr uint32_t QTD_PID_IN         = 1;
constexpr uint32_t QTD_PID_SETUP      = 2;
constexpr uint32_t QTD_ACTIVE         = 1u << 7;
constexpr uint32_t QTD_HALTED         = 1u << 6;
constexpr uint32_t QTD_DATA_BUFFER    = 1u << 5;
constexpr uint32_t QTD_BABBLE         = 1u << 4;
constexpr uint32_t QTD_XACT_ERROR     = 1u << 3;
constexpr uint32_t QTD_MISSED_UFRAME  = 1u << 2;
constexpr uint32_t QTD_ERROR_MASK     = QTD_HALTED | QTD_DATA_BUFFER | QTD_BABBLE | QTD_XACT_ERROR | QTD_MISSED_UFRAME;
constexpr uint32_t QTD_CERR_3         = 3u << 10;
constexpr uint32_t QTD_IOC            = 1u << 15;
constexpr uint32_t QTD_BYTES_SHIFT    = 16;
constexpr uint32_t QTD_BYTES_MASK     = 0x7FFFu << QTD_BYTES_SHIFT;
constexpr uint32_t QTD_TOGGLE         = 1u << 31;

constexpr uint8_t USB_REQ_GET_DESCRIPTOR = 6;
constexpr uint8_t USB_REQ_SET_ADDRESS    = 5;
constexpr uint8_t USB_REQ_SET_CONFIGURATION = 9;
constexpr uint8_t USB_REQ_CLEAR_FEATURE  = 1;
constexpr uint16_t USB_FEATURE_ENDPOINT_HALT = 0;
constexpr uint8_t USB_DESC_DEVICE        = 1;
constexpr uint8_t USB_DESC_CONFIGURATION = 2;
constexpr uint8_t USB_DESC_INTERFACE     = 4;
constexpr uint8_t USB_DESC_ENDPOINT      = 5;
constexpr uint8_t USB_CLASS_MASS_STORAGE = 8;
constexpr uint8_t USB_MSC_SUBCLASS_SCSI  = 6;
constexpr uint8_t USB_MSC_PROTOCOL_BOT   = 0x50;

} // namespace starlix::drivers::ehci
