#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "accelerationc.h"

#define N 4

int main(void)
{
	struct acc_motion h_shake = { 7000, 7000, 0, 9 };
	struct acc_motion v_shake = { 0, 0, 14000, 9 };
	struct acc_motion hv_shake = { 8000, 8000, 10000, 9 };
	int h_eid = syscall(379, &h_shake);
	int v_eid = syscall(379, &v_shake);
	int hv_eid = syscall(379, &hv_shake);
	int i, pid;

	for (i = 0; i < N; i++) {
		pid = fork();
		if (pid > 0)
			continue;
		else if (pid < 0)
			exit(128);
		else {
			pid = getpid();
			if (!syscall(380, h_eid))
				printf("%d detected a horizontal shake\n", pid);
			else
				printf("%d event destroyed\n", pid);
			return 0;
		}
	}
	for (i = 0; i < N; i++) {
		pid = fork();
		if (pid > 0)
			continue;
		else if (pid < 0)
			exit(128);
		else {
			pid = getpid();
			if (!syscall(380, v_eid))
				printf("%d detected a vertical shake\n", pid);
			else
				printf("%d event destroyed\n", pid);
			return 0;
		}
	}
	for (i = 0; i < N; i++) {
		pid = fork();
		if (pid > 0)
			continue;
		else if (pid < 0)
			exit(128);
		else {
			pid = getpid();
			if (!syscall(380, hv_eid))
				printf("%d detected a shake\n", pid);
			else
				printf("%d event destroyed\n", pid);
			return 0;
		}
	}
	usleep(60 * 1000000);
	printf("destroy event h_eid: %ld\n", syscall(382, h_eid));
	printf("destroy event v_eid: %ld\n", syscall(382, v_eid));
	printf("destroy event hv_eid: %ld\n", syscall(382, hv_eid));
	return 0;
}
