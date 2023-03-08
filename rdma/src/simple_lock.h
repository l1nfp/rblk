
#ifndef SIMPLE_LOCK_H
#define SIMPLE_LOCK_H
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

void wait_kernel_event(int fd)
{
    lseek(fd, 0, 0);
}

void wake_up_kernel(int fd)
{
    write(fd, NULL, 0);
}

int read_swap_target(int fd)
{
    int ret;
    read(fd, &ret, sizeof(ret));
    return ret;
}

int get_lock_fd()
{
    return open("/proc/mydev", O_RDWR);
}
#endif /* SIMPLE_LOCK_H */
