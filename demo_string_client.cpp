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

#define LOG(fmt, ...) do { \
    struct timeval _tv; gettimeofday(&_tv, NULL); \
    struct tm *_tm = localtime(&_tv.tv_sec); \
    printf("%02d:%02d:%02d.%06ld " fmt, \
        _tm->tm_hour, _tm->tm_min, _tm->tm_sec, _tv.tv_usec, \
        ##__VA_ARGS__); \
} while(0)

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: %s <string> <upper|lower>\n", argv[0]);
        return 1;
    }

    const char *input_str = argv[1];
    const char *mode_str = argv[2];

    enum { CMD_TO_UPPER = 100, CMD_TO_LOWER = 101 };
    uint32_t opcode;
    if (strcmp(mode_str, "upper") == 0)      opcode = CMD_TO_UPPER;
    else if (strcmp(mode_str, "lower") == 0) opcode = CMD_TO_LOWER;
    else { printf("mode must be 'upper' or 'lower'\n"); return 1; }

    // 1. open
    int fd = open("/dev/binderfs/simple", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }

    // 2. mmap — must have so driver can allocate reply buffer
    void *map = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    // 3. build BC_TRANSACTION with string payload
    struct __attribute__((packed)) {
        uint32_t cmd;
        struct binder_transaction_data txn;
    } write_data;

    write_data.cmd = BC_TRANSACTION;
    memset(&write_data.txn, 0, sizeof(write_data.txn));
    write_data.txn.target.handle = 0;
    write_data.txn.code = opcode;
    write_data.txn.data.ptr.buffer = (binder_uintptr_t)input_str;
    write_data.txn.data_size = strlen(input_str) + 1;

    // 4. first ioctl: write BC_TRANSACTION, read ACK
    uint8_t buf[4096];
    struct binder_write_read bwr;
    memset(&bwr, 0, sizeof(bwr));
    bwr.write_size = sizeof(uint32_t) + sizeof(struct binder_transaction_data);
    bwr.write_buffer = (uintptr_t)&write_data;
    bwr.read_size = sizeof(buf);
    bwr.read_buffer = (uintptr_t)buf;

    if (ioctl(fd, BINDER_WRITE_READ, &bwr) < 0) {
        perror("ioctl1"); return 1;
    }

    {
        uintptr_t p = (uintptr_t)buf;
        uintptr_t end = p + bwr.read_consumed;
        while (p < end) {
            uint32_t c = *(uint32_t*)p;
            p += sizeof(uint32_t);
            switch (c) {
            case BR_TRANSACTION_COMPLETE:
                LOG("[client] sent OK\n");
                break;
            case BR_FAILED_REPLY:
                LOG("[client] FAILED\n"); close(fd); return 1;
            case BR_DEAD_REPLY:
                LOG("[client] DEAD\n"); close(fd); return 1;
            default:
                break; // ignore BR_NOOP etc
            }
        }
    }

    // 5. second ioctl: wait for BR_REPLY
    memset(&bwr, 0, sizeof(bwr));
    bwr.read_size = sizeof(buf);
    bwr.read_buffer = (uintptr_t)buf;

    if (ioctl(fd, BINDER_WRITE_READ, &bwr) < 0) {
        perror("ioctl2"); return 1;
    }

    {
        uintptr_t p = (uintptr_t)buf;
        uintptr_t end = p + bwr.read_consumed;
        while (p < end) {
            uint32_t c = *(uint32_t*)p;
            p += sizeof(uint32_t);
            if (c == BR_REPLY) {
                struct binder_transaction_data *tr = (struct binder_transaction_data*)p;
                p += sizeof(*tr);
                const char *result = (const char *)(uintptr_t)tr->data.ptr.buffer;
                LOG("[client] got reply: '%s'\n", result);
            }
        }
    }

    close(fd);
    return 0;
}
