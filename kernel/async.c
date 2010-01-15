/*
 * async.c: Asynchronous function calls for boot performance
 *
 * (C) Copyright 2009 Intel Corporation
 * Author: Arjan van de Ven <arjan@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */


/*

The primary goal of this feature is to reduce the kernel boot time,
by doing various independent hardware delays and discovery operations
decoupled and not strictly serialized.

More specifically, the asynchronous function call concept allows
certain operations (primarily during system boot) to happen
asynchronously, out of order, while these operations still
have their externally visible parts happen sequentially and in-order.
(not unlike how out-of-order CPUs retire their instructions in order)

Parts can be executed in parallel should be scheduled via async_call()
while parts which need to be executed sequentially to implement
in-order appearance via async_call_ordered().

*/

#include <linux/async.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/delay.h>

extern int initcall_debug;

struct async_ent {
	struct work_struct	work;
	async_func_t		func;
	void			*data;
	bool			ordered;
};

static struct workqueue_struct *async_wq;
static struct workqueue_struct *async_ordered_wq;

static void async_work_func(struct work_struct *work)
{
	struct async_ent *ent = container_of(work, struct async_ent, work);
	ktime_t calltime, delta, rettime;

	if (initcall_debug && system_state == SYSTEM_BOOTING) {
		printk("calling  %pF @ %i\n",
		       ent->func, task_pid_nr(current));
		calltime = ktime_get();
	}

	if (ent->ordered)
		flush_workqueue(async_wq);

	ent->func(ent->data);

	if (initcall_debug && system_state == SYSTEM_BOOTING) {
		rettime = ktime_get();
		delta = ktime_sub(rettime, calltime);
		printk("initcall %pF returned 0 after %lld usecs\n",
		       ent->func, (long long)ktime_to_ns(delta) >> 10);
	}
}

static bool __async_call(async_func_t func, void *data, bool ordered)
{
	struct async_ent *ent;

	ent = kzalloc(sizeof(struct async_ent), GFP_ATOMIC);
	if (!ent) {
		kfree(ent);
		if (ordered) {
			flush_workqueue(async_wq);
			flush_workqueue(async_ordered_wq);
		}
		func(data);
		return false;
	}

	ent->func = func;
	ent->data = data;
	ent->ordered = ordered;
	/*
	 * Use separate INIT_WORK for sync and async so that they end
	 * up with different lockdep keys.
	 */
	if (ordered) {
		INIT_WORK(&ent->work, async_work_func);
		queue_work(async_ordered_wq, &ent->work);
	} else {
		INIT_WORK(&ent->work, async_work_func);
		queue_work(async_wq, &ent->work);
	}
	return true;
}

/**
 * async_call - schedule a function for asynchronous execution
 * @func: function to execute asynchronously
 * @data: data pointer to pass to the function
 *
 * Schedule @func(@data) for asynchronous execution.  The function
 * might be called directly if memory allocation fails.
 *
 * CONTEXT:
 * Don't care but keep in mind that @func may be executed directly.
 *
 * RETURNS:
 * %true if async execution is scheduled, %false if executed locally.
 */
bool async_call(async_func_t func, void *data)
{
	return __async_call(func, data, false);
}
EXPORT_SYMBOL_GPL(async_call);

/**
 * async_call_ordered - schedule ordered asynchronous execution
 * @func: function to execute asynchronously
 * @data: data pointer to pass to the function
 *
 * Schedule @func(data) for ordered asynchronous excution.  It will be
 * executed only after all async functions scheduled upto this point
 * have finished.
 *
 * CONTEXT:
 * Might sleep.
 *
 * RETURNS:
 * %true if async execution is scheduled, %false if executed locally.
 */
bool async_call_ordered(async_func_t func, void *data)
{
	might_sleep();
	return __async_call(func, data, true);
}
EXPORT_SYMBOL_GPL(async_call_ordered);

/**
 * async_barrier - asynchronous execution barrier
 *
 * Wait till all currently scheduled async executions are finished.
 *
 * CONTEXT:
 * Might sleep.
 */
void async_barrier(void)
{
	ktime_t starttime, delta, endtime;

	if (initcall_debug && system_state == SYSTEM_BOOTING) {
		printk("async_waiting @ %i\n", task_pid_nr(current));
		starttime = ktime_get();
	}

	flush_workqueue(async_wq);
	flush_workqueue(async_ordered_wq);

	if (initcall_debug && system_state == SYSTEM_BOOTING) {
		endtime = ktime_get();
		delta = ktime_sub(endtime, starttime);
		printk("async_continuing @ %i after %lli usec\n",
		       task_pid_nr(current),
		       (long long)ktime_to_ns(delta) >> 10);
	}
}
EXPORT_SYMBOL_GPL(async_barrier);

static int __init init_async(void)
{
	async_wq = __create_workqueue("async", 0, WQ_MAX_ACTIVE);
	async_ordered_wq = create_singlethread_workqueue("async_ordered");
	BUG_ON(!async_wq || !async_ordered_wq);
	return 0;
}
core_initcall(init_async);
