/*
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012 by Dimitri Stolnikov <horiz0n@gmx.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#ifndef _WIN32
#include <unistd.h>
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

//#define USE_SSE

#ifdef USE_SSE
#include <immintrin.h>
#endif

#include <libusb.h>

/*
 * All libusb callback functions should be marked with the LIBUSB_CALL macro
 * to ensure that they are compiled with the same calling convention as libusb.
 *
 * If the macro isn't available in older libusb versions, we simply define it.
 */
#ifndef LIBUSB_CALL
#define LIBUSB_CALL
#endif

#include "mirisdr.h"
#include "mirisdr_reg.h"
#include "tuner_msi001.h"

typedef struct mirisdr_tuner {
	/* tuner interface */
	int (*init)(void *);
	int (*exit)(void *);
	int (*set_freq)(void *, uint32_t freq /* Hz */);
	int (*set_bw)(void *, int bw /* Hz */);
	int (*set_gain)(void *, int gain /* dB */);
	int (*set_gain_mode)(void *, int manual);
} mirisdr_tuner_t;

enum mirisdr_async_status {
	mirisdr_INACTIVE = 0,
	mirisdr_CANCELING,
	mirisdr_RUNNING
};

struct mirisdr_dev {
	libusb_context *ctx;
	struct libusb_device_handle *devh;
	uint32_t xfer_buf_num;
	uint32_t xfer_iso_pack;
	uint32_t xfer_buf_len;
	struct libusb_transfer **xfer;
	unsigned char **xfer_buf;
	mirisdr_read_async_cb_t cb;
	void *cb_ctx;
	enum mirisdr_async_status async_status;
	/* adc context */
	uint32_t rate; /* Hz */
	uint32_t adc_clock; /* Hz */
	/* tuner context */
	mirisdr_tuner_t *tuner;
	uint32_t freq; /* Hz */
	int gain; /* dB */
	/* samples context */
	int headerflag;
	uint32_t addr;
};

typedef struct mirisdr_dongle {
	uint16_t vid;
	uint16_t pid;
	const char *name;
} mirisdr_dongle_t;

static mirisdr_dongle_t known_devices[] = {
	{ 0x1df7, 0x2500, "Mirics MSi2500 default (e.g. VTX3D card)" },
	{ 0x04bb, 0x0537, "IO-DATA GV-TV100 stick" }
};

#define DEFAULT_BUF_NUMBER	32
#define DEFAULT_ISO_PACKETS	8
#define DEFAULT_BUF_LENGTH	(3072 * DEFAULT_ISO_PACKETS)

#define DEF_ADC_FREQ	4000000

#define CTRL_IN		(LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN)
#define CTRL_OUT	(LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT)
#define FUNC(group, function) ((group << 8) | function)

#define CTRL_TIMEOUT	300
#define ISO_TIMEOUT	0

int _msi001_init(void *dev) {
	return 0;
}

int msi001_exit(void *dev) {
	return 0;
}

int msi001_set_freq(void *dev, uint32_t freq) {
	return msi001_init(dev, freq);
}

int msi001_set_bw(void *dev, int bw) {
	return 0;
}

int msi001_set_lna_gain(void *dev, int32_t gain) {
	return 0;
}

int msi001_mixer_gain_set(void *dev, int8_t gain) {
	return 0;
}

int msi001_set_enh_gain(void *dev, int32_t gain) {
	return 0;
}

int msi001_set_gain(void *dev, int gain) {
	return 0;
}

int msi001_set_gain_mode(void *dev, int manual) {
	return 0;
}

static mirisdr_tuner_t tuner = {
	_msi001_init, msi001_exit,
	msi001_set_freq,
	msi001_set_bw, msi001_set_gain, msi001_set_gain_mode
};

int msi2500_write_reg(mirisdr_dev_t *dev, uint8_t reg, uint32_t val)
{
	uint16_t wValue = (val & 0xff) << 8 | reg;
	uint16_t wIndex = (val >> 8) & 0xffff;

	int r = libusb_control_transfer(dev->devh, 0x42, 0x41, wValue, wIndex, NULL, 0, CTRL_TIMEOUT);

//	fprintf(stderr, "%s: reg %02x -> %06x\n", __FUNCTION__, reg, val);
	return r;
}

void mirisdr_init_baseband(mirisdr_dev_t *dev)
{
	/* TODO figure out what that does and why it's needed */
	libusb_control_transfer(dev->devh, 0x42, 0x43, 0x0, 0x0, NULL, 0, CTRL_TIMEOUT);

	/* initialisation */

	msi2500_write_reg(dev, 0x05, 0x00000c);
	msi2500_write_reg(dev, 0x00, 0x000200);
	msi2500_write_reg(dev, 0x02, 0x004801);

	/* IF filter bw + compression scheme? */
	msi2500_write_reg(dev, 0x07, 0x0000a5);

	/* sample rate  = 9.14 MS/s */
	msi2500_write_reg(dev, 0x04, 0x04923d);
	msi2500_write_reg(dev, 0x03, 0x01c907);

	//6M sample rate
//	msi2500_write_reg(dev, 0x04, 0x9220b);
//	msi2500_write_reg(dev, 0x03, 0x14a0b);

	// doesn't work yet
//	fprintf(stderr, "setting fs\n");
//	mirisdr_set_samp_rate(dev, 8000000);

	msi2500_write_reg(dev, 0x13, 0x006b46);
	msi2500_write_reg(dev, 0x14, 0x0000f5);
	msi2500_write_reg(dev, 0x12, 0x802800);

	msi2500_write_reg(dev, 0x29, 0x032201);

	/* enable lock led, deselect eeprom CS */
	msi2500_write_reg(dev, 0x08, 0x006680);

	/* tuner init */
	msi2500_write_reg(dev, 0x09, 0x0094b3);
	msi2500_write_reg(dev, 0x09, 0x00800e);
	msi2500_write_reg(dev, 0x09, 0x200256);

	/* set gains, TODO remove, use tuner driver */
	msi2500_write_reg(dev, 0x09, 0x014281);
}

int mirisdr_deinit_baseband(mirisdr_dev_t *dev)
{
	int r = 0;

	if (!dev)
		return -1;

	if (dev->tuner && dev->tuner->exit) {
//		r = dev->tuner->exit(dev); /* deinitialize tuner */
	}

	/* disable lock led */
	msi2500_write_reg(dev, 0x08, 0x006600);

	return r;
}

int mirisdr_get_usb_strings(mirisdr_dev_t *dev, char *manufact, char *product,
			    char *serial)
{
	struct libusb_device_descriptor dd;
	libusb_device *device = NULL;
	const int buf_max = 256;
	int r = 0;

	if (!dev || !dev->devh)
		return -1;

	device = libusb_get_device(dev->devh);

	r = libusb_get_device_descriptor(device, &dd);
	if (r < 0)
		return -1;

	if (manufact) {
		memset(manufact, 0, buf_max);
		libusb_get_string_descriptor_ascii(dev->devh, dd.iManufacturer,
						   (unsigned char *)manufact,
						   buf_max);
	}

	if (product) {
		memset(product, 0, buf_max);
		libusb_get_string_descriptor_ascii(dev->devh, dd.iProduct,
						   (unsigned char *)product,
						   buf_max);
	}

	if (serial) {
		memset(serial, 0, buf_max);
		libusb_get_string_descriptor_ascii(dev->devh, dd.iSerialNumber,
						   (unsigned char *)serial,
						   buf_max);
	}

	return 0;
}

int mirisdr_set_center_freq(mirisdr_dev_t *dev, uint32_t freq)
{
	int r = -2;

	if (!dev || !dev->tuner)
		return -1;

	if (dev->tuner->set_freq)
		r = dev->tuner->set_freq(dev, freq);

	if (!r)
		dev->freq = freq;
	else
		dev->freq = 0;

	return r;
}

uint32_t mirisdr_get_center_freq(mirisdr_dev_t *dev)
{
	if (!dev || !dev->tuner)
		return 0;

	return dev->freq;
}

int mirisdr_get_tuner_gains(mirisdr_dev_t *dev, int *gains)
{
	const int msi001_gains[] = { -10, 15, 40, 65, 90, 115, 140, 165, 190, 215,
				  240, 290, 340, 420, 430, 450, 470, 490 };
	int len = sizeof(msi001_gains);

	if (!dev)
		return -1;

	if (!gains) { /* no buffer provided, just return the count */
		return len / sizeof(int);
	} else {
		if (len)
			memcpy(gains, msi001_gains, len);

		return len / sizeof(int);
	}
}

int mirisdr_set_tuner_gain(mirisdr_dev_t *dev, int gain)
{
	int r = -2;

	if (!dev || !dev->tuner)
		return -1;

	if (dev->tuner->set_gain)
		r = dev->tuner->set_gain((void *)dev, gain);

	if (!r)
		dev->gain = gain;
	else
		dev->gain = 0;

	return r;
}

int mirisdr_get_tuner_gain(mirisdr_dev_t *dev)
{
	if (!dev || !dev->tuner)
		return 0;

	return dev->gain;
}

int mirisdr_set_tuner_gain_mode(mirisdr_dev_t *dev, int mode)
{
	int r = -2;

	if (!dev || !dev->tuner)
		return -1;

	if (dev->tuner->set_gain_mode)
		r = dev->tuner->set_gain_mode((void *)dev, mode);

	return r;
}

int mirisdr_set_tuner_lna_gain(mirisdr_dev_t *dev, int gain)
{
	return 0;
}

int mirisdr_set_tuner_mixer_gain(mirisdr_dev_t *dev, int gain)
{
	return 0;
}

int mirisdr_set_tuner_mixer_enh(mirisdr_dev_t *dev, int enh)
{
	return 0;
}

int mirisdr_set_tuner_if_gain(mirisdr_dev_t *dev, int stage, int gain)
{
	return 0;
}

int mirisdr_set_sample_rate(mirisdr_dev_t *dev, uint32_t samp_rate)
{
	int n;
	int r = 0;

	if (!dev)
		return -1;

	/* TODO */
//	mirisdr_set_samp_rate(dev, samp_rate);

	if (r >= 0) {
		if (dev->tuner && dev->tuner->set_bw)
			dev->tuner->set_bw(dev, samp_rate);

		dev->rate = samp_rate;
	} else {
		dev->rate = 0;
	}

	return r;
}

uint32_t mirisdr_get_sample_rate(mirisdr_dev_t *dev)
{
	if (!dev)
		return 0;

	return dev->rate;
}

static mirisdr_dongle_t *find_known_device(uint16_t vid, uint16_t pid)
{
	unsigned int i;
	mirisdr_dongle_t *device = NULL;

	for (i = 0; i < sizeof(known_devices)/sizeof(mirisdr_dongle_t); i++ ) {
		if (known_devices[i].vid == vid && known_devices[i].pid == pid) {
			device = &known_devices[i];
			break;
		}
	}

	return device;
}

uint32_t mirisdr_get_device_count(void)
{
	int i;
	libusb_context *ctx;
	libusb_device **list;
	struct libusb_device_descriptor dd;
	uint32_t device_count = 0;
	ssize_t cnt;

	libusb_init(&ctx);

	cnt = libusb_get_device_list(ctx, &list);

	for (i = 0; i < cnt; i++) {
		libusb_get_device_descriptor(list[i], &dd);

		if (find_known_device(dd.idVendor, dd.idProduct))
			device_count++;
	}

	libusb_free_device_list(list, 1);

	libusb_exit(ctx);

	return device_count;
}

const char *mirisdr_get_device_name(uint32_t index)
{
	int i;
	libusb_context *ctx;
	libusb_device **list;
	struct libusb_device_descriptor dd;
	mirisdr_dongle_t *device = NULL;
	uint32_t device_count = 0;
	ssize_t cnt;

	libusb_init(&ctx);

	cnt = libusb_get_device_list(ctx, &list);

	for (i = 0; i < cnt; i++) {
		libusb_get_device_descriptor(list[i], &dd);

		device = find_known_device(dd.idVendor, dd.idProduct);

		if (device) {
			device_count++;

			if (index == device_count - 1)
				break;

			device = NULL;
		}
	}

	libusb_free_device_list(list, 1);

	libusb_exit(ctx);

	if (device)
		return device->name;
	else
		return "";
}

int mirisdr_get_device_usb_strings(uint32_t index, char *manufact,
				   char *product, char *serial)
{
	int r = -2;
	int i;
	libusb_context *ctx;
	libusb_device **list;
	struct libusb_device_descriptor dd;
	mirisdr_dev_t devt;
	uint32_t device_count = 0;
	ssize_t cnt;

	libusb_init(&ctx);

	cnt = libusb_get_device_list(ctx, &list);

	for (i = 0; i < cnt; i++) {
		libusb_get_device_descriptor(list[i], &dd);

		if (find_known_device(dd.idVendor, dd.idProduct))
			device_count++;

		if (index == device_count - 1) {
			r = libusb_open(list[i], &devt.devh);
			if (!r) {
				r = mirisdr_get_usb_strings(&devt,
							    manufact,
							    product,
							    serial);
				libusb_close(devt.devh);
			}
			break;
		}
	}

	libusb_free_device_list(list, 1);

	libusb_exit(ctx);

	return r;
}

int mirisdr_open(mirisdr_dev_t **out_dev, uint32_t index)
{
	int r;
	int i;
	libusb_device **list;
	mirisdr_dev_t *dev = NULL;
	libusb_device *device = NULL;
	uint32_t device_count = 0;
	struct libusb_device_descriptor dd;
	ssize_t cnt;

	dev = malloc(sizeof(mirisdr_dev_t));
	if (NULL == dev)
		return -ENOMEM;

	memset(dev, 0, sizeof(mirisdr_dev_t));

	libusb_init(&dev->ctx);

	cnt = libusb_get_device_list(dev->ctx, &list);

	for (i = 0; i < cnt; i++) {
		device = list[i];

		libusb_get_device_descriptor(list[i], &dd);

		if (find_known_device(dd.idVendor, dd.idProduct))
			device_count++;

		if (index == device_count - 1)
			break;

		device = NULL;
	}

	if (!device) {
		r = -1;
		goto err;
	}

	r = libusb_open(device, &dev->devh);
	if (r < 0) {
		libusb_free_device_list(list, 1);
		fprintf(stderr, "usb_open error %d\n", r);
		goto err;
	}

	libusb_free_device_list(list, 1);

	r = libusb_claim_interface(dev->devh, 0);
	if (r < 0) {
		fprintf(stderr, "usb_claim_interface error %d\n", r);
		goto err;
	}

	dev->adc_clock = DEF_ADC_FREQ;

	mirisdr_init_baseband(dev);

	dev->tuner = &tuner; /* so far we have only one tuner */

	if (dev->tuner->init) {
		r = dev->tuner->init(dev);
	}

	r = libusb_set_interface_alt_setting(dev->devh, 0, 1);

	*out_dev = dev;

	return 0;
err:
	if (dev) {
		if (dev->ctx)
			libusb_exit(dev->ctx);

		free(dev);
	}

	return r;
}

int mirisdr_close(mirisdr_dev_t *dev)
{
	if (!dev)
		return -1;

	mirisdr_deinit_baseband(dev);

	libusb_release_interface(dev->devh, 0);
	libusb_close(dev->devh);

	libusb_exit(dev->ctx);

	free(dev);

	return 0;
}

int mirisdr_reset_buffer(mirisdr_dev_t *dev)
{
	if (!dev)
		return -1;

	/* TODO: implement */

	return 0;
}

int mirisdr_read_sync(mirisdr_dev_t *dev, void *buf, int len, int *n_read)
{
	if (!dev)
		return -1;

//	return libusb_bulk_transfer(dev->devh, 0x86, buf, len, n_read, BULK_TIMEOUT);
	return -1;
}

void hexdump(uint8_t *inbuf, int cnt)
{
	int i;
	for (i = 0; i < cnt; i++) {
		printf("%02x ", inbuf[i]);
	}
	printf("\n");
}

int mirisdr_convert_samples(mirisdr_dev_t *dev, unsigned char* inbuf, int16_t *outsamples, int length)
{
	int i, j, k, l, block;
	uint32_t offs;
	uint8_t *ip;
	uint32_t address;
	int op = 0;

	ip = inbuf;

	block = length / 1024;
	while (block--) {
		/* parse header */
#if 1
		address = ip[1] + (ip[2] << 8) + (ip[3] << 16);
		if (address != dev->addr)
			fprintf(stderr, "Lost samples!\n");

		dev->addr = address + (ip[0] >> 7) + 1;

		if (((ip[5] & 0x40) && dev->headerflag)) {
			hexdump(ip, 16);
			dev->headerflag = 0;
		} else if ((!(ip[5] & 0x40) && !dev->headerflag)) {
			hexdump(ip, 16);
			dev->headerflag = 1;
		}
#endif

		/* skip header */
		ip += 16;

		/* 16-sample-ips per block */
		k = 6;
		while (k--) {
			uint32_t flag;
			for (j = 0; j < 16; j++) {
				for (i = 0; i < 10; i += 5) {
					outsamples[op++] = (ip[i+0] << 6) | ((ip[i+1] & 0x03) << 14);
					outsamples[op++] = ((ip[i+1] & 0xfc) << 4) | ((ip[i+2] & 0x0f) << 12);
					outsamples[op++] = ((ip[i+2] & 0xf0) << 2) | ((ip[i+3] & 0x3f) << 10);
					outsamples[op++] = (ip[i+3] & 0xc0) | (ip[i+4] << 8);
				}
				/* 10 bytes per 8 samples */
				ip += 10;
			}

			flag = *(uint32_t*)ip;
			flag = 0;
			for (j = 0; j < 16; j++) {
#ifdef USE_SSE
				/* Hoernchen's SSE accelerated version */
				__m128i* addr = (__m128i*)&(outsamples[op-128+j*8+0]);
				__m128i v;
				switch(flag & 0x3) {
				case 0:
					v = _mm_loadu_si128(addr);
					v = _mm_srai_epi16(v, 2);
					_mm_storeu_si128(addr, v);
					break;
				case 1:
					v = _mm_loadu_si128(addr);
					v = _mm_srai_epi16(v, 1);
					_mm_storeu_si128(addr, v);
					break;
				case 2:
				case 3:
					break;
				}

#else
				switch (flag & 0x03) {
				case 0:
					outsamples[op-128+j*8+0] >>= 2;
					outsamples[op-128+j*8+1] >>= 2;
					outsamples[op-128+j*8+2] >>= 2;
					outsamples[op-128+j*8+3] >>= 2;
					outsamples[op-128+j*8+4] >>= 2;
					outsamples[op-128+j*8+5] >>= 2;
					outsamples[op-128+j*8+6] >>= 2;
					outsamples[op-128+j*8+7] >>= 2;
					break;
				case 1:
					outsamples[op-128+j*8+0] >>= 1;
					outsamples[op-128+j*8+1] >>= 1;
					outsamples[op-128+j*8+2] >>= 1;
					outsamples[op-128+j*8+3] >>= 1;
					outsamples[op-128+j*8+4] >>= 1;
					outsamples[op-128+j*8+5] >>= 1;
					outsamples[op-128+j*8+6] >>= 1;
					outsamples[op-128+j*8+7] >>= 1;
					break;
				case 2:
				case 3:
					break;

				}
#endif
				flag >>= 2;
			}
			/* flagbytes */
			ip += 4;
		}
		ip += 24;
	}

	return op;
//	fwrite(outsamples, op * sizeof(uint16_t) , 1, file);
}

static void LIBUSB_CALL _libusb_callback(struct libusb_transfer *xfer)
{
	int i, len, total_len = 0;
	static unsigned char* iso_packet_buf;
	mirisdr_dev_t *dev = (mirisdr_dev_t *)xfer->user_data;
	int16_t outsamples[768 * 3 * 8];

//	if (xfer->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
	for (i = 0; i < xfer->num_iso_packets; i++) {
		struct libusb_iso_packet_descriptor *pack = &xfer->iso_packet_desc[i];

		if (pack->status != LIBUSB_TRANSFER_COMPLETED) {
//			fprintf(stderr, "transfer status: %d\n", xfer->status);
//			mirisdr_cancel_async(dev); /* abort async loop */
		}

		if (pack->actual_length > 0) {
			iso_packet_buf =  libusb_get_iso_packet_buffer_simple(xfer, i);
			if (iso_packet_buf) {
				len = mirisdr_convert_samples(dev, iso_packet_buf, outsamples + total_len, pack->actual_length);
				total_len += len;
			}
//			if (pack->actual_length != 3072)
//				fprintf(stderr, "pack%u length:%u, actual_length:%u\n", i, pack->length, pack->actual_length);
		}
	}

	if (dev->cb && total_len > 0)
		dev->cb((uint8_t*)outsamples, total_len * sizeof(int16_t), dev->cb_ctx);

	/* resubmit transfer */
	if (libusb_submit_transfer(xfer) < 0) {
		fprintf(stderr, "error re-submitting URB\n");
		exit(1);
	}
}

static int _mirisdr_alloc_async_buffers(mirisdr_dev_t *dev)
{
	unsigned int i;

	if (!dev)
		return -1;

	if (!dev->xfer) {
		dev->xfer = malloc(dev->xfer_buf_num *
				   sizeof(struct libusb_transfer *));

		for(i = 0; i < dev->xfer_buf_num; ++i)
			dev->xfer[i] = libusb_alloc_transfer(dev->xfer_iso_pack);
	}

	if (!dev->xfer_buf) {
		dev->xfer_buf = malloc(dev->xfer_buf_num *
					   sizeof(unsigned char *));

		for(i = 0; i < dev->xfer_buf_num; ++i)
			dev->xfer_buf[i] = malloc(dev->xfer_buf_len);
	}

	printf("%s\n", __FUNCTION__);
	return 0;
}

static int _mirisdr_free_async_buffers(mirisdr_dev_t *dev)
{
	unsigned int i;

	if (!dev)
		return -1;

	if (dev->xfer) {
		for(i = 0; i < dev->xfer_buf_num; ++i) {
			if (dev->xfer[i]) {
				libusb_free_transfer(dev->xfer[i]);
			}
		}

		free(dev->xfer);
		dev->xfer = NULL;
	}

	if (dev->xfer_buf) {
		for(i = 0; i < dev->xfer_buf_num; ++i) {
			if (dev->xfer_buf[i])
				free(dev->xfer_buf[i]);
		}

		free(dev->xfer_buf);
		dev->xfer_buf = NULL;
	}

	return 0;
}

int mirisdr_read_async(mirisdr_dev_t *dev, mirisdr_read_async_cb_t cb, void *ctx,
		       uint32_t buf_num, uint32_t buf_len)
{
	unsigned int i;
	int r, num_iso_pack = 8;
	struct timeval tv = { 1, 0 };

	if (!dev)
		return -1;

	dev->cb = cb;
	dev->cb_ctx = ctx;

//	if (buf_num > 0)
//		dev->xfer_buf_num = buf_num;

//	else
		dev->xfer_buf_num = DEFAULT_BUF_NUMBER;
		dev->xfer_iso_pack = DEFAULT_ISO_PACKETS;

//	if (buf_len > 0 && buf_len % 512 == 0) /* len must be multiple of 512 */
//		dev->xfer_buf_len = buf_len;
//	else
		dev->xfer_buf_len = DEFAULT_BUF_LENGTH;

	_mirisdr_alloc_async_buffers(dev);

	for(i = 0; i < dev->xfer_buf_num; ++i) {
		libusb_fill_iso_transfer(dev->xfer[i],
					  dev->devh,
					  0x81,
					  dev->xfer_buf[i],
					  dev->xfer_buf_len,
					  num_iso_pack,
					  _libusb_callback,
					  (void *)dev,
					  ISO_TIMEOUT);

		libusb_set_iso_packet_lengths(dev->xfer[i],
					      dev->xfer_buf_len/dev->xfer_iso_pack);

		libusb_submit_transfer(dev->xfer[i]);
	}

	dev->async_status = mirisdr_RUNNING;

	while (mirisdr_INACTIVE != dev->async_status) {
		r = libusb_handle_events_timeout(dev->ctx, &tv);
		if (r < 0) {
			fprintf(stderr, "handle_events returned: %d\n", r);
			if (r == LIBUSB_ERROR_INTERRUPTED) /* stray signal */
				continue;
			break;
		}

		if (mirisdr_CANCELING == dev->async_status) {
			dev->async_status = mirisdr_INACTIVE;

			if (!dev->xfer)
				break;

			for(i = 0; i < dev->xfer_buf_num; ++i) {
				if (!dev->xfer[i])
					continue;

				if (dev->xfer[i]->status == LIBUSB_TRANSFER_COMPLETED) {
					libusb_cancel_transfer(dev->xfer[i]);
					dev->async_status = mirisdr_CANCELING;
				}
			}

			if (mirisdr_INACTIVE == dev->async_status)
				break;
		}
	}

	_mirisdr_free_async_buffers(dev);

	return r;
}

int mirisdr_cancel_async(mirisdr_dev_t *dev)
{
	if (!dev)
		return -1;

	if (mirisdr_RUNNING == dev->async_status) {
		dev->async_status = mirisdr_CANCELING;
		return 0;
	}

	return -2;
}

int mirisdr_reg_write_fn(void *dev, uint8_t reg, uint32_t val)
{
	if (dev)
		return msi2500_write_reg(((mirisdr_dev_t *)dev), reg, val);

	return -1;
}
