/*
 * Phoenix-RTOS
 *
 * USB driver Communications Devces Class definitions
 *
 * Copyright 2018 Phoenix Systems
 * Author: Kamil Amanowicz
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _USB_CDC_H_
#define _USB_CDC_H_

/* device class code */
#define USB_CLASS_CDC 0x2

/* interface class code */
#define USB_INTERFACE_COMMUNICATIONS 0x2
#define USB_INTERFACE_DATA 0xA

/* communications class subclass codes */
#define USB_SUBCLASS_DLCM 0x1			/* direct line control model */
#define USB_SUBCLASS_ACM 0x2			/* abstract control model */
#define USB_SUBCLASS_TCM 0x3			/* telephone control model */
#define USB_SUBCLASS_MCCM 0x4			/* multi-channel control model */
#define USB_SUBCLASS_CAPI 0x5			/* CAPI control model */
#define USB_SUBCLASS_ECM 0x6			/* ethernet networking control model */
#define USB_SUBCLASS_ATM 0x7			/* ATM networking control model */
#define USB_SUBCLASS_WIRELESS 0x8		/* wireless handset control model */
#define USB_SUBCLASS_DEV_MGMT 0x9		/* device management */
#define USB_SUBCLASS_MDLM 0xA			/* mobile direct line model */
#define USB_SUBCLASS_OBEX 0xB			/* OBEX */
#define USB_SUBCLASS_EEM 0xC			/* ethernet emulation model */
#define USB_SUBCLASS_NCM 0xD			/* network control model */


/*TODO: communications class protocol codes */
/*TODO: data class protocol codes */


/* descriptors subclassses for communications class (CC)*/
#define USB_DESC_SUBTYPE_CC_HEADER 0x0
#define USB_DESC_SUBTYPE_CC_CALL_MGMT 0x1
#define USB_DESC_SUBTYPE_CC_ACM 0x2

typedef struct _usb_cdc_line_coding {
	unsigned int dwDTERate;
	unsigned char bCharFormat;
	unsigned char bParityType;
	unsigned char bDataBits;
} usb_cdc_line_coding_t;

#endif
