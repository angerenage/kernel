#pragma once

#define XHCI_OP_USBCMD 0x00u
#define XHCI_OP_USBSTS 0x04u
#define XHCI_OP_PAGESIZE 0x08u
#define XHCI_OP_CRCR 0x18u
#define XHCI_OP_DCBAAP 0x30u
#define XHCI_OP_CONFIG 0x38u
#define XHCI_CMD_RS (1u << 0u)
#define XHCI_CMD_HCRST (1u << 1u)
#define XHCI_STS_HCH (1u << 0u)
#define XHCI_STS_HSE (1u << 2u)
#define XHCI_STS_CNR (1u << 11u)
#define XHCI_RT_IR0 0x20u
#define XHCI_IR_IMAN 0x00u
#define XHCI_IR_IMOD 0x04u
#define XHCI_IR_ERSTSZ 0x08u
#define XHCI_IR_ERSTBA 0x10u
#define XHCI_IR_ERDP 0x18u
#define XHCI_TRB_CYCLE 1u
#define XHCI_TRB_TYPE_SHIFT 10u
#define XHCI_TRB_LINK 6u
#define XHCI_TRB_NOOP_CMD 23u
#define XHCI_TRB_COMMAND_COMPLETION 33u
#define XHCI_TRB_COMPLETION_SHIFT 24u
#define XHCI_TRB_SUCCESS 1u
