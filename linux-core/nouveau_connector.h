/*
 * Copyright (C) 2008 Maarten Maathuis.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef __NOUVEAU_CONNECTOR_H__
#define __NOUVEAU_CONNECTOR_H__

#include "nv50_i2c.h"

struct nouveau_connector {
	struct drm_connector base;

	struct drm_display_mode *native_mode;
	bool digital;

	int bus;
	struct nv50_i2c_channel *i2c_chan;

	int scaling_mode;

	bool use_dithering;

	struct nouveau_encoder *(*to_encoder) (struct nouveau_connector *connector, bool digital);
};
#define to_nouveau_connector(x) container_of((x), struct nouveau_connector, base)

int nv50_connector_create(struct drm_device *dev, int bus, int i2c_index, int type);
void nv50_connector_detect_all(struct drm_device *dev);

#endif /* __NOUVEAU_CONNECTOR_H__ */