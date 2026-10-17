/* SPDX-License-Identifier: MIT */
/*
 * Ozzy USB Audio Driver - Device Settings Controls
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_CONTROLS_H
#define OZZY_CONTROLS_H

struct ozzy_chip;

/*
 * ozzy_controls_init - Add the ALSA controls for the settings this device has
 * (common/ozzy_settings.h, from chip->quirks). Call after ops->init and before
 * the card is registered. Returns 0 on success, negative errno on failure.
 */
int ozzy_controls_init(struct ozzy_chip *chip);

#endif /* OZZY_CONTROLS_H */
