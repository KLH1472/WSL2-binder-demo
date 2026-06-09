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

enum { CMD_GET_SERVICE = 2, CMD_UPPER = 100, CMD_LOWER = 101 };

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: %s <string> <upper|lower>\n", argv[0]); return 1; }
    const char *input = argv[1];
    uint32_t op = (strcmp(argv[2],"upper")==0) ? CMD_UPPER : CMD_LOWER;

    int fd = open("/dev/binderfs/triple", O_RDWR|O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }
    void *map = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);

    // ---------- step 1: query SM ----------
    BinderBuf qb;
    qb.write_string("UpperService");

    struct __attribute__((packed)) { uint32_t cmd; struct binder_transaction_data txn; } q;
    q.cmd = BC_TRANSACTION;
    memset(&q.txn, 0, sizeof(q.txn));
    q.txn.target.handle = 0;
    q.txn.code = CMD_GET_SERVICE;
    q.txn.data.ptr.buffer = (binder_uintptr_t)qb.data;
    q.txn.data_size = qb.cursor;

    struct binder_write_read bwr; memset(&bwr,0,sizeof(bwr));
    bwr.write_size = sizeof(q); bwr.write_buffer = (uintptr_t)&q;
    ioctl(fd, BINDER_WRITE_READ, &bwr);

    uint32_t srv_handle = 0;

    uint8_t buf[4096];
    memset(&bwr, 0, sizeof(bwr));
    bwr.read_size = sizeof(buf); bwr.read_buffer = (uintptr_t)buf;
    ioctl(fd, BINDER_WRITE_READ, &bwr);

    uintptr_t p = (uintptr_t)buf, e = p + bwr.read_consumed;
    while (p < e) {
        uint32_t c = *(uint32_t*)p; p += 4;
        if (c == BR_REPLY) {
            struct binder_transaction_data *t = (struct binder_transaction_data*)p;
            p += sizeof(*t);
            if (t->code == CMD_GET_SERVICE && t->data_size > 0 && t->offsets_size > 0) {
                const uint8_t *data = (const uint8_t*)(uintptr_t)t->data.ptr.buffer;
                const binder_size_t *offs = (const binder_size_t*)(uintptr_t)t->data.ptr.offsets;
                struct flat_binder_object *fbo = (struct flat_binder_object*)(data + *offs);
                srv_handle = fbo->handle;
            }
        }
    }

    if (!srv_handle) { printf("[client] service not found\n"); close(fd); return 1; }
    printf("[client] found 'UpperService' at handle %u\n", srv_handle);

    // ---------- step 2: direct call to server ----------
    BinderBuf rb;
    rb.write_string(input);

    struct __attribute__((packed)) { uint32_t cmd; struct binder_transaction_data txn; } req;
    req.cmd = BC_TRANSACTION;
    memset(&req.txn, 0, sizeof(req.txn));
    req.txn.target.handle = srv_handle;
    req.txn.code = op;
    req.txn.data.ptr.buffer = (binder_uintptr_t)rb.data;
    req.txn.data_size = rb.cursor;

    memset(&bwr, 0, sizeof(bwr));
    bwr.write_size = sizeof(req); bwr.write_buffer = (uintptr_t)&req;
    bwr.read_size = sizeof(buf); bwr.read_buffer = (uintptr_t)buf;
    ioctl(fd, BINDER_WRITE_READ, &bwr);

    char result[256] = "";
    bool have_result = false;

    // process first read buffer
    {
        uintptr_t p = (uintptr_t)buf, e = p + bwr.read_consumed;
        while (p < e) {
            uint32_t c = *(uint32_t*)p; p += 4;
            if (c == BR_REPLY) {
                struct binder_transaction_data *t = (struct binder_transaction_data*)p;
                p += sizeof(*t);
                strcpy(result, (const char*)(uintptr_t)t->data.ptr.buffer);
                have_result = true;
            } else if (c == BR_DEAD_REPLY || c == BR_FAILED_REPLY) {
                have_result = true;
            }
        }
    }

    // wait for actual reply if only ack was received
    if (!have_result) {
        memset(&bwr, 0, sizeof(bwr));
        bwr.read_size = sizeof(buf); bwr.read_buffer = (uintptr_t)buf;
        ioctl(fd, BINDER_WRITE_READ, &bwr);

        uintptr_t p = (uintptr_t)buf, e = p + bwr.read_consumed;
        while (p < e) {
            uint32_t c = *(uint32_t*)p; p += 4;
            if (c == BR_REPLY) {
                struct binder_transaction_data *t = (struct binder_transaction_data*)p;
                p += sizeof(*t);
                strcpy(result, (const char*)(uintptr_t)t->data.ptr.buffer);
            }
        }
    }

    printf("[client] result: '%s'\n", result);

    close(fd); return 0;
}
