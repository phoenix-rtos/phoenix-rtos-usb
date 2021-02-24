/*
 * Phoenix-RTOS
 *
 * cdc demo - Example of using: USB Communication Device Class
 *
 * Copyright 2019 Phoenix Systems
 * Author: Hubert Buczynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/platform.h>
#include <phoenix/arch/imxrt.h>

#include <cdc_client.h>

#define RCV_MODE 2
#define SENDING_MODE 1

#define CDC_MODE RCV_MODE

#define BUFF_SIZE 0x1000

#define LOG(str, ...) do { if (1) fprintf(stderr, "cdc-client: " str "\n", ##__VA_ARGS__); } while (0)
#define LOG_ERROR(str, ...) do { fprintf(stderr, __FILE__  ":%d error: " str "\n", __LINE__, ##__VA_ARGS__); } while (0)


struct {
	char buff[BUFF_SIZE];
} client_common;


int main(int argc, char **argv)
{
	sleep(1);

	LOG("started.");

	if (cdc_init(NULL, NULL)) {
		LOG_ERROR("couldn't initialize CDC transport.");
		return -1;
	}

	LOG("initialized.");

#if CDC_MODE == SENDING_MODE
	int i;
	for (i = 0; i < BUFF_SIZE; i++)
		client_common.buff[i] = 'A' + i % ('Z' - 'A');

	LOG("SENDING MODE initialized.");
	while (1)
		cdc_send(CDC_ENDPT_BULK, (char *)client_common.buff, BUFF_SIZE);
#elif CDC_MODE == RCV_MODE
	LOG("RCV MODE initialized.");

	while (1)
		printf("Size: %d\n", cdc_recv(ENDPOINT_BULK, (char *)client_common.buff, BUFF_SIZE));
#else
	while (1)
	{}
#endif

	return EOK;
}
