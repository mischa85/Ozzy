// SPDX-License-Identifier: MIT
/*
 * Ozzy USB Audio Driver - probe and lifecycle on the shared engine
 *
 * The device's family engine (engine/) says which interfaces and endpoints it needs; this
 * claims them, gives the engine its bus and memory, runs the handshake and puts ALSA PCM,
 * rawmidi and the settings controls on top. The device streams from probe to disconnect; a
 * rate change or a device setting restarts the engine (ozzy_engine_restart: no USB reset).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <sound/initval.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "ozzy.h"
#include "../common/ozzy_version.h"

MODULE_AUTHOR("Marcel Bierling <marcel@hackerman.art>");
MODULE_DESCRIPTION("Ozzy USB Audio Driver");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION(OZZY_GIT_VERSION);

#define DRIVER_NAME "snd-usb-ozzy"

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;

static struct usb_driver ozzy_driver;

/*
 * The USB ID table, from common/ozzy_devices.def: every device of a family the engine drives,
 * except USB MIDI class devices (snd-usb-audio drives those on Linux)
 */
#define OZZY_LINUX_ROW_PLOYTEC_XONE(vid, pid) { USB_DEVICE(vid, pid) },
#define OZZY_LINUX_ROW_PLOYTEC_ISOC(vid, pid) { USB_DEVICE(vid, pid) },
#define OZZY_LINUX_ROW_USB_MIDI(vid, pid)

static const struct usb_device_id ozzy_id_table[] = {
#define OZZY_DEVICE(key, vid, pid, name, family, in, out, rates, status, quirks) \
	OZZY_LINUX_ROW_##family(vid, pid)
#include "../common/ozzy_devices.def"
#undef OZZY_DEVICE
	{}
};
MODULE_DEVICE_TABLE(usb, ozzy_id_table);

/*
 * ozzy_chip_restart - Process context, chip->lock held: apply the engine's rate and settings.
 * Running streams stop with an xrun, then the engine restarts with what changed.
 */
int ozzy_chip_restart(struct ozzy_chip *chip)
{
	ozzy_pcm_stop_streams(chip);
	if (chip->gone)
		return -ENODEV;
	chip->running = ozzy_engine_restart(chip->engine);
	return chip->running ? 0 : -EIO;
}

/* Claim the profile's interfaces (the probed one is interface 0) and select its alternate setting */
static int ozzy_chip_claim(struct ozzy_chip *chip, struct usb_interface *probed)
{
	const struct ozzy_device_profile *p = &chip->engine->profile;
	unsigned int i;
	int ret;

	for (i = 0; i < p->interface_count && i < OZZY_MAX_INTERFACES; i++) {
		struct usb_interface *intf = usb_ifnum_to_if(chip->dev, p->interfaces[i]);

		if (!intf) {
			chip_err(chip, "interface %u not found", p->interfaces[i]);
			return -ENODEV;
		}
		if (intf != probed) {
			ret = usb_driver_claim_interface(&ozzy_driver, intf, chip);
			if (ret < 0) {
				chip_err(chip, "cannot claim interface %u", p->interfaces[i]);
				return ret;
			}
		}
		chip->intf[chip->num_intf++] = intf;
		if (p->alt_setting) {
			ret = usb_set_interface(chip->dev, p->interfaces[i], p->alt_setting);
			if (ret < 0) {
				chip_err(chip, "interface %u: alternate setting %u failed", p->interfaces[i], p->alt_setting);
				return ret;
			}
		}
	}
	return 0;
}

/* Every endpoint of the claimed interfaces to the engine, which picks its pipes; their URBs */
static int ozzy_chip_pipes(struct ozzy_chip *chip)
{
	struct ozzy_usb_endpoint eps[32];
	const struct ozzy_usb_endpoint *bound[OZZY_PIPE_ROLES];
	unsigned int n = 0, i, k;
	int ret;

	for (i = 0; i < chip->num_intf; i++) {
		const struct usb_host_interface *alt = chip->intf[i]->cur_altsetting;

		for (k = 0; k < alt->desc.bNumEndpoints && n < ARRAY_SIZE(eps); k++) {
			struct usb_endpoint_descriptor *epd = &alt->endpoint[k].desc;

			eps[n].intf = alt->desc.bInterfaceNumber;
			eps[n].address = epd->bEndpointAddress;
			eps[n].attributes = epd->bmAttributes;
			eps[n].interval = epd->bInterval;
			eps[n].max_packet = le16_to_cpu(epd->wMaxPacketSize);
			eps[n++].handle = epd;
		}
	}
	ozzy_engine_bind_endpoints(chip->engine, eps, n, bound);
	for (i = 0; i < OZZY_PIPE_ROLES; i++) {
		if (!bound[i])
			continue;
		ret = ozzy_urb_add_pipe(&chip->bus, bound[i]->handle);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* The engine's transfer memory: DMA-capable kmalloc, each buffer cache-line aligned */
static void *ozzy_kmalloc_alloc(void *ctx, size_t size)
{
	return kmalloc(size, GFP_KERNEL);
}

static void ozzy_kmalloc_free(void *ctx, void *p)
{
	kfree(p);
}

static const struct ozzy_allocator ozzy_kmalloc = {
	.alloc = ozzy_kmalloc_alloc,
	.free  = ozzy_kmalloc_free,
};

/* The card's last reference is gone: free what the engine and the bus had */
static void ozzy_chip_card_free(struct snd_card *card)
{
	struct ozzy_chip *chip = card->private_data;

	ozzy_urb_free(&chip->bus);
	if (chip->engine)
		ozzy_engine_free_memory(chip->engine);
	kfree(chip->engine);
	vfree(chip->shm);
}

static void ozzy_chip_release_interfaces(struct ozzy_chip *chip)
{
	unsigned int i;

	for (i = 1; i < chip->num_intf; i++)
		usb_driver_release_interface(&ozzy_driver, chip->intf[i]);
	chip->num_intf = 0;
}

static int ozzy_chip_probe(struct usb_interface *intf, const struct usb_device_id *usb_id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	const struct ozzy_device *model;
	struct ozzy_chip *chip;
	struct snd_card *card;
	size_t size;
	int ret;

	/* Interface 0 only: the others are claimed from here */
	if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;
	model = ozzy_device_lookup(le16_to_cpu(dev->descriptor.idVendor), le16_to_cpu(dev->descriptor.idProduct));
	if (!model)
		return -ENODEV;

	ret = snd_card_new(&intf->dev, index[0], id[0], THIS_MODULE, sizeof(*chip), &card);
	if (ret < 0)
		return ret;
	chip = card->private_data;
	chip->card = card;
	chip->dev = dev;
	chip->model = model;
	mutex_init(&chip->lock);
	/* Before anything runs: completions reach the PCM and MIDI sides once the engine starts */
	spin_lock_init(&chip->stream[OZZY_PCM_OUT].lock);
	spin_lock_init(&chip->stream[OZZY_PCM_IN].lock);
	spin_lock_init(&chip->midi.lock);
	card->private_free = ozzy_chip_card_free;
	strscpy(card->driver, DRIVER_NAME, sizeof(card->driver));
	strscpy(card->shortname, dev->product ?: model->name, sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "%s at %d:%d", card->shortname, dev->bus->busnum,
		 dev->devnum);

	chip_log(chip, "build %s (engine), %s (%04x:%04x)%s", OZZY_GIT_VERSION, model->name, model->vid, model->pid,
		  model->status == OZZY_STATUS_EXPERIMENTAL ? ", experimental" : "");

	size = ozzy_engine_size(model);
	chip->engine = ozzy_engine_init(size ? kzalloc(size, GFP_KERNEL) : NULL, model);
	chip->shm = vzalloc(sizeof(*chip->shm));
	ret = !chip->engine || !chip->shm ? -ENOMEM : ozzy_urb_init(&chip->bus, chip);
	if (ret < 0)
		goto err_card;
	if (dev->product)
		ozzy_strlcpy(chip->shm->product_name, dev->product, sizeof(chip->shm->product_name));

	ret = ozzy_chip_claim(chip, intf);
	if (ret == 0)
		ret = ozzy_chip_pipes(chip);
	if (ret < 0)
		goto err_intf;

	ozzy_engine_attach(chip->engine, &chip->bus.bus, chip->shm);
	if (!ozzy_engine_alloc_memory(chip->engine, &ozzy_kmalloc)) {
		ret = -ENOMEM;
		goto err_intf;
	}

	mutex_lock(&chip->lock);
	chip->running = ozzy_engine_start(chip->engine);
	mutex_unlock(&chip->lock);
	if (!chip->running) {
		chip_err(chip, "device initialization failed");
		ret = -EIO;
		goto err_stop;
	}
	chip_log(chip, "running at %u Hz, %u in / %u out", (unsigned int)chip->shm->audio.sample_rate,
		  model->in_channels, model->out_channels);

	ret = ozzy_pcm_init(chip);
	if (ret == 0)
		ret = ozzy_midi_init(chip);
	if (ret == 0)
		ret = ozzy_controls_init(chip);
	if (ret == 0)
		ret = snd_card_register(card);
	if (ret < 0)
		goto err_stop;

	usb_set_intfdata(intf, chip);
	return 0;

err_stop:
	ozzy_engine_stop(chip->engine);
err_intf:
	ozzy_chip_release_interfaces(chip);
err_card:
	snd_card_free(card);
	return ret;
}

static void ozzy_chip_disconnect(struct usb_interface *intf)
{
	struct ozzy_chip *chip = usb_get_intfdata(intf);

	/* The claimed interfaces come here too: the device goes with interface 0 */
	if (!chip || intf != chip->intf[0])
		return;

	mutex_lock(&chip->lock);
	chip->gone = true;
	chip->running = false;
	ozzy_engine_stop(chip->engine);
	mutex_unlock(&chip->lock);

	snd_card_disconnect(chip->card);
	ozzy_chip_release_interfaces(chip);
	snd_card_free_when_closed(chip->card);
}

/* Someone else resets the device (usbfs, a hub): stop around it, then the handshake again */
static int ozzy_chip_pre_reset(struct usb_interface *intf)
{
	struct ozzy_chip *chip = usb_get_intfdata(intf);

	if (!chip || intf != chip->intf[0])
		return 0;
	mutex_lock(&chip->lock);
	ozzy_pcm_stop_streams(chip);
	ozzy_engine_suspend(chip->engine);
	return 0;
}

static int ozzy_chip_post_reset(struct usb_interface *intf)
{
	struct ozzy_chip *chip = usb_get_intfdata(intf);
	const struct ozzy_device_profile *p;
	unsigned int i;

	if (!chip || intf != chip->intf[0])
		return 0;
	p = &chip->engine->profile;
	for (i = 0; i < p->interface_count && p->alt_setting; i++)
		usb_set_interface(chip->dev, p->interfaces[i], p->alt_setting);
	chip->running = ozzy_engine_start(chip->engine);
	if (!chip->running)
		chip_err(chip, "restart after reset failed");
	mutex_unlock(&chip->lock);
	return 0;
}

static struct usb_driver ozzy_driver = {
	.name        = DRIVER_NAME,
	.probe       = ozzy_chip_probe,
	.disconnect  = ozzy_chip_disconnect,
	.soft_unbind = 1,
	.pre_reset   = ozzy_chip_pre_reset,
	.post_reset  = ozzy_chip_post_reset,
	.id_table    = ozzy_id_table,
};

module_usb_driver(ozzy_driver);
