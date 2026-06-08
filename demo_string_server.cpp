#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstdint>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <ctime>
#include <linux/android/binder.h>
#include <linux/android/binderfs.h>

#define DEV_NAME "simple"
#define MMAP_SIZE (1024 * 1024)

#define LOG(fmt, ...) do { \
    struct timeval _tv; gettimeofday(&_tv, NULL); \
    struct tm *_tm = localtime(&_tv.tv_sec); \
    printf("%02d:%02d:%02d.%06ld " fmt, \
        _tm->tm_hour, _tm->tm_min, _tm->tm_sec, _tv.tv_usec, \
        ##__VA_ARGS__); \
} while(0)

int main() {
    int ctl = open("/dev/binderfs/binder-control", O_RDWR | O_CLOEXEC);
    if (ctl < 0) { perror("open binder-control"); return 1; }
    struct binderfs_device dev = {0};
    strcpy(dev.name, DEV_NAME);
    if (ioctl(ctl, BINDER_CTL_ADD, &dev) < 0 && errno != EEXIST) {
        perror("BINDER_CTL_ADD"); close(ctl); return 1;
    }
    close(ctl);

    char path[64];
    snprintf(path, sizeof(path), "/dev/binderfs/%s", DEV_NAME);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open device"); return 1; }

    void *map = mmap(NULL, MMAP_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    uint32_t max_threads = 4;
    ioctl(fd, BINDER_SET_MAX_THREADS, &max_threads);

    if (ioctl(fd, BINDER_SET_CONTEXT_MGR, 0) < 0) {
        perror("SET_CONTEXT_MGR"); return 1;
    }

    struct binder_write_read bwr;
    memset(&bwr, 0, sizeof(bwr));
    uint32_t cmd = BC_ENTER_LOOPER;
    bwr.write_size = sizeof(cmd);
    bwr.write_buffer = (uintptr_t)&cmd;
    ioctl(fd, BINDER_WRITE_READ, &bwr);

    LOG("[server] ready\n");

    while (1) {
        uint8_t buf[4096];
        memset(&bwr, 0, sizeof(bwr));
        bwr.read_size = sizeof(buf);
        bwr.read_buffer = (uintptr_t)buf;

        if (ioctl(fd, BINDER_WRITE_READ, &bwr) < 0) {
            perror("ioctl"); break;
        }

        uintptr_t p = (uintptr_t)buf;
        uintptr_t end = p + bwr.read_consumed;
        while (p < end) {
            uint32_t c = *(uint32_t*)p;
            p += sizeof(uint32_t);

            if (c == BR_TRANSACTION) {
                struct binder_transaction_data *tr = (struct binder_transaction_data*)p;
                p += sizeof(*tr);

                enum { CMD_TO_UPPER = 100, CMD_TO_LOWER = 101 };

                const char *input = (const char *)(uintptr_t)tr->data.ptr.buffer;
                int len = tr->data_size;

                LOG("[server] got code=%u, str='%s' (len=%d)\n",
                       tr->code, input, len);

                char result[256];
                memset(result, 0, sizeof(result));
                strncpy(result, input, sizeof(result) - 1);

                if (tr->code == CMD_TO_UPPER) {
                    for (int i = 0; result[i]; i++)
                        if (result[i] >= 'a' && result[i] <= 'z')
                            result[i] -= 32;
                } else if (tr->code == CMD_TO_LOWER) {
                    for (int i = 0; result[i]; i++)
                        if (result[i] >= 'A' && result[i] <= 'Z')
                            result[i] += 32;
                }

                struct __attribute__((packed)) {
                    uint32_t cmd;
                    struct binder_transaction_data txn;
                } reply;
                reply.cmd = BC_REPLY;
                memset(&reply.txn, 0, sizeof(reply.txn));
                reply.txn.code = tr->code;
                reply.txn.data.ptr.buffer = (binder_uintptr_t)result;
                reply.txn.data_size = strlen(result) + 1;

                struct binder_write_read bwr2;
                memset(&bwr2, 0, sizeof(bwr2));
                bwr2.write_size = sizeof(uint32_t) + sizeof(struct binder_transaction_data);
                bwr2.write_buffer = (uintptr_t)&reply;
                ioctl(fd, BINDER_WRITE_READ, &bwr2);

                LOG("[server] replied '%s'\n", result);
            }
        }
    }

    close(fd);
    return 0;
}
