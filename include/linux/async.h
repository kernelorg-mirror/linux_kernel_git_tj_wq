/*
 * async.h: Asynchronous function calls for boot performance
 *
 * (C) Copyright 2009 Intel Corporation
 * Author: Arjan van de Ven <arjan@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <linux/types.h>
#include <linux/workqueue.h>

typedef void (*async_func_t)(void *data);

extern bool async_call(async_func_t func, void *data);
extern bool async_call_ordered(async_func_t func, void *data);
extern void async_barrier(void);
