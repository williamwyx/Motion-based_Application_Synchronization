#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/cred.h>
#include <linux/acceleration.h>
#include <linux/uaccess.h>

#if 1
#define dbg printk
#else
#define dbg
#endif

static int this_event_id;
static struct event_t *baseline_events;
static DEFINE_MUTEX(rwmutex);

/* System call 378 */
SYSCALL_DEFINE1(set_acceleration, struct dev_acceleration __user *, acc)
{
	static struct dev_acceleration raw_acc_static;

	if (current_cred()->uid != 0)
		return -EPERM;
	if (copy_from_user(&raw_acc_static,
			   acc, sizeof(struct dev_acceleration)))
		return -EFAULT;
	dbg("Acceleration: x=%d, y=%d, z=%d\n", raw_acc_static.x,
	    raw_acc_static.y, raw_acc_static.z);
	return 0;
}

/* System call 379 */
SYSCALL_DEFINE1(accevt_create, struct acc_motion __user *, acc)
{
	struct event_t *ev = kmalloc(sizeof(struct event_t), GFP_KERNEL);
	struct event_t *iter;

	dbg("create()\n");
	if (!ev)
		return -ENOMEM;
	/* ev will be filled up and inserted into baseline_events */
	memset(ev, 0, sizeof(struct event_t));

	if (copy_from_user(&ev->baseline, acc, sizeof(struct acc_motion))) {
		kfree(ev);
		return -EFAULT;
	}
	if (mutex_lock_interruptible(&rwmutex) < 0) {
		kfree(ev);
		return -ECANCELED;
	}
	ev->id = this_event_id++;
	/* cap frq at WINDOW */
	if (ev->baseline.frq > WINDOW)
		ev->baseline.frq = WINDOW;
	/* initialize the wq head and chain in elements in create */
	ev->wq = kmalloc(sizeof(wait_queue_head_t), GFP_KERNEL);
	if (!ev->wq) {
		kfree(ev);
		mutex_unlock(&rwmutex);
		return -ENOMEM;
	}
	init_waitqueue_head(ev->wq);
	dbg("adding event to baseline_events\n");
	/* create baseline_events list head if it's the first time */
	if (!baseline_events) {
		dbg("creating baseline events\n");
		/* baseline_events is freed in destroy */
		baseline_events = kmalloc(sizeof(struct event_t), GFP_KERNEL);
		if (!baseline_events) {
			kfree(ev);
			mutex_unlock(&rwmutex);
			return -ENOMEM;
		}
		INIT_LIST_HEAD(&baseline_events->siblings);
	}
	list_add(&ev->siblings, &baseline_events->siblings);
	/* test printing */
	dbg("Current events:\n");
	list_for_each_entry(iter, &baseline_events->siblings, siblings)
		dbg("%d\t", iter->id);
	dbg("\n");
	mutex_unlock(&rwmutex);
	return ev->id;
}

static struct event_t *search_event_id(int event_id)
{
	struct event_t *iter;
	int found = 0;

	if (event_id < 0)
		return NULL;
	dbg("search_event_id()\n");
	list_for_each_entry(iter, &baseline_events->siblings, siblings)
		if (event_id == iter->id) {
			found = 1;
			break;
		}
	return !found ? NULL : iter;
}

/* System call 380 */
SYSCALL_DEFINE1(accevt_wait, int, event_id)
{
	struct event_t *iter;

	dbg("wait()\n");
	if (mutex_lock_interruptible(&rwmutex) < 0)
		return -ECANCELED;
	/* insert process into the correct event in baseline_events */
	iter = search_event_id(event_id);
	if (!iter) {
		mutex_unlock(&rwmutex);
		return -EINVAL;
	}
	/* insert current into wait queue */
	dbg("preparing to insert into wq\n");
	iter->pcnt++;
	mutex_unlock(&rwmutex);
	wait_event_interruptible(*(iter->wq), iter->wk_flag);
	if (mutex_lock_interruptible(&rwmutex) < 0)
		return -ECANCELED;
	if (iter->wk_flag == 1) {
		dbg("wake all was called\n");
		if (!--iter->pcnt)
			iter->wk_flag = 0;
	} else if (iter->wk_flag == 2) {
		dbg("destroy was called\n");
		if (!--iter->pcnt) {
			list_del(&iter->siblings);
			kfree(iter->wq);
			kfree(iter);
		}
		mutex_unlock(&rwmutex);
		return -EINVAL;
	}
	mutex_unlock(&rwmutex);
	return 0;
}

static int baseline_trigger(struct acc_motion *trigger)
{
	struct event_t *iter;

	dbg("baseline_trigger()\n");
	if (!baseline_events)
		return 0;

	/* iterate through all baseline_events for trigger */
	if (mutex_lock_interruptible(&rwmutex) < 0)
		return -ECANCELED;
	list_for_each_entry(iter, &baseline_events->siblings, siblings) {
		struct acc_motion *bline = &iter->baseline;

		dbg("trigger: %d, %d, %d, %d ?> %d, %d, %d, %d\n",
		    trigger->dlt_x, trigger->dlt_y, trigger->dlt_z,
		    trigger->frq, bline->dlt_x, bline->dlt_y, bline->dlt_z,
		    bline->frq);
		if (!iter->wq)
			dbg("iter->wq is NULL for id: %d\n", iter->id);
		if (trigger->dlt_x > bline->dlt_x &&
		    trigger->dlt_y > bline->dlt_y &&
		    trigger->dlt_z > bline->dlt_z &&
		    trigger->frq > bline->frq &&
		    iter->wq) {
			dbg("waking up all in event %d\n", iter->id);
			iter->wk_flag = 1;
			wake_up_all(iter->wq);
		}
	}
	mutex_unlock(&rwmutex);
	return 0;
}

/* System call 381 */
SYSCALL_DEFINE1(accevt_signal, struct dev_acceleration __user *, acc)
{
	static struct acc_motion dlt_acc[WINDOW];
	static struct dev_acceleration last_acc;
	static int dlt_acc_nr, dlt_acc_full, first_flag = 1;
	struct dev_acceleration new_acc;
	static DEFINE_MUTEX(dltmutex);
	int i = 0;

	if (current_cred()->uid != 0)
		return -EPERM;

	/* at the first time, only store the user acc */
	if (first_flag) {
		if (copy_from_user(&last_acc, acc,
				   sizeof(struct dev_acceleration)))
			return -EFAULT;
		first_flag = 0;
		return 0;
	}

	/* store the diff between two accs */
	if (copy_from_user(&new_acc, acc, sizeof(struct dev_acceleration)))
		return -EFAULT;
	if (mutex_lock_interruptible(&dltmutex) < 0)
		return -ECANCELED;
	dlt_acc[dlt_acc_nr].dlt_x = abs(new_acc.x - last_acc.x);
	dlt_acc[dlt_acc_nr].dlt_y = abs(new_acc.y - last_acc.y);
	dlt_acc[dlt_acc_nr].dlt_z = abs(new_acc.z - last_acc.z);

	/* replace last_acc with new_acc */
	last_acc = new_acc;

	/* wait for dlt_acc to fill up for the first time */
	if (dlt_acc_nr == WINDOW - 1)
		dlt_acc_full = 1;
	if (dlt_acc_full) {
		struct acc_motion this_trigger = { 0 };

		dbg("dlt_acc_full\n");
		for (i = 0; i < WINDOW; i++) {
			if (dlt_acc[i].dlt_x + dlt_acc[i].dlt_y +
			    dlt_acc[i].dlt_z > NOISE) {
				this_trigger.dlt_x += dlt_acc[i].dlt_x;
				this_trigger.dlt_y += dlt_acc[i].dlt_y;
				this_trigger.dlt_z += dlt_acc[i].dlt_z;
				this_trigger.frq++;
			}
		}
		dbg("Trigger: %d, %d, %d, %d\n", this_trigger.dlt_x,
		       this_trigger.dlt_y, this_trigger.dlt_z,
		       this_trigger.frq);
		if (baseline_trigger(&this_trigger) < 0)
			return -ECANCELED;
	}
	dlt_acc_nr = (dlt_acc_nr + 1) % WINDOW;
	mutex_unlock(&dltmutex);
	return 0;
}

/* System call 382 */
SYSCALL_DEFINE1(accevt_destroy, int, event_id)
{
	struct event_t *iter;

	dbg("destroy()\n");
	if (mutex_lock_interruptible(&rwmutex) < 0)
		return -ECANCELED;
	if (!baseline_events) {
		mutex_unlock(&rwmutex);
		return -EINVAL;
	}
	iter = search_event_id(event_id);
	if (!iter) {
		mutex_unlock(&rwmutex);
		return -EINVAL;
	}
	if (!iter->wq) {
		dbg("destroy: no wq");
		list_del(&iter->siblings);
		kfree(iter);
	} else if (!waitqueue_active(iter->wq)) {
		dbg("destroy: wq inactive");
		list_del(&iter->siblings);
		kfree(iter->wq);
		kfree(iter);
	} else {
		dbg("destroy: wake up all");
		/* wait will destroy the event */
		iter->wk_flag = 2;
		wake_up_all(iter->wq);
	}
	/* test printing */
	dbg("Current events:\n");
	list_for_each_entry(iter, &baseline_events->siblings, siblings)
		dbg("%d\t", iter->id);
	dbg("\n");
	if (list_empty(&baseline_events->siblings)) {
		dbg("freeing baseline_events\n");
		kfree(baseline_events);
		baseline_events = NULL;
	}
	mutex_unlock(&rwmutex);
	return 0;
}
