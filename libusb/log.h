
/*
 * Phoenix-RTOS
 *
 * Libusb driver interface
 *
 * libusb/log.h
 *
 * Copyright 2025 Phoenix Systems
 * Author: Adam Greloch
 *
 * %LICENSE%
 */


#ifndef _LIBUSB_LOG_H_
#define _LIBUSB_LOG_H_


#define LIBUSB_LOG_TAG "libusb"
#define LIBUSB_TRACE   0

/* clang-format off */
#define log_msg(fmt, ...) do { fprintf(stderr, LIBUSB_LOG_TAG ": " fmt, ##__VA_ARGS__); } while (0)
#define log_trace(fmt, ...) do { if (LIBUSB_TRACE != 0) log_msg("%s: " fmt, __func__ __VA_OPT__(, ) ##__VA_ARGS__); } while (0)
#define log_error(fmt, ...) log_msg(fmt, ##__VA_ARGS__)
/* clang-format on */


#endif
