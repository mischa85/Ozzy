#ifndef WSTREAM_H
#define WSTREAM_H

#include "device.h"

enum { WSTREAM_MAX_PACKETS = 8 };

/* One ACX RT stream (event-driven), the context of its ACXSTREAM. Its WaveRT packets are one
 * contiguous S24_3LE ring that the engine reads (render) or writes (capture) in the USB
 * completion path; wstream_progress, called from there, turns the ring position into packet
 * completions and positions. */
struct wstream {
    ACXSTREAM stream;
    struct wdev* dev;
    bool render;
    uint32_t block_align;
    uint32_t rate;                    /* the format's */
    bool holding;                     /* running: counted in the device's holding[] */

    /* The packets: one allocation, the data contiguous from buffer + data_offset */
    uint8_t* buffer;
    ULONG data_offset;
    ULONG packet_count;
    ULONG packet_frames;
    PMDL mdl[WSTREAM_MAX_PACKETS];

    struct ozzy_pcm_ring ring;
    bool attached;

    /* Written in wstream_progress, read by the RT callbacks */
    volatile LONG64 pos;              /* ring position: frames since the first run */
    volatile LONG64 pos_qpc;
    volatile LONG64 completed;        /* packets completed (notified) */
    volatile LONG64 last_start_qpc;   /* capture: when the last completed packet started */
    uint64_t boundary_qpc;            /* when the packet in progress started (DISPATCH only) */
    uint64_t next_boundary;           /* ring position where that packet ends (DISPATCH only) */
};
typedef struct wstream wstream;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(wstream, wstream_get)

/* DISPATCH_LEVEL: the engine moved a packet through the ring. @frames: ring position after
 * it; @qpc: KeQueryPerformanceCounter at completion */
void wstream_progress(struct wstream* s, uint64_t frames, uint64_t qpc);

/* The latency @s reports to Windows (its hardware FIFO), in frames */
uint32_t wstream_fifo_frames(const struct wstream* s);

/* The circuits' EvtAcxCircuitCreateStream */
EVT_ACX_CIRCUIT_CREATE_STREAM wstream_evt_create;

#endif
