/*
 * Phoenix-RTOS
 *
 * USB host memory pool management
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <sys/mman.h>
#include <sys/threads.h>

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define USB_CHUNK_SIZE 32
#define USB_BUF_SIZE   4096


struct usb_chunk_hdr {
	struct usb_chunk_hdr *next;
	size_t size;
};


typedef struct usb_buf {
	union {
		struct {
			struct usb_buf *next;
			unsigned freesz;
			struct usb_chunk_hdr *head;
		};
		char padding[USB_CHUNK_SIZE];
	};

	char start[];
} usb_buf_t;


static struct {
	usb_buf_t *buffer;
	handle_t lock;
} usb_mem_common;


static void *usb_allocUncached(size_t size)
{
	void *res;

	if ((res = mmap(NULL, size, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_UNCACHED, -1, 0)) == MAP_FAILED)
		return NULL;

	return res;
}


void *usb_allocAligned(size_t size, size_t alignment)
{
	char *addr, *res;

	size = (size + (_PAGE_SIZE - 1)) & ~(_PAGE_SIZE - 1);
	alignment = (alignment + (_PAGE_SIZE - 1)) & ~(_PAGE_SIZE - 1);

	if ((addr = usb_allocUncached(size)) == NULL)
		return NULL;

	if (va2pa(addr) % alignment == 0)
		return addr;

	munmap(addr, size);

	if ((addr = usb_allocUncached(alignment * 2)) == NULL)
		return NULL;

	res = (char *)(((uintptr_t)addr + (alignment - 1)) & ~(alignment - 1));

	/* unmap memory prefix */
	if (res != addr)
		munmap(addr, res - addr);

	/* unmap memory sufix */
	if (res + size != addr + alignment * 2)
		munmap(res + size, (addr + alignment * 2) - (res + size));

	return res;
}


void usb_freeAligned(void *ptr, size_t size)
{
	munmap(ptr, size);
}


static usb_buf_t *usb_allocBuffer(void)
{
	struct usb_chunk_hdr *hdr;
	usb_buf_t *buf;

	buf = usb_allocAligned(USB_BUF_SIZE, USB_BUF_SIZE);
	if (buf == NULL)
		return NULL;

	buf->next = NULL;
	buf->freesz = USB_BUF_SIZE - USB_CHUNK_SIZE;

	hdr = (struct usb_chunk_hdr *)buf->start;
	hdr->next = NULL;
	hdr->size = buf->freesz;
	buf->head = hdr;

	return buf;
}


static void *usb_allocFrom(usb_buf_t *buf, size_t size)
{
	struct usb_chunk_hdr *prev, *next, *hdr = NULL;

	/* Search for a chunk in this buffer */
	if (buf->freesz >= size) {
		prev = buf->head;
		hdr = buf->head;
		if (hdr->size < size) {
			hdr = hdr->next;
			while (hdr != NULL && hdr->size < size) {
				prev = hdr;
				hdr = hdr->next;
			}
		}
	}

	/* No big enough chunks in this buffer */
	if (hdr == NULL) {
		/* Alloc next buffer */
		if (buf->next == NULL) {
			if ((buf->next = usb_allocBuffer()) == NULL)
				return NULL;
		}

		return usb_allocFrom(buf->next, size);
	}

	if (hdr->size > size) {
		/* Shrink this chunk */
		next = (struct usb_chunk_hdr *)((char *)hdr + size);
		next->next = hdr->next;
		next->size = hdr->size - size;
	}
	else {
		next = hdr->next;
	}

	if (hdr == buf->head)
		buf->head = next;
	else
		prev->next = next;

	buf->freesz -= size;
	memset(hdr, 0, size);

	return (void *)hdr;
}


void usb_free(void *ptr, size_t size)
{
	usb_buf_t *buf;
	struct usb_chunk_hdr *next, *prev, *hdr;

	if (ptr == NULL || size == 0)
		return;

	size = (size + (USB_CHUNK_SIZE - 1)) & ~(USB_CHUNK_SIZE - 1);
	if (size > USB_BUF_SIZE - USB_CHUNK_SIZE) {
		munmap(ptr, size);
		return;
	}

	buf = (usb_buf_t *)((uintptr_t)ptr & ~(USB_BUF_SIZE - 1));
	hdr = (struct usb_chunk_hdr *)ptr;
	mutexLock(usb_mem_common.lock);

	hdr->size = size;

	if (buf->head == NULL) {
		/* Buffer full */
		buf->head = (struct usb_chunk_hdr *)ptr;
		hdr->next = NULL;
	}
	else {
		prev = buf->head;
		/* Find, where to put the freed chunk */
		while (prev->next < hdr && prev->next != NULL)
			prev = prev->next;

		next = prev->next;
		if ((struct usb_chunk_hdr *)((uintptr_t)prev + prev->size) == ptr) {
			/* Left merge */
			prev->size += size;
			hdr = prev;
		}
		else if (prev > hdr) {
			buf->head = hdr;
			next = prev;
		}
		else {
			prev->next = hdr;
		}

		if ((struct usb_chunk_hdr *)((char *)hdr + hdr->size) == next) {
			/* Right merge */
			hdr->size += next->size;
			hdr->next = next->next;
		}
		else {
			hdr->next = next;
		}
	}

	buf->freesz += size;
	mutexUnlock(usb_mem_common.lock);
}


void *usb_alloc(size_t size)
{
	void *ret;

	if (size == 0)
		return NULL;

	size = (size + (USB_CHUNK_SIZE - 1)) & ~(USB_CHUNK_SIZE - 1);
	/* Don't manage larger buffers */
	if (size > USB_BUF_SIZE - USB_CHUNK_SIZE)
		return usb_allocAligned(size, USB_BUF_SIZE);

	mutexLock(usb_mem_common.lock);
	ret = usb_allocFrom(usb_mem_common.buffer, size);
	mutexUnlock(usb_mem_common.lock);

	return ret;
}


int usb_memInit(void)
{
	if ((usb_mem_common.buffer = usb_allocBuffer()) == NULL)
		return -1;

	return mutexCreate(&usb_mem_common.lock);
}
