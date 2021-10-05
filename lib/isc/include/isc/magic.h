/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

#pragma once

#include <isc/likely.h>

/*! \file isc/magic.h */

typedef struct {
	unsigned int magic;
} isc__magic_t;

/*%
 * To use this macro the magic number MUST be the first thing in the
 * structure, and MUST be of type "unsigned int".
 * The intent of this is to allow magic numbers to be checked even though
 * the object is otherwise opaque.
 */
#define ISC_MAGIC_VALID(a, b)       \
	(ISC_LIKELY((a) != NULL) && \
	 ISC_LIKELY(((const isc__magic_t *)(a))->magic == (b)))

#define ISC_MAGIC(a, b, c, d) ((a) << 24 | (b) << 16 | (c) << 8 | (d))
