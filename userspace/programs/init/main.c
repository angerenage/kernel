#include <stdio.h>
#include <system/process.h>

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	printf("init: hello from userspace\n");

	struct self_info self;
	if (!process_self_info(&self)) {
		printf("init: process_self_info failed\n");
		return 1;
	}
	printf("init: pid=%llu thread_id=%llu thread_count=%llu\n",
	       (unsigned long long)self.pid,
	       (unsigned long long)self.thread_id,
	       (unsigned long long)self.thread_count);

	struct process_create_response child;
	if (!process_create("child", sizeof("child"), &child)) {
		printf("init: process_create failed\n");
		return 1;
	}
	printf("init: created child process_cap=%llu address_space_cap=%llu\n",
	       (unsigned long long)child.process_cap,
	       (unsigned long long)child.address_space_cap);

	struct process_info_response info;
	if (!process_get_info(child.process_cap, &info)) {
		printf("init: process_get_info failed\n");
		return 1;
	}
	printf("init: child pid=%llu thread_id=%llu thread_count=%llu\n",
	       (unsigned long long)info.pid,
	       (unsigned long long)info.thread_id,
	       (unsigned long long)info.thread_count);

	return 0;
}
