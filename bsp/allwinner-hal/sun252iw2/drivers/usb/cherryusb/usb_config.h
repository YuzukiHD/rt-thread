/*
 * Copyright (c) 2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CherryUSB configuration for the sun252iw2: MUSB device (4 endpoints), EHCI/OHCI
 * host with the OHCI companion behind the EHCI registers.
 */
#ifndef CHERRYUSB_CONFIG_H
#define CHERRYUSB_CONFIG_H

#ifdef __RTTHREAD__
#include <rtthread.h>
#define CONFIG_USB_PRINTF(...) rt_kprintf(__VA_ARGS__)
#else
#define CONFIG_USB_PRINTF(...) printf(__VA_ARGS__)
#endif

#ifndef CONFIG_USB_DBG_LEVEL
#define CONFIG_USB_DBG_LEVEL USB_DBG_INFO
#endif

/* the controllers are DMA masters: the CPU maintains the data cache */
#define CONFIG_USB_DCACHE_ENABLE
#ifdef CONFIG_USB_DCACHE_ENABLE
#define CONFIG_USB_ALIGN_SIZE 64
#else
#define CONFIG_USB_ALIGN_SIZE 4
#endif

/* the buffers live in cached memory, the glue maintains the cache */
#define USB_NOCACHE_RAM_SECTION

/* ---------------- device (MUSB) ---------------- */
#define CONFIG_USBDEV_MAX_BUS 1
#define CONFIG_USBDEV_EP_NUM 8
#define CONFIG_USBDEV_SETUP_LOG_PRINT 0
#define CONFIG_USBDEV_TX_RX_THREAD_STACK_SIZE 2048
#define CONFIG_USBDEV_TX_RX_PRIO 4
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512
#define CONFIG_USBDEV_MSC_MAX_LUN 1
#define CONFIG_USBDEV_MSC_MAX_BUFSIZE 512
#define CONFIG_USB_MUSB_EP_NUM 4
#define CONFIG_USB_MUSB_SUNXI
/* the endpoint FIFOs are moved by the OTG DMA engine (undefine for CPU copies) */
#define CONFIG_USB_MUSB_DMA

/* ---------------- host (EHCI + OHCI) ---------------- */
#define CONFIG_USBHOST_MAX_RHPORTS 1
#define CONFIG_USBHOST_MAX_EXTHUBS 1
#define CONFIG_USBHOST_MAX_EHPORTS 4
#define CONFIG_USBHOST_MAX_INTERFACES 8
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS 2
#define CONFIG_USBHOST_MAX_ENDPOINTS 4
#define CONFIG_USBHOST_MAX_MSC_CLASS 2
#define CONFIG_USBHOST_MAX_HID_CLASS 4
#define CONFIG_USBHOST_MAX_SERIAL_CLASS 4
#define CONFIG_USBHOST_DEV_NAMELEN 16
#define CONFIG_USBHOST_PSC_PRIO 0
#define CONFIG_USBHOST_PSC_STACKSIZE 2048
#define CONFIG_USBHOST_REQUEST_BUFFER_LEN 512
#define CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT 500
#define CONFIG_USBHOST_MSC_TIMEOUT 5000
#define CONFIG_USBHOST_MAX_BUS 1
#define CONFIG_USBHOST_SERIAL_RX_SIZE 2048
#define CONFIG_USBHOST_SERIAL_TX_SIZE 2048

#define CONFIG_USB_EHCI_HCCR_OFFSET (0x0)
#define CONFIG_USB_EHCI_FRAME_LIST_SIZE 1024
#define CONFIG_USB_EHCI_QH_NUM 10
#define CONFIG_USB_EHCI_QTD_NUM (CONFIG_USB_EHCI_QH_NUM * 3)
#define CONFIG_USB_EHCI_ITD_NUM 4
#define CONFIG_USB_EHCI_CONFIGFLAG
#define CONFIG_USB_EHCI_WITH_OHCI
#define CONFIG_USB_EHCI_DESC_DCACHE_ENABLE
#define CONFIG_USB_OHCI_HCOR_OFFSET (0x400)
#define CONFIG_USB_OHCI_ED_NUM 10
#define CONFIG_USB_OHCI_TD_NUM 18
#define CONFIG_USB_OHCI_DESC_DCACHE_ENABLE

#define CONFIG_USB_MUSB_PIPE_NUM 4
#define CONFIG_USB_HS

#ifndef usb_phyaddr2ramaddr
#define usb_phyaddr2ramaddr(addr) (addr)
#endif
#ifndef usb_ramaddr2phyaddr
#define usb_ramaddr2phyaddr(addr) (addr)
#endif

#endif
