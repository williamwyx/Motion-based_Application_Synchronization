#ifndef _ACCELERATION_H
#define _ACCELERATION_H

#define NOISE 500
#define WINDOW 20

struct dev_acceleration {
	int x;
	int y;
	int z;
};

struct acc_motion {
	unsigned int dlt_x;
	unsigned int dlt_y;
	unsigned int dlt_z;
	unsigned int frq;
};

struct event_t {
	int id;
	struct acc_motion baseline;
	wait_queue_head_t *wq;
	struct list_head siblings;
	int pcnt;
	int wk_flag;
};

#endif
