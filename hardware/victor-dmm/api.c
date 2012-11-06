/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2012 Bert Vermeulen <bert@biot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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

#include <glib.h>
#include "libsigrok.h"
#include "libsigrok-internal.h"
#include "protocol.h"
#include <libusb.h>
#include <stdlib.h>
#include <string.h>

#define VICTOR_VID 0x1244
#define VICTOR_PID 0xd237
#define VICTOR_VENDOR "Victor"
#define VICTOR_INTERFACE 0
#define VICTOR_ENDPOINT LIBUSB_ENDPOINT_IN | 1

SR_PRIV struct sr_dev_driver victor_dmm_driver_info;
static struct sr_dev_driver *di = &victor_dmm_driver_info;
static int hw_dev_close(struct sr_dev_inst *sdi);
static int hw_dev_acquisition_stop(struct sr_dev_inst *sdi, void *cb_data);

static const int hwcaps[] = {
	SR_HWCAP_MULTIMETER,
	SR_HWCAP_LIMIT_MSEC,
	SR_HWCAP_LIMIT_SAMPLES,
	SR_HWCAP_CONTINUOUS,
	0
};

static const char *probe_names[] = {
	"P1",
};


/* Properly close and free all devices. */
static int clear_instances(void)
{
	struct sr_dev_inst *sdi;
	struct drv_context *drvc;
	struct dev_context *devc;
	GSList *l;

	if (!(drvc = di->priv))
		/* Can get called on an unused driver, doesn't matter. */
		return SR_OK;

	for (l = drvc->instances; l; l = l->next) {
		if (!(sdi = l->data))
			continue;
		if (!(devc = sdi->priv))
			continue;
		hw_dev_close(sdi);
		sr_usb_dev_inst_free(devc->usb);
		sr_dev_inst_free(sdi);
	}

	g_slist_free(drvc->instances);
	drvc->instances = NULL;

	return SR_OK;
}

static int hw_init(void)
{
	struct drv_context *drvc;

	if (!(drvc = g_try_malloc0(sizeof(struct drv_context)))) {
		sr_err("Driver context malloc failed.");
		return SR_ERR_MALLOC;
	}
	di->priv = drvc;

	return SR_OK;
}

static GSList *hw_scan(GSList *options)
{
	struct drv_context *drvc;
	struct dev_context *devc;
	struct sr_dev_inst *sdi;
	struct sr_probe *probe;
	struct libusb_device_descriptor des;
	libusb_device **devlist;
	GSList *devices;
	int ret, devcnt, i;

	(void)options;

	if (!(drvc = di->priv)) {
		sr_err("Driver was not initialized.");
		return NULL;
	}

	/* USB scan is always authoritative. */
	clear_instances();

	devices = NULL;
	libusb_get_device_list(NULL, &devlist);
	for (i = 0; devlist[i]; i++) {
		if ((ret = libusb_get_device_descriptor(devlist[i], &des)) != 0) {
			sr_warn("Failed to get device descriptor: %s",
					libusb_error_name(ret));
			continue;
		}

		if (des.idVendor != VICTOR_VID || des.idProduct != VICTOR_PID)
			continue;

		devcnt = g_slist_length(drvc->instances);
		if (!(sdi = sr_dev_inst_new(devcnt, SR_ST_INACTIVE, VICTOR_VENDOR,
				NULL, NULL)))
			return NULL;
		sdi->driver = di;

		if (!(devc = g_try_malloc0(sizeof(struct dev_context))))
			return NULL;
		sdi->priv = devc;

		if (!(probe = sr_probe_new(0, SR_PROBE_ANALOG, TRUE, "P1")))
			return NULL;
		sdi->probes = g_slist_append(NULL, probe);

		if (!(devc->usb = sr_usb_dev_inst_new(libusb_get_bus_number(devlist[i]),
				libusb_get_device_address(devlist[i]), NULL)))
			return NULL;

		drvc->instances = g_slist_append(drvc->instances, sdi);
		devices = g_slist_append(devices, sdi);
	}
	libusb_free_device_list(devlist, 1);

	return devices;
}

static GSList *hw_dev_list(void)
{
	struct drv_context *drvc;

	if (!(drvc = di->priv)) {
		sr_err("Driver was not initialized.");
		return NULL;
	}

	return drvc->instances;
}

static int hw_dev_open(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	libusb_device **devlist;
	int ret, i;

	if (!di->priv) {
		sr_err("Driver was not initialized.");
		return SR_ERR;
	}

	devc = sdi->priv;
	libusb_get_device_list(NULL, &devlist);
	for (i = 0; devlist[i]; i++) {
		if (libusb_get_bus_number(devlist[i]) != devc->usb->bus
				|| libusb_get_device_address(devlist[i]) != devc->usb->address)
			continue;
		if ((ret = libusb_open(devlist[i], &devc->usb->devhdl))) {
			sr_err("Failed to open device: %s", libusb_error_name(ret));
			return SR_ERR;
		}
		break;
	}
	libusb_free_device_list(devlist, 1);
	if (!devlist[i]) {
		sr_err("Device not found.");
		return SR_ERR;
	}

	/* The device reports as HID class, so the kernel would have
	 * claimed it. */
	if (libusb_kernel_driver_active(devc->usb->devhdl, 0) == 1) {
		if (libusb_detach_kernel_driver(devc->usb->devhdl, 0) < 0) {
			sr_err("Failed to detach kernel driver.");
			return SR_ERR;
		}
	}

	if ((ret = libusb_claim_interface(devc->usb->devhdl,
			VICTOR_INTERFACE))) {
		sr_err("Failed to claim interface: %s", libusb_error_name(ret));
		return SR_ERR;
	}
	sdi->status = SR_ST_ACTIVE;

	return SR_OK;
}

static int hw_dev_close(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;

	if (!di->priv) {
		sr_err("Driver was not initialized.");
		return SR_ERR;
	}

	devc = sdi->priv;
	if (!devc->usb->devhdl)
		/*  Nothing to do. */
		return SR_OK;

	libusb_release_interface(devc->usb->devhdl, VICTOR_INTERFACE);
	libusb_close(devc->usb->devhdl);
	devc->usb->devhdl = NULL;
	sdi->status = SR_ST_INACTIVE;

	return SR_OK;
}

static int hw_cleanup(void)
{
	struct drv_context *drvc;

	if (!(drvc = di->priv))
		/* Can get called on an unused driver, doesn't matter. */
		return SR_OK;

	clear_instances();
	g_free(drvc);
	di->priv = NULL;

	return SR_OK;
}

static int hw_info_get(int info_id, const void **data,
		       const struct sr_dev_inst *sdi)
{

	(void)sdi;

	switch (info_id) {
		case SR_DI_HWCAPS:
			*data = hwcaps;
			break;
		case SR_DI_NUM_PROBES:
			*data = GINT_TO_POINTER(1);
			break;
		case SR_DI_PROBE_NAMES:
			*data = probe_names;
			break;
		default:
			return SR_ERR_ARG;
	}

	return SR_OK;
}

static int hw_dev_config_set(const struct sr_dev_inst *sdi, int hwcap,
			     const void *value)
{
	struct dev_context *devc;
	gint64 now;
	int ret;

	if (!di->priv) {
		sr_err("Driver was not initialized.");
		return SR_ERR;
	}

	if (sdi->status != SR_ST_ACTIVE) {
		sr_err("Device inactive, can't set config options.");
		return SR_ERR;
	}

	devc = sdi->priv;
	ret = SR_OK;
	switch (hwcap) {
		case SR_HWCAP_LIMIT_MSEC:
			devc->limit_msec = *(const int64_t *)value;
			now = g_get_monotonic_time() / 1000;
			devc->end_time = now + devc->limit_msec;
			sr_dbg("setting time limit to %" PRIu64 "ms.",
					devc->limit_msec);
			break;
		case SR_HWCAP_LIMIT_SAMPLES:
			devc->limit_samples = *(const uint64_t *)value;
			sr_dbg("setting sample limit to %" PRIu64 ".",
					devc->limit_samples);
			break;
	default:
		sr_err("Unknown hardware capability: %d.", hwcap);
		ret = SR_ERR_ARG;
	}

	return ret;
}

static void receive_transfer(struct libusb_transfer *transfer)
{
	struct dev_context *devc;
	struct sr_dev_inst *sdi;
	int ret;

	sdi = transfer->user_data;
	devc = sdi->priv;
	if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
		/* USB device was unplugged. */
		hw_dev_acquisition_stop(sdi, sdi);
	} else if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		sr_dbg("got %d-byte packet", transfer->actual_length);
		if (transfer->actual_length == 14) {
			devc->num_samples++;
			/* TODO */

			if (devc->limit_samples) {
				if (devc->num_samples >= devc->limit_samples)
					hw_dev_acquisition_stop(sdi, sdi);
			}
		}
	}
	/* Anything else is either an error or a timeout, which is fine:
	 * we were just going to send another transfer request anyway. */

	if (sdi->status == SR_ST_ACTIVE) {
		/* Send the same request again. */
		if ((ret = libusb_submit_transfer(transfer) != 0)) {
			sr_err("unable to resubmit transfer: %s", libusb_error_name(ret));
			libusb_free_transfer(transfer);
			g_free(transfer->buffer);
			hw_dev_acquisition_stop(sdi, sdi);
		}
	} else {
		/* This was the last transfer we're going to receive, so
		 * clean up now. */
		libusb_free_transfer(transfer);
		g_free(transfer->buffer);
	}

}

static int handle_events(int fd, int revents, void *cb_data)
{
	struct dev_context *devc;
	struct sr_datafeed_packet packet;
	struct sr_dev_inst *sdi;
	struct timeval tv;
	gint64 now;
	int i;

	(void)fd;
	(void)revents;

	sdi = cb_data;
	devc = sdi->priv;

	if (devc->limit_msec) {
		now = g_get_monotonic_time() / 1000;
		if (now > devc->end_time)
			hw_dev_acquisition_stop(sdi, sdi);
	}

	if (sdi->status == SR_ST_STOPPING) {
		for (i = 0; devc->usbfd[i] != -1; i++)
			sr_source_remove(devc->usbfd[i]);

		hw_dev_close(sdi);

		packet.type = SR_DF_END;
		sr_session_send(cb_data, &packet);
	}

	memset(&tv, 0, sizeof(struct timeval));
	libusb_handle_events_timeout_completed(NULL, &tv, NULL);

	return TRUE;
}

static int hw_dev_acquisition_start(const struct sr_dev_inst *sdi,
				    void *cb_data)
{
	struct sr_datafeed_packet packet;
	struct sr_datafeed_header header;
	struct sr_datafeed_meta_analog meta;
	struct dev_context *devc;
	const struct libusb_pollfd **pfd;
	struct libusb_transfer *transfer;
	int ret, i;
	unsigned char *buf;

	if (!di->priv) {
		sr_err("Driver was not initialized.");
		return SR_ERR;
	}

	sr_dbg("Starting acquisition.");

	devc = sdi->priv;
	devc->cb_data = cb_data;

	/* Send header packet to the session bus. */
	sr_dbg("Sending SR_DF_HEADER.");
	packet.type = SR_DF_HEADER;
	packet.payload = (uint8_t *)&header;
	header.feed_version = 1;
	sr_session_send(devc->cb_data, &packet);

	/* Send metadata about the SR_DF_ANALOG packets to come. */
	packet.type = SR_DF_META_ANALOG;
	packet.payload = &meta;
	meta.num_probes = 1;
	sr_session_send(devc->cb_data, &packet);

	pfd = libusb_get_pollfds(NULL);
	for (i = 0; pfd[i]; i++) {
		/* Handle USB events every 100ms, for decent latency. */
		sr_source_add(pfd[i]->fd, pfd[i]->events, 100,
				handle_events, (void *)sdi);
		/* We'll need to remove this fd later. */
		devc->usbfd[i] = pfd[i]->fd;
	}
	devc->usbfd[i] = -1;

	buf = g_try_malloc(14);
	transfer = libusb_alloc_transfer(0);
	/* Each transfer request gets 100ms to arrive before it's restarted.
	 * The device only sends 1 transfer/second no matter how many
	 * times you ask, but we want to keep step with the USB events
	 * handling above. */
	libusb_fill_interrupt_transfer(transfer, devc->usb->devhdl,
			VICTOR_ENDPOINT, buf, 14, receive_transfer, cb_data, 100);
	if ((ret = libusb_submit_transfer(transfer) != 0)) {
		sr_err("unable to submit transfer: %s", libusb_error_name(ret));
		libusb_free_transfer(transfer);
		g_free(buf);
		return SR_ERR;
	}

	return SR_OK;
}

static int hw_dev_acquisition_stop(struct sr_dev_inst *sdi, void *cb_data)
{

	(void)cb_data;

	if (!di->priv) {
		sr_err("Driver was not initialized.");
		return SR_ERR;
	}

	if (sdi->status != SR_ST_ACTIVE) {
		sr_err("Device not active, can't stop acquisition.");
		return SR_ERR;
	}

	sdi->status = SR_ST_STOPPING;

	return SR_OK;
}

SR_PRIV struct sr_dev_driver victor_dmm_driver_info = {
	.name = "victor-dmm",
	.longname = "Victor DMMs",
	.api_version = 1,
	.init = hw_init,
	.cleanup = hw_cleanup,
	.scan = hw_scan,
	.dev_list = hw_dev_list,
	.dev_clear = clear_instances,
	.dev_open = hw_dev_open,
	.dev_close = hw_dev_close,
	.info_get = hw_info_get,
	.dev_config_set = hw_dev_config_set,
	.dev_acquisition_start = hw_dev_acquisition_start,
	.dev_acquisition_stop = hw_dev_acquisition_stop,
	.priv = NULL,
};
