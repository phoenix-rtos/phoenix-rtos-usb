/*
 * Phoenix-RTOS
 *
 * usb/log.h
 *
 * Copyright 2025 Phoenix Systems
 * Author: Adam Greloch
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#ifndef _USB_LOG_H_
#define _USB_LOG_H_

#define USB_LOG_TAG "usb"
#define USB_TRACE   0

/* clang-format off */
#define log_info(fmt, ...) do { fprintf(stdout, USB_LOG_TAG ": " fmt, ##__VA_ARGS__); } while (0)
#define log_msg(fmt, ...) do { fprintf(stderr, USB_LOG_TAG ": " fmt, ##__VA_ARGS__); } while (0)
#define log_trace(fmt, ...) do { if (USB_TRACE != 0) log_msg("%s: " fmt, __func__ __VA_OPT__(, ) ##__VA_ARGS__); } while (0)
#define log_error(fmt, ...) log_msg(fmt, ##__VA_ARGS__)
/* clang-format on */

#endif
