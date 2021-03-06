/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)	"hyp_core_ctl: " fmt

#include <linux/init.h>
#include <linux/cpumask.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/slab.h>
#include <linux/cpuhotplug.h>

/**
 * struct hyp_core_ctl_data - The private data structure of this driver
 * @lock: spinlock to serialize task wakeup and enable/reserve_cpus
 * @task: task_struct pointer to the thread running the state machine
 * @pending: state machine work pending status
 * @reservation_enabled: status of the reservation
 *
 * @reserve_cpus: The CPUs to be reserved. input.
 * @our_isolated_cpus: The CPUs isolated by hyp_core_ctl driver. output.
 * @final_reserved_cpus: The CPUs reserved for the Hypervisor. output.
 *
 */
struct hyp_core_ctl_data {
	spinlock_t lock;
	struct task_struct *task;
	bool pending;
	bool reservation_enabled;
	cpumask_t reserve_cpus;
	cpumask_t our_isolated_cpus;
	cpumask_t final_reserved_cpus;
};

#define CREATE_TRACE_POINTS
#include <trace/events/hyp_core_ctl.h>

static struct hyp_core_ctl_data *the_hcd;

static inline void hyp_core_ctl_print_status(char *msg)
{
	trace_hyp_core_ctl_status(the_hcd, msg);

	pr_debug("%s: reserve=%*pbl reserved=%*pbl our_isolated=%*pbl online=%*pbl isolated=%*pbl\n",
		msg, cpumask_pr_args(&the_hcd->reserve_cpus),
		cpumask_pr_args(&the_hcd->final_reserved_cpus),
		cpumask_pr_args(&the_hcd->our_isolated_cpus),
		cpumask_pr_args(cpu_online_mask),
		cpumask_pr_args(cpu_isolated_mask));
}

static void hyp_core_ctl_undo_reservation(struct hyp_core_ctl_data *hcd)
{
	int cpu, ret;

	hyp_core_ctl_print_status("undo_reservation_start");

	for_each_cpu(cpu, &hcd->our_isolated_cpus) {
		ret = sched_unisolate_cpu(cpu);
		if (ret < 0) {
			pr_err("fail to un-isolate CPU%d. ret=%d\n", cpu, ret);
			continue;
		}
		cpumask_clear_cpu(cpu, &hcd->our_isolated_cpus);
	}

	hyp_core_ctl_print_status("undo_reservation_end");
}

static void finalize_reservation(struct hyp_core_ctl_data *hcd, cpumask_t *temp)
{
	if (cpumask_equal(temp, &hcd->final_reserved_cpus))
		return;

	cpumask_copy(&hcd->final_reserved_cpus, temp);
}

static void hyp_core_ctl_do_reservation(struct hyp_core_ctl_data *hcd)
{
	cpumask_t offline_cpus, iter_cpus, temp_reserved_cpus;
	int i, ret;

	cpumask_clear(&offline_cpus);
	cpumask_clear(&temp_reserved_cpus);

	hyp_core_ctl_print_status("reservation_start");

	/*
	 * Iterate all reserve CPUs and isolate them if not done already.
	 * The offline CPUs can't be isolated but they are considered
	 * reserved. When an offline and reserved CPU comes online, it
	 * will be isolated to honor the reservation.
	 */
	cpumask_andnot(&iter_cpus, &hcd->reserve_cpus, &hcd->our_isolated_cpus);

	for_each_cpu(i, &iter_cpus) {
		if (!cpu_online(i)) {
			cpumask_set_cpu(i, &offline_cpus);
			continue;
		}

		ret = sched_isolate_cpu(i);
		if (ret < 0) {
			pr_err("fail to isolate CPU%d. ret=%d\n", i, ret);
			continue;
		}
		cpumask_set_cpu(i, &hcd->our_isolated_cpus);
	}

	cpumask_or(&temp_reserved_cpus, &hcd->our_isolated_cpus, &offline_cpus);
	finalize_reservation(hcd, &temp_reserved_cpus);

	hyp_core_ctl_print_status("reservation_end");
}

static int hyp_core_ctl_thread(void *data)
{
	struct hyp_core_ctl_data *hcd = data;

	while (1) {
		spin_lock(&hcd->lock);
		if (!hcd->pending) {
			set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock(&hcd->lock);

			schedule();

			spin_lock(&hcd->lock);
			set_current_state(TASK_RUNNING);
		}
		hcd->pending = false;
		spin_unlock(&hcd->lock);

		if (kthread_should_stop())
			break;

		if (hcd->reservation_enabled)
			hyp_core_ctl_do_reservation(hcd);
		else
			hyp_core_ctl_undo_reservation(hcd);
	}

	return 0;
}

static int hyp_core_ctl_hp_offline(unsigned int cpu)
{
	if (!the_hcd || !the_hcd->reservation_enabled)
		return 0;

	/*
	 * A CPU can't be left in isolated state while it is
	 * going offline. So unisolate the CPU if it is
	 * isolated by us. An offline CPU is considered
	 * as reserved. So no further action is needed.
	 */
	if (cpumask_test_and_clear_cpu(cpu, &the_hcd->our_isolated_cpus))
		sched_unisolate_cpu_unlocked(cpu);

	return 0;
}

static int hyp_core_ctl_hp_online(unsigned int cpu)
{
	if (!the_hcd || !the_hcd->reservation_enabled)
		return 0;

	/*
	 * A reserved CPU is coming online. It should be isolated
	 * to honor the reservation. So kick the state machine.
	 */
	spin_lock(&the_hcd->lock);
	if (cpumask_test_cpu(cpu, &the_hcd->final_reserved_cpus)) {
		the_hcd->pending = true;
		wake_up_process(the_hcd->task);
	}
	spin_unlock(&the_hcd->lock);

	return 0;
}

static void hyp_core_ctl_enable(bool enable)
{
	spin_lock(&the_hcd->lock);
	if (enable == the_hcd->reservation_enabled)
		goto out;

	trace_hyp_core_ctl_enable(enable);
	pr_debug("reservation %s\n", enable ? "enabled" : "disabled");

	the_hcd->reservation_enabled = enable;
	the_hcd->pending = true;
	wake_up_process(the_hcd->task);
out:
	spin_unlock(&the_hcd->lock);
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return -EINVAL;

	hyp_core_ctl_enable(enable);

	return count;
}

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	if (!the_hcd)
		return -EPERM;

	return scnprintf(buf, PAGE_SIZE, "%u\n", the_hcd->reservation_enabled);
}

static DEVICE_ATTR_RW(enable);

static struct attribute *hyp_core_ctl_attrs[] = {
	&dev_attr_enable.attr,
	NULL
};

static struct attribute_group hyp_core_ctl_attr_group = {
	.attrs = hyp_core_ctl_attrs,
	.name = "hyp_core_ctl",
};

static int __init hyp_core_ctl_init(void)
{
	int ret;
	struct hyp_core_ctl_data *hcd;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };

	hcd = kzalloc(sizeof(*hcd), GFP_KERNEL);
	if (!hcd) {
		ret = -ENOMEM;
		goto out;
	}

	ret = cpulist_parse(CONFIG_QCOM_HYP_CORE_CTL_RESERVE_CPUS,
			    &hcd->reserve_cpus);
	if (ret < 0) {
		pr_err("Incorrect default reserve CPUs. ret=%d\n", ret);
		goto free_hcd;
	}

	spin_lock_init(&hcd->lock);
	hcd->task = kthread_run(hyp_core_ctl_thread, (void *) hcd,
				"hyp_core_ctl");

	if (IS_ERR(hcd->task)) {
		ret = PTR_ERR(hcd->task);
		goto free_hcd;
	}

	sched_setscheduler_nocheck(hcd->task, SCHED_FIFO, &param);

	ret = sysfs_create_group(&cpu_subsys.dev_root->kobj,
				 &hyp_core_ctl_attr_group);
	if (ret < 0) {
		pr_err("Fail to create sysfs files. ret=%d\n", ret);
		goto stop_task;
	}

	cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
				  "qcom/hyp_core_ctl:online",
				  hyp_core_ctl_hp_online, NULL);

	cpuhp_setup_state_nocalls(CPUHP_HYP_CORE_CTL_ISOLATION_DEAD,
				  "qcom/hyp_core_ctl:dead",
				  NULL, hyp_core_ctl_hp_offline);

	the_hcd = hcd;
	return 0;

stop_task:
	kthread_stop(hcd->task);
free_hcd:
	kfree(hcd);
out:
	return ret;
}
late_initcall(hyp_core_ctl_init);
