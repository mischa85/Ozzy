// SPDX-License-Identifier: MIT
/*
 * Ozzy USB Audio Driver - Core Module
 *
 * Generic USB probe/disconnect, device registration, and module entry
 * point. Device-specific behavior is delegated through ozzy_device_ops.
 *
 * Based on the work done in TerraTec DMX 6Fire USB.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <sound/initval.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/gfp.h>
#include <sound/core.h>
#include <linux/usb.h>

#include "ozzy.h"
#include "ozzy_log.h"
#include "ozzy_pcm.h"
#include "ozzy_midi.h"
#include "devices/ploytec.h"
#include "../common/ozzy_version.h"

MODULE_AUTHOR("Marcel Bierling <marcel@hackerman.art>");
MODULE_DESCRIPTION("Ozzy USB Audio Driver");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION(OZZY_GIT_VERSION);

#define DRIVER_NAME "snd-usb-ozzy"

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

static DEFINE_MUTEX(register_mutex);

/* Forward declaration for usb_driver_claim_interface */
static struct usb_driver ozzy_usb_driver;

/* ========================================================================
 * Device Registration Table
 * ======================================================================== */

static const struct ozzy_device_desc ploytec_desc = {
	.info = &ploytec_info,
	.ops  = &ploytec_ops,
};

static const struct ozzy_device_desc ploytec_db2_desc = {
	.info = &ploytec_db2_info,
	.ops  = &ploytec_ops,
};

static const struct usb_device_id ozzy_id_table[] = {
	{ USB_DEVICE(0x0a4a, 0xffdb), .driver_info = (kernel_ulong_t)&ploytec_desc }, /* Xone:DB4 */
	{ USB_DEVICE(0x0a4a, 0xffd2), .driver_info = (kernel_ulong_t)&ploytec_db2_desc }, /* Xone:DB2 */
	{ USB_DEVICE(0x0a4a, 0xffdd), .driver_info = (kernel_ulong_t)&ploytec_desc }, /* Xone:DX */
	{ USB_DEVICE(0x0a4a, 0xff4d), .driver_info = (kernel_ulong_t)&ploytec_desc }, /* Xone:4D */
	{ USB_DEVICE(0x0a4a, 0xffad), .driver_info = (kernel_ulong_t)&ploytec_desc }, /* Wizard 4 */
	{ USB_DEVICE(0x0a4a, 0xff01), .driver_info = (kernel_ulong_t)&ploytec_desc }, /* Xone:2D (experimental) */
	{}
};
MODULE_DEVICE_TABLE(usb, ozzy_id_table);

/* ========================================================================
 * USB Probe / Disconnect
 * ======================================================================== */

/*
 * ozzy_probe - USB interface probe callback.
 *
 * Only handles the primary interface (bInterfaceNumber == 0). Secondary
 * interfaces are claimed via usb_driver_claim_interface() rather than
 * letting the kernel call probe() again for each interface.
 */
static int ozzy_probe(struct usb_interface *intf,
		      const struct usb_device_id *usb_id)
{
	const struct ozzy_device_desc *desc;
	struct usb_device *device;
	struct snd_card *card;
	struct ozzy_chip *chip;
	int i, ret;

	/* Only handle the primary interface -- we claim secondary ones ourselves */
	if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	desc = (const struct ozzy_device_desc *)usb_id->driver_info;
	device = interface_to_usbdev(intf);

	ozzy_log(&device->dev, "build: %s\n", OZZY_GIT_VERSION);
	ozzy_log(&device->dev, "found device: %s\n", device->product);
	ozzy_log(&device->dev, "  manufacturer: %s\n", device->manufacturer ?: "unknown");
	ozzy_log(&device->dev, "  USB ID: %04x:%04x (bcdDevice %04x)\n",
		 le16_to_cpu(device->descriptor.idVendor),
		 le16_to_cpu(device->descriptor.idProduct),
		 le16_to_cpu(device->descriptor.bcdDevice));
	ozzy_log(&device->dev, "  speed: %s\n",
		 device->speed == USB_SPEED_HIGH ? "high (480 Mbps)" :
		 device->speed == USB_SPEED_SUPER ? "super (5 Gbps)" :
		 device->speed == USB_SPEED_FULL ? "full (12 Mbps)" : "unknown");
	ozzy_log(&device->dev, "  device family: %s\n", desc->info->name);

	/* Set alternate setting on primary interface */
	ret = usb_set_interface(device, 0, desc->info->alt_setting);
	if (ret != 0) {
		ozzy_err(&device->dev, "Cannot set interface 0 alt %u\n",
			desc->info->alt_setting);
		return -EIO;
	}

	/* Find a free card slot */
	mutex_lock(&register_mutex);
	for (i = 0; i < SNDRV_CARDS; i++) {
		if (enable[i])
			break;
	}
	if (i >= SNDRV_CARDS) {
		ozzy_err(&device->dev, "No available audio device slot\n");
		mutex_unlock(&register_mutex);
		return -ENODEV;
	}
	mutex_unlock(&register_mutex);

	/* Create ALSA card with chip as private data */
	ret = snd_card_new(&intf->dev, index[i], id[i], THIS_MODULE,
			   sizeof(struct ozzy_chip), &card);
	if (ret < 0) {
		ozzy_err(&device->dev, "Cannot create ALSA card\n");
		return ret;
	}

	chip = card->private_data;
	chip->card = card;
	chip->dev = device;
	chip->info = desc->info;
	chip->ops = desc->ops;
	chip->intf = intf;
	chip->intf2 = NULL;

	/* Default to highest rate */
	chip->requested_rate = chip->info->num_rates - 1;

	strscpy(card->driver, DRIVER_NAME, sizeof(card->driver));
	strscpy(card->shortname, device->product, sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "%s at %d:%d",
		 card->shortname, device->bus->busnum, device->devnum);

	/* Claim secondary interface(s) */
	if (chip->info->num_interfaces > 1) {
		struct usb_interface *intf2 = usb_ifnum_to_if(device, 1);

		if (!intf2) {
			ozzy_err(&device->dev, "Secondary interface not found\n");
			ret = -ENODEV;
			goto err_card;
		}

		ret = usb_driver_claim_interface(&ozzy_usb_driver, intf2, chip);
		if (ret < 0) {
			ozzy_err(&device->dev, "Cannot claim secondary interface\n");
			goto err_card;
		}
		chip->intf2 = intf2;

		ret = usb_set_interface(device, 1, chip->info->alt_setting);
		if (ret != 0) {
			ozzy_err(&device->dev, "Cannot set interface 1 alt %u\n",
				chip->info->alt_setting);
			goto err_intf2;
		}
	}

	/* Detect output endpoint transfer type */
	if (device->ep_out[chip->info->out_ep]) {
		const struct usb_endpoint_descriptor *epd = &device->ep_out[chip->info->out_ep]->desc;

		chip->is_bulk = usb_endpoint_xfer_bulk(epd);
		chip->out_maxpacket = usb_endpoint_maxp(epd);
	}

	ozzy_log(&device->dev, "  interfaces claimed: %u (alt setting %u)\n",
		 chip->info->num_interfaces, chip->info->alt_setting);
	ozzy_log(&device->dev, "  PCM out: EP %u (%s, wMaxPacketSize %u)\n",
		 chip->info->out_ep, chip->is_bulk ? "bulk" : "interrupt",
		 chip->out_maxpacket);
	ozzy_log(&device->dev, "  PCM in:  EP %u\n", chip->info->in_ep);
	if (chip->info->midi_in_ep)
		ozzy_log(&device->dev, "  MIDI in: EP %u%s\n",
			 chip->info->midi_in_ep,
			 chip->info->midi_out_embedded ? " (out embedded in PCM)" : "");
	ozzy_log(&device->dev, "  channels: %u in / %u out\n",
		 chip->info->capture_channels, chip->info->playback_channels);

	/* Device-specific initialization (vendor handshake) */
	ret = chip->ops->init(chip);
	if (ret < 0) {
		ozzy_err(&device->dev, "Device initialization failed\n");
		goto err_intf2;
	}

	/* Initialize PCM subsystem */
	ret = ozzy_pcm_init(chip);
	if (ret < 0) {
		ozzy_err(&device->dev, "PCM initialization failed\n");
		goto err_device;
	}

	/* Initialize MIDI subsystem (if device has MIDI) */
	if (chip->info->midi_in_ep || chip->info->midi_out_embedded) {
		ret = ozzy_midi_init(chip);
		if (ret < 0) {
			ozzy_err(&device->dev, "MIDI initialization failed\n");
			goto err_device;
		}
	}

	/* Register the card */
	ret = snd_card_register(card);
	if (ret < 0) {
		ozzy_err(&device->dev, "Cannot register ALSA card\n");
		goto err_device;
	}

	usb_set_intfdata(intf, chip);
	return 0;

err_device:
	if (chip->ops->free)
		chip->ops->free(chip);
err_intf2:
	if (chip->intf2)
		usb_driver_release_interface(&ozzy_usb_driver, chip->intf2);
err_card:
	snd_card_free(card);
	return ret;
}

/*
 * ozzy_disconnect - USB interface disconnect callback.
 * Stops all URBs, releases secondary interfaces, and frees the card.
 */
static void ozzy_disconnect(struct usb_interface *intf)
{
	struct ozzy_chip *chip = usb_get_intfdata(intf);

	if (!chip)
		return;

	/* If this is the secondary interface being unbound, ignore it */
	if (intf != chip->intf)
		return;

	ozzy_pcm_drain(chip);
	ozzy_pcm_abort(chip);
	ozzy_midi_abort(chip);

	if (chip->ops->free)
		chip->ops->free(chip);

	if (chip->intf2)
		usb_driver_release_interface(&ozzy_usb_driver, chip->intf2);

	snd_card_disconnect(chip->card);
	snd_card_free_when_closed(chip->card);
}

/* ========================================================================
 * USB Reset Callbacks (eliminates the old justresetting global)
 * ======================================================================== */

/*
 * ozzy_pre_reset - Called before USB device reset.
 * Stops all URBs so the device can be safely reset.
 */
static int ozzy_pre_reset(struct usb_interface *intf)
{
	struct ozzy_chip *chip = usb_get_intfdata(intf);

	if (!chip)
		return 0;

	/* Only handle from primary interface */
	if (intf != chip->intf)
		return 0;

	ozzy_pcm_abort(chip);
	ozzy_midi_abort(chip);
	return 0;
}

/*
 * ozzy_post_reset - Called after USB device reset completes.
 * Re-initializes the device handshake and restarts URBs.
 */
static int ozzy_post_reset(struct usb_interface *intf)
{
	struct ozzy_chip *chip = usb_get_intfdata(intf);
	int ret;

	if (!chip)
		return 0;

	/* Only handle from primary interface */
	if (intf != chip->intf)
		return 0;

	/* Re-run device handshake */
	ret = chip->ops->init(chip);
	if (ret < 0) {
		ozzy_err(&chip->dev->dev, "Post-reset init failed\n");
		return ret;
	}

	/* Restart URBs */
	ret = ozzy_pcm_init_urbs(chip);
	if (ret < 0) {
		ozzy_err(&chip->dev->dev, "Post-reset PCM URB init failed\n");
		return ret;
	}

	if (chip->info->midi_in_ep) {
		ret = ozzy_midi_init_urbs(chip);
		if (ret < 0) {
			ozzy_err(&chip->dev->dev, "Post-reset MIDI URB init failed\n");
			return ret;
		}
	}

	return 0;
}

/* ========================================================================
 * Module Registration
 * ======================================================================== */

static struct usb_driver ozzy_usb_driver = {
	.name        = DRIVER_NAME,
	.probe       = ozzy_probe,
	.disconnect  = ozzy_disconnect,
	/* Don't let the USB core kill our URBs before ozzy_disconnect drains them */
	.soft_unbind = 1,
	.pre_reset   = ozzy_pre_reset,
	.post_reset  = ozzy_post_reset,
	.id_table    = ozzy_id_table,
};

module_usb_driver(ozzy_usb_driver);
