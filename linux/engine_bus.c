// SPDX-License-Identifier: MIT
/*
 * Ozzy USB Audio Driver - the engine's bus on URBs (make OZZY_ENGINE=1)
 *
 * Every transfer buffer is its own kmalloc (DMA-capable, OZZY_DMA_ALIGN apart), handed to the
 * engine as its transfer memory; the shared memory stays CPU only. URBs are allocated once per
 * pipe and reused; completions hand the result to the engine, which resubmits and moves PCM
 * and MIDI right there, then the PCM and MIDI sides catch up (epcm_progress, emidi_service).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/version.h>

#include "engine_chip.h"

#define LBUS_CTRL_BUF  64

static struct lbus *to_lbus(void *ctx)
{
	return ctx;
}

static struct lbus_pipe *lbus_find(struct lbus *bus, uint8_t address)
{
	unsigned int i;

	for (i = 0; i < bus->num_pipes; i++)
		if (bus->pipes[i].address == address)
			return &bus->pipes[i];
	return NULL;
}

/* A killed, unlinked or disconnected URB is "aborted" to the engine: no resubmit */
static int lbus_status(int status)
{
	switch (status) {
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		return OZZY_STATUS_ABORTED;
	default:
		return status;
	}
}

/* Interrupt context: release the URB, hand the result to the engine, then let PCM and MIDI catch up */
static void lbus_complete(struct urb *urb)
{
	struct lbus_urb *u = urb->context;
	struct ozzy_echip *chip = u->chip;
	struct lbus *bus = &chip->bus;
	struct lbus_pipe *p = &bus->pipes[u->pipe];
	void *xfer = u->xfer;
	int status = lbus_status(urb->status);
	unsigned int i;

	atomic_inc(&bus->in_completion);
	if (p->type == USB_ENDPOINT_XFER_ISOC) {
		struct ozzy_isoc_packet *packets = u->packets;
		uint64_t now = ktime_get_ns();

		for (i = 0; i < (unsigned int)urb->number_of_packets; i++) {
			packets[i].actual = urb->iso_frame_desc[i].actual_length;
			packets[i].status = urb->iso_frame_desc[i].status;
			packets[i].time = now;
		}
		clear_bit(u->index, &p->busy);
		ozzy_engine_isoc_complete(chip->engine, p->address, xfer, status, packets, urb->number_of_packets);
	} else {
		clear_bit(u->index, &p->busy);
		ozzy_engine_complete(chip->engine, p->address, xfer, status, urb->actual_length);
	}
	emidi_service(chip);
	atomic_dec(&bus->in_completion);
}

/* A free URB of @p, its bit claimed; NULL if all are in flight */
static struct lbus_urb *lbus_claim(struct lbus_pipe *p)
{
	unsigned int i;

	for (i = 0; i < LBUS_URBS; i++)
		if (p->urbs[i].urb && !test_and_set_bit(i, &p->busy))
			return &p->urbs[i];
	return NULL;
}

static bool lbus_submit(void *ctx, uint8_t pipe, void *buf, uint32_t size, void *xfer)
{
	struct lbus *bus = to_lbus(ctx);
	struct lbus_pipe *p = lbus_find(bus, pipe);
	struct lbus_urb *u;
	int ret;

	if (!p || bus->chip->gone)
		return false;
	u = lbus_claim(p);
	if (!u)
		return false;
	u->xfer = xfer;
	if (p->type == USB_ENDPOINT_XFER_INT)
		usb_fill_int_urb(u->urb, bus->dev, p->usb_pipe, buf, size, lbus_complete, u, p->interval);
	else
		usb_fill_bulk_urb(u->urb, bus->dev, p->usb_pipe, buf, size, lbus_complete, u);
	usb_anchor_urb(u->urb, &bus->anchor);
	ret = usb_submit_urb(u->urb, GFP_ATOMIC);
	if (ret < 0) {
		usb_unanchor_urb(u->urb);
		clear_bit(u->index, &p->busy);
		if (ret != -ENODEV && ret != -EPERM)
			dev_err_ratelimited(&bus->dev->dev, "[ozzy] pipe 0x%02x submit failed (%d)\n", pipe, ret);
		return false;
	}
	return true;
}

/* Isochronous: the engine's packets one after the other in @buf, which they never wrap (the
 * engine's per-transfer buffers on Linux) */
static bool lbus_submit_isoc(void *ctx, uint8_t pipe, void *buf, uint32_t buf_size, struct ozzy_isoc_packet *packets,
			     uint32_t count, void *xfer)
{
	struct lbus *bus = to_lbus(ctx);
	struct lbus_pipe *p = lbus_find(bus, pipe);
	struct lbus_urb *u;
	struct urb *urb;
	uint32_t i, end = 0;
	int ret;

	if (!p || bus->chip->gone || !count || count > OZZY_MAX_ISOC_PACKETS)
		return false;
	for (i = 0; i < count; i++) {
		if (packets[i].offset + packets[i].length > buf_size)
			return false;
		end = max(end, packets[i].offset + packets[i].length);
	}
	u = lbus_claim(p);
	if (!u)
		return false;
	u->xfer = xfer;
	u->packets = packets;
	urb = u->urb;
	urb->dev = bus->dev;
	urb->pipe = p->usb_pipe;
	urb->transfer_flags = URB_ISO_ASAP;
	urb->transfer_buffer = buf;
	urb->transfer_buffer_length = end;
	urb->number_of_packets = count;
	urb->interval = 1 << (clamp_t(unsigned int, p->interval, 1, 16) - 1);
	urb->complete = lbus_complete;
	urb->context = u;
	for (i = 0; i < count; i++) {
		urb->iso_frame_desc[i].offset = packets[i].offset;
		urb->iso_frame_desc[i].length = packets[i].length;
	}
	usb_anchor_urb(urb, &bus->anchor);
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		usb_unanchor_urb(urb);
		clear_bit(u->index, &p->busy);
		if (ret != -ENODEV && ret != -EPERM)
			dev_err_ratelimited(&bus->dev->dev, "[ozzy] isoc pipe 0x%02x submit failed (%d)\n", pipe, ret);
		return false;
	}
	return true;
}

/* Process context (the handshake): through a kmalloc'd buffer, as the engine passes stack memory */
static bool lbus_control(void *ctx, uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void *data, uint16_t len)
{
	struct lbus *bus = to_lbus(ctx);
	bool in = type & USB_DIR_IN;
	int ret;

	if (len > LBUS_CTRL_BUF)
		return false;
	if (!in && len)
		memcpy(bus->ctrl_buf, data, len);
	ret = usb_control_msg(bus->dev, in ? usb_rcvctrlpipe(bus->dev, 0) : usb_sndctrlpipe(bus->dev, 0), req, type,
			      val, idx, len ? bus->ctrl_buf : NULL, len, 5000);
	if (ret < 0) {
		dev_err(&bus->dev->dev, "[ozzy] control request 0x%02x (type 0x%02x, wValue 0x%04x) failed (%d)\n",
			req, type, val, ret);
		return false;
	}
	if (in && len)
		memcpy(data, bus->ctrl_buf, len);
	return true;
}

static int lbus_clear_halt(void *ctx, uint8_t endpoint)
{
	struct lbus *bus = to_lbus(ctx);
	unsigned int num = endpoint & USB_ENDPOINT_NUMBER_MASK;

	return usb_clear_halt(bus->dev, (endpoint & USB_DIR_IN) ? usb_rcvbulkpipe(bus->dev, num) :
								  usb_sndbulkpipe(bus->dev, num));
}

static enum hrtimer_restart lbus_tick(struct hrtimer *timer)
{
	struct lbus *bus = container_of(timer, struct lbus, timer);

	if (!READ_ONCE(bus->timer_on))
		return HRTIMER_NORESTART;
	ozzy_engine_timer(bus->chip->engine);
	emidi_service(bus->chip);
	hrtimer_forward_now(timer, bus->timer_interval);
	return HRTIMER_RESTART;
}

static bool lbus_start_timer(void *ctx, uint32_t interval_us)
{
	struct lbus *bus = to_lbus(ctx);

	bus->timer_interval = ns_to_ktime((u64)interval_us * NSEC_PER_USEC);
	WRITE_ONCE(bus->timer_on, true);
	hrtimer_start(&bus->timer, bus->timer_interval, HRTIMER_MODE_REL_SOFT);
	return true;
}

/* Waits for a call in progress */
static void lbus_stop_timer(void *ctx)
{
	struct lbus *bus = to_lbus(ctx);

	WRITE_ONCE(bus->timer_on, false);
	hrtimer_cancel(&bus->timer);
}

static void lbus_pcm_progress(void *ctx, enum ozzy_pcm_dir dir, uint64_t frames)
{
	epcm_progress(to_lbus(ctx)->chip, dir, frames);
}

static void lbus_sleep(void *ctx, unsigned int ms)
{
	msleep(ms);
}

static uint64_t lbus_time(void *ctx)
{
	return ktime_get_ns();
}

static const struct ozzy_bus_ops lbus_ops = {
	.submit       = lbus_submit,
	.submit_isoc  = lbus_submit_isoc,
	.control      = lbus_control,
	.clear_halt   = lbus_clear_halt,
	.start_timer  = lbus_start_timer,
	.stop_timer   = lbus_stop_timer,
	.pcm_progress = lbus_pcm_progress,
	.sleep_ms     = lbus_sleep,
	.time         = lbus_time,
};

int lbus_init(struct lbus *bus, struct ozzy_echip *chip)
{
	memset(bus, 0, sizeof(*bus));
	bus->bus.ops = &lbus_ops;
	bus->bus.ctx = bus;
	bus->chip = chip;
	bus->dev = chip->dev;
	init_usb_anchor(&bus->anchor);
	atomic_set(&bus->in_completion, 0);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	hrtimer_setup(&bus->timer, lbus_tick, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
#else
	hrtimer_init(&bus->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
	bus->timer.function = lbus_tick;
#endif
	bus->ctrl_buf = kmalloc(LBUS_CTRL_BUF, GFP_KERNEL);
	return bus->ctrl_buf ? 0 : -ENOMEM;
}

/* A pipe the engine found (OzzyKext::ConfigureHardware's job): its URBs, allocated once */
int lbus_add_pipe(struct lbus *bus, const struct usb_endpoint_descriptor *epd)
{
	struct lbus_pipe *p;
	unsigned int num = usb_endpoint_num(epd), i;
	bool in = usb_endpoint_dir_in(epd);

	if (bus->num_pipes == LBUS_MAX_PIPES)
		return -ENOSPC;
	p = &bus->pipes[bus->num_pipes];
	p->address = epd->bEndpointAddress;
	p->type = usb_endpoint_type(epd);
	p->interval = epd->bInterval;
	switch (p->type) {
	case USB_ENDPOINT_XFER_ISOC:
		p->usb_pipe = in ? usb_rcvisocpipe(bus->dev, num) : usb_sndisocpipe(bus->dev, num);
		break;
	case USB_ENDPOINT_XFER_INT:
		p->usb_pipe = in ? usb_rcvintpipe(bus->dev, num) : usb_sndintpipe(bus->dev, num);
		break;
	default:
		p->usb_pipe = in ? usb_rcvbulkpipe(bus->dev, num) : usb_sndbulkpipe(bus->dev, num);
		break;
	}
	for (i = 0; i < LBUS_URBS; i++) {
		p->urbs[i].urb = usb_alloc_urb(p->type == USB_ENDPOINT_XFER_ISOC ? OZZY_MAX_ISOC_PACKETS : 0, GFP_KERNEL);
		if (!p->urbs[i].urb)
			return -ENOMEM;
		p->urbs[i].chip = bus->chip;
		p->urbs[i].pipe = bus->num_pipes;
		p->urbs[i].index = i;
	}
	bus->num_pipes++;
	return 0;
}

/*
 * lbus_alloc_memory - The engine's transfer memory: every buffer its own kmalloc (DMA-capable,
 * cache-line aligned), sized for the transfers in flight, not for the HAL's rings.
 */
int lbus_alloc_memory(struct lbus *bus, const struct ozzy_engine *engine)
{
	struct ozzy_engine_memory *m = &bus->mem;
	unsigned int i, out_maxp = 0, fb_maxp = 0;

	for (i = 0; i < bus->num_pipes; i++) {
		const struct lbus_pipe *p = &bus->pipes[i];
		const struct usb_host_endpoint *ep = (p->address & USB_DIR_IN) ? bus->dev->ep_in[p->address & 0x0F] :
										 bus->dev->ep_out[p->address & 0x0F];
		unsigned int maxp = ep ? usb_endpoint_maxp(&ep->desc) * usb_endpoint_maxp_mult(&ep->desc) : 0;

		if (p->type != USB_ENDPOINT_XFER_ISOC)
			continue;
		if (p->address & USB_DIR_IN)
			fb_maxp = max(fb_maxp, maxp);
		else
			out_maxp = max(out_maxp, maxp);
	}

	memset(m, 0, sizeof(*m));
	for (i = 0; i < LBUS_PCM_SLOTS; i++) {
		m->pcm_out[i] = kzalloc(OZZY_PCM_SLOT_MIN, GFP_KERNEL);
		m->pcm_in[i] = kzalloc(OZZY_PCM_SLOT_MIN, GFP_KERNEL);
		if (!m->pcm_out[i] || !m->pcm_in[i])
			return -ENOMEM;
	}
	m->pcm_out_slots = m->pcm_in_slots = LBUS_PCM_SLOTS;
	if (out_maxp) {
		/* One OUT transfer: at most a packet per microframe, each at most wMaxPacketSize */
		m->isoc_out_size = OZZY_DMA_ROUND(OZZY_MAX_ISOC_PACKETS * out_maxp);
		for (i = 0; i < ISOC_OUT_TRANSFERS; i++) {
			m->isoc_out[i] = kzalloc(m->isoc_out_size, GFP_KERNEL);
			if (!m->isoc_out[i])
				return -ENOMEM;
		}
	}
	m->isoc_in_size = ISOC_FB_TRANSFERS * OZZY_DMA_ROUND(OZZY_MAX_ISOC_PACKETS * max(fb_maxp, 4u));
	m->isoc_in = kzalloc(m->isoc_in_size, GFP_KERNEL);
	m->midi_in = kzalloc(OZZY_MIDI_IN_SIZE, GFP_KERNEL);
	m->midi_out = kzalloc(OZZY_DMA_ROUND(OZZY_MIDI_OUT_SIZE), GFP_KERNEL);
	if (!m->isoc_in || !m->midi_in || !m->midi_out)
		return -ENOMEM;
	return 0;
}

/* Process context: kill everything in flight (the engine has stopped resubmitting) */
void lbus_kill(struct lbus *bus)
{
	lbus_stop_timer(bus);
	usb_kill_anchored_urbs(&bus->anchor);
}

/*
 * lbus_wait_completions - Process context: wait until no completion runs. One that started
 * before a ring was detached may still use it; once none runs, every later one sees the detach.
 */
void lbus_wait_completions(struct lbus *bus)
{
	unsigned int i;

	for (i = 0; i < 100 && atomic_read(&bus->in_completion); i++)
		usleep_range(500, 1000);
}

void lbus_free(struct lbus *bus)
{
	struct ozzy_engine_memory *m = &bus->mem;
	unsigned int i, k;

	for (i = 0; i < bus->num_pipes; i++)
		for (k = 0; k < LBUS_URBS; k++)
			usb_free_urb(bus->pipes[i].urbs[k].urb);
	bus->num_pipes = 0;
	for (i = 0; i < OZZY_NUM_PACKETS; i++) {
		kfree(m->pcm_out[i]);
		kfree(m->pcm_in[i]);
	}
	for (i = 0; i < ISOC_OUT_TRANSFERS; i++)
		kfree(m->isoc_out[i]);
	kfree(m->isoc_in);
	kfree(m->midi_in);
	kfree(m->midi_out);
	memset(m, 0, sizeof(*m));
	kfree(bus->ctrl_buf);
	bus->ctrl_buf = NULL;
}
