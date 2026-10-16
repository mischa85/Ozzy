/* SPDX-License-Identifier: MIT */
/*
 * Ploytec Device Family - Allen & Heath Xone Series
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_PLOYTEC_H
#define OZZY_PLOYTEC_H

#include "../ozzy.h"

extern const struct ozzy_device_info ploytec_info;
extern const struct ozzy_device_ops ploytec_ops;

/* Bulk in / isochronous out (Akai EIE Pro) */
extern const struct ozzy_device_info ploytec_isoc_info;
extern const struct ozzy_device_ops ploytec_isoc_ops;

#endif /* OZZY_PLOYTEC_H */
