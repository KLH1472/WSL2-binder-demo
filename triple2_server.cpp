#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/android/binder.h>
#include "binder_buf.h"

enum { CMD_REGISTER = 1, CMD_UPPER = 100, CMD_LOWER = 101 };
int main() {
    setvbuf(stdout, NULL, _IONBF, 0);

    int fd = open("/dev/binderfs/triple", O_RDWR|O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }
    void *map = mmap(NULL, 1024*1024, PROT_READ, MAP_PRIVATE, fd, 0);

    struct binder_write_read bwr; memset(&bwr, 0, sizeof(bwr));
    uint32_t cmd = BC_ENTER_LOOPER;
    bwr.write_size = sizeof(cmd); bwr.write_buffer = (uintptr_t)&cmd;
    ioctl(fd, BINDER_WRITE_READ, &bwr);

    // ---------- register with SM ----------
    BinderBuf bb;
    bb.write_string("UpperService");
    bb.write_binder((binder_uintptr_t)map);

    struct __attribute__((packed)) { uint32_t cmd; struct binder_transaction_data txn; } reg;
    reg.cmd = BC_TRANSACTION;
    memset(&reg.txn, 0, sizeof(reg.txn));
    reg.txn.target.handle = 0;
    reg.txn.code = CMD_REGISTER;
    reg.txn.data.ptr.buffer  = (binder_uintptr_t)bb.data;
    reg.txn.data_size        = bb.cursor;
    reg.txn.data.ptr.offsets = (binder_uintptr_t)bb.offsets;
    reg.txn.offsets_size     = bb.n_offsets * sizeof(binder_size_t);

    uint8_t rbuf[4096];
    memset(&bwr, 0, sizeof(bwr));
    bwr.write_size = sizeof(reg);
    bwr.write_buffer = (uintptr_t)&reg;
    bwr.read_size = sizeof(rbuf); bwr.read_buffer = (uintptr_t)rbuf;
    ioctl(fd, BINDER_WRITE_READ, &bwr);

    uint32_t my_handle = 0;
    uintptr_t p = (uintptr_t)rbuf, e = p + bwr.read_consumed;
    while (p < e) {
        uint32_t c = *(uint32_t*)p; p += 4;
        if (c == BR_REPLY) {
            struct binder_transaction_data *tr = (struct binder_transaction_data*)p;
            p += sizeof(*tr);
            my_handle = *(uint32_t*)(uintptr_t)tr->data.ptr.buffer;
        }
    }
    printf("[server] registered, my handle in SM = %u\n", my_handle);

    // ---------- main loop ----------
    while (1) {
        memset(&bwr, 0, sizeof(bwr));
        bwr.read_size = sizeof(rbuf); bwr.read_buffer = (uintptr_t)rbuf;
        if (ioctl(fd, BINDER_WRITE_READ, &bwr) < 0) { perror("ioctl"); break; }

        p = (uintptr_t)rbuf; e = p + bwr.read_consumed;
        while (p < e) {
            uint32_t c = *(uint32_t*)p; p += 4;
            if (c == BR_TRANSACTION) {
                struct binder_transaction_data *tr = (struct binder_transaction_data*)p;
                p += sizeof(*tr);
                const char *input = (const char*)(uintptr_t)tr->data.ptr.buffer;

                char result[256] = {0};
                int len = tr->data_size;
                if (len > 255) len = 255;
                memcpy(result, input, len);

                if (tr->code == CMD_UPPER) {
                    for (int i = 0; result[i]; i++) {
                        if (result[i]>='a'&&result[i]<='z') result[i]-=32;
                    }
                } else if (tr->code == CMD_LOWER) {
                    for (int i = 0; result[i]; i++) {
                        if (result[i]>='A'&&result[i]<='Z') result[i]+=32;
                    }
                }

                const char *opname = tr->code == CMD_UPPER ? "upper" :
                                     tr->code == CMD_LOWER ? "lower" : "unknown";
                printf("[server] %s '%s' -> '%s'\n", opname, input, result);

                struct __attribute__((packed)) { uint32_t cmd; struct binder_transaction_data txn; } rpl;
                rpl.cmd = BC_REPLY;
                memset(&rpl.txn, 0, sizeof(rpl.txn));
                rpl.txn.code = tr->code;
                rpl.txn.data.ptr.buffer = (binder_uintptr_t)result;
                rpl.txn.data_size = strlen(result) + 1;

                struct binder_write_read b2; memset(&b2, 0, sizeof(b2));
                b2.write_size = sizeof(rpl);
                b2.write_buffer = (uintptr_t)&rpl;
                ioctl(fd, BINDER_WRITE_READ, &b2);
            }
        }
    }
    close(fd); return 0;
}
