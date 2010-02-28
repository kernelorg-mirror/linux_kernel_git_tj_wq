/* Copyright 2008, 2005 Rusty Russell rusty@rustcorp.com.au IBM Corporation.
 * GPL v2 and any later version.
 */
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/stop_machine.h>
#include <linux/syscalls.h>
#include <linux/interrupt.h>

#include <asm/atomic.h>
#include <asm/uaccess.h>

/* This controls the threads on each CPU. */
enum stopmachine_state {
	/* Dummy starting state for thread. */
	STOPMACHINE_NONE,
	/* Awaiting everyone to be scheduled. */
	STOPMACHINE_PREPARE,
	/* Disable interrupts. */
	STOPMACHINE_DISABLE_IRQ,
	/* Run the function */
	STOPMACHINE_RUN,
	/* Exit */
	STOPMACHINE_EXIT,
	/* Done */
	STOPMACHINE_DONE,
};
static enum stopmachine_state state;

struct stop_machine_data {
	int (*fn)(void *);
	void *data;
	int fnret;
};

/* Like num_online_cpus(), but hotplug cpu uses us, so we need this. */
static unsigned int num_threads;
static atomic_t thread_ack;
static DEFINE_MUTEX(lock);
/* setup_lock protects refcount, stop_machine_wq and stop_machine_work. */
static DEFINE_MUTEX(setup_lock);
/* Users of stop_machine. */
static int refcount;
static struct task_struct **stop_machine_threads;
static struct stop_machine_data active, idle;
static const struct cpumask *active_cpus;

static void set_state(enum stopmachine_state newstate)
{
	/* Reset ack counter. */
	atomic_set(&thread_ack, num_threads);
	smp_wmb();
	state = newstate;
}

/* Last one to ack a state moves to the next state. */
static void ack_state(void)
{
	if (atomic_dec_and_test(&thread_ack))
		set_state(state + 1);
}

/* This is the actual function which stops the CPU. It runs
 * on dedicated per-cpu kthreads. */
static int stop_cpu(void *unused)
{
	enum stopmachine_state curstate = STOPMACHINE_NONE;
	struct stop_machine_data *smdata;
	int cpu = smp_processor_id();
	int err;

repeat:
	/* Wait for __stop_machine() to initiate */
	while (true) {
		set_current_state(TASK_INTERRUPTIBLE);
		/* <- kthread_stop() and __stop_machine()::smp_wmb() */
		if (kthread_should_stop()) {
			__set_current_state(TASK_RUNNING);
			return 0;
		}
		if (state == STOPMACHINE_PREPARE)
			break;
		schedule();
	}
	smp_rmb();	/* <- __stop_machine()::set_state() */

	/* Okay, let's go */
	smdata = &idle;
	if (!active_cpus) {
		if (cpu == cpumask_first(cpu_online_mask))
			smdata = &active;
	} else {
		if (cpumask_test_cpu(cpu, active_cpus))
			smdata = &active;
	}
	/* Simple state machine */
	do {
		/* Chill out and ensure we re-read stopmachine_state. */
		cpu_relax();
		if (state != curstate) {
			curstate = state;
			switch (curstate) {
			case STOPMACHINE_DISABLE_IRQ:
				local_irq_disable();
				hard_irq_disable();
				break;
			case STOPMACHINE_RUN:
				/* On multiple CPUs only a single error code
				 * is needed to tell that something failed. */
				err = smdata->fn(smdata->data);
				if (err)
					smdata->fnret = err;
				break;
			default:
				break;
			}
			ack_state();
		}
	} while (curstate != STOPMACHINE_EXIT);

	local_irq_enable();
	goto repeat;
}

/* Callback for CPUs which aren't supposed to do anything. */
static int chill(void *unused)
{
	return 0;
}

static int create_stop_machine_thread(unsigned int cpu)
{
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };
	struct task_struct **pp = per_cpu_ptr(stop_machine_threads, cpu);
	struct task_struct *p;

	if (*pp)
		return -EBUSY;

	p = kthread_create(stop_cpu, NULL, "kstop/%u", cpu);
	if (IS_ERR(p))
		return PTR_ERR(p);

	sched_setscheduler_nocheck(p, SCHED_FIFO, &param);
	*pp = p;
	return 0;
}

/* Should be called with cpu hotplug disabled and setup_lock held */
static void kill_stop_machine_threads(void)
{
	unsigned int cpu;

	if (!stop_machine_threads)
		return;

	for_each_online_cpu(cpu) {
		struct task_struct *p = *per_cpu_ptr(stop_machine_threads, cpu);
		if (p)
			kthread_stop(p);
	}
	free_percpu(stop_machine_threads);
	stop_machine_threads = NULL;
}

int stop_machine_create(void)
{
	unsigned int cpu;

	get_online_cpus();
	mutex_lock(&setup_lock);
	if (refcount)
		goto done;

	stop_machine_threads = alloc_percpu(struct task_struct *);
	if (!stop_machine_threads)
		goto err_out;

	/*
	 * cpu hotplug is disabled, create only for online cpus,
	 * cpu_callback() will handle cpu hot [un]plugs.
	 */
	for_each_online_cpu(cpu) {
		if (create_stop_machine_thread(cpu))
			goto err_out;
		kthread_bind(*per_cpu_ptr(stop_machine_threads, cpu), cpu);
	}
done:
	refcount++;
	mutex_unlock(&setup_lock);
	put_online_cpus();
	return 0;

err_out:
	kill_stop_machine_threads();
	mutex_unlock(&setup_lock);
	put_online_cpus();
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(stop_machine_create);

void stop_machine_destroy(void)
{
	get_online_cpus();
	mutex_lock(&setup_lock);
	if (!--refcount)
		kill_stop_machine_threads();
	mutex_unlock(&setup_lock);
	put_online_cpus();
}
EXPORT_SYMBOL_GPL(stop_machine_destroy);

static int __cpuinit stop_machine_cpu_callback(struct notifier_block *nfb,
					       unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	struct task_struct **pp = per_cpu_ptr(stop_machine_threads, cpu);

	/* Hotplug exclusion is enough, no need to worry about setup_lock */
	if (!stop_machine_threads)
		return NOTIFY_OK;

	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_UP_PREPARE:
		if (create_stop_machine_thread(cpu)) {
			printk(KERN_ERR "failed to create stop machine "
			       "thread for %u\n", cpu);
			return NOTIFY_BAD;
		}
		break;

	case CPU_ONLINE:
		kthread_bind(*pp, cpu);
		break;

	case CPU_UP_CANCELED:
	case CPU_POST_DEAD:
		kthread_stop(*pp);
		*pp = NULL;
		break;
	}
	return NOTIFY_OK;
}

int __stop_machine(int (*fn)(void *), void *data, const struct cpumask *cpus)
{
	int i, ret;

	/* Set up initial state. */
	mutex_lock(&lock);
	num_threads = num_online_cpus();
	active_cpus = cpus;
	active.fn = fn;
	active.data = data;
	active.fnret = 0;
	idle.fn = chill;
	idle.data = NULL;

	set_state(STOPMACHINE_PREPARE);	/* -> stop_cpu()::smp_rmb() */
	smp_wmb();			/* -> stop_cpu()::set_current_state() */

	/* Schedule the stop_cpu work on all cpus: hold this CPU so one
	 * doesn't hit this CPU until we're ready. */
	get_cpu();
	for_each_online_cpu(i)
		wake_up_process(*per_cpu_ptr(stop_machine_threads, i));
	/* This will release the thread on our CPU. */
	put_cpu();
	while (state < STOPMACHINE_DONE)
		yield();
	ret = active.fnret;
	mutex_unlock(&lock);
	return ret;
}

int stop_machine(int (*fn)(void *), void *data, const struct cpumask *cpus)
{
	int ret;

	ret = stop_machine_create();
	if (ret)
		return ret;
	/* No CPUs can come up or down during this. */
	get_online_cpus();
	ret = __stop_machine(fn, data, cpus);
	put_online_cpus();
	stop_machine_destroy();
	return ret;
}
EXPORT_SYMBOL_GPL(stop_machine);

void __init init_stop_machine(void)
{
	hotcpu_notifier(stop_machine_cpu_callback, 0);
}
