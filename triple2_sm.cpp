#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/android/binder.h>
#include <linux/android/binderfs.h>
#include "binder_buf.h"

#define DEV_NAME "triple"
#define MMAP_SIZE (1024*1024)

enum { CMD_REGISTER = 1, CMD_GET_SERVICE = 2 };

struct svc_entry {
    char name[64];
    uint32_t handle;
} g_services[8];
int g_nsvc = 0;

uint32_t find_handle(const char *name) {
    for (int i = 0; i < g_nsvc; i++)
        if (strcmp(g_services[i].name, name) == 0)
            return g_services[i].handle;
    return 0;
}

int main() {
    int ctl = open("/dev/binderfs/binder-control", O_RDWR|O_CLOEXEC);
    struct binderfs_device dev = {0};
    strcpy(dev.name, DEV_NAME);
    ioctl(ctl, BINDER_CTL_ADD, &dev);
    close(ctl);

    char path[64]; snprintf(path, sizeof(path), "/dev/binderfs/%s", DEV_NAME);
    int fd = open(path, O_RDWR|O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }

    void *map = mmap(NULL, MMAP_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    uint32_t max_t = 4; ioctl(fd, BINDER_SET_MAX_THREADS, &max_t);
    ioctl(fd, BINDER_SET_CONTEXT_MGR, 0);

    struct binder_write_read bwr; memset(&bwr, 0, sizeof(bwr));
    uint32_t cmd = BC_ENTER_LOOPER;
    bwr.write_size = sizeof(cmd); bwr.write_buffer = (uintptr_t)&cmd;
    ioctl(fd, BINDER_WRITE_READ, &bwr);
    printf("[sm] ready\n");

    while (1) {
        uint8_t buf[4096];
        memset(&bwr, 0, sizeof(bwr));
        bwr.read_size = sizeof(buf); bwr.read_buffer = (uintptr_t)buf;
        if (ioctl(fd, BINDER_WRITE_READ, &bwr) < 0) { perror("ioctl"); break; }

        uintptr_t p = (uintptr_t)buf, end = p + bwr.read_consumed;
        while (p < end) {
            uint32_t c = *(uint32_t*)p; p += 4;

            if (c == BR_TRANSACTION) {
                struct binder_transaction_data *tr = (struct binder_transaction_data*)p;
                p += sizeof(*tr);

                if (tr->code == CMD_REGISTER) {
                    const uint8_t *d = (const uint8_t*)(uintptr_t)tr->data.ptr.buffer;
                    const binder_size_t *offs = (const binder_size_t*)(uintptr_t)tr->data.ptr.offsets;
                    struct flat_binder_object *fbo = (struct flat_binder_object*)(d + *offs);

                    strcpy(g_services[g_nsvc].name, (const char*)d);
                    g_services[g_nsvc].handle = fbo->handle;
                    printf("[sm] registered '%s' at handle %u\n", d, fbo->handle);
                    g_nsvc++;

                    struct __attribute__((packed)) { uint32_t cmd; struct binder_transaction_data txn; uint32_t h; } r;
                    r.cmd = BC_REPLY;
                    memset(&r.txn, 0, sizeof(r.txn));
                    r.txn.code = CMD_REGISTER;
                    r.h = fbo->handle;
                    r.txn.data.ptr.buffer = (binder_uintptr_t)&r.h;
                    r.txn.data_size = sizeof(r.h);

                    struct binder_write_read b2; memset(&b2, 0, sizeof(b2));
                    b2.write_size = sizeof(uint32_t)+sizeof(struct binder_transaction_data);
                    b2.write_buffer = (uintptr_t)&r;
                    ioctl(fd, BINDER_WRITE_READ, &b2);
                    printf("[sm] replied handle=%u to server\n", fbo->handle);
                }
                else if (tr->code == CMD_GET_SERVICE) {
                    const char *name = (const char*)(uintptr_t)tr->data.ptr.buffer;
                    uint32_t h = find_handle(name);
                    printf("[sm] query '%s' -> handle %u\n", name, h);

                    if (h) {
                        BinderBuf bb;
                        bb.write_translated_handle(h);

                        struct __attribute__((packed)) { uint32_t cmd; struct binder_transaction_data txn; } rply;
                        rply.cmd = BC_REPLY;
                        memset(&rply.txn, 0, sizeof(rply.txn));
                        rply.txn.code = CMD_GET_SERVICE;
                        rply.txn.data.ptr.buffer = (binder_uintptr_t)bb.data;
                        rply.txn.data_size = bb.cursor;
                        rply.txn.data.ptr.offsets = (binder_uintptr_t)bb.offsets;
                        rply.txn.offsets_size = bb.n_offsets * sizeof(binder_size_t);

                        struct binder_write_read b2; memset(&b2, 0, sizeof(b2));
                        b2.write_size = sizeof(rply);
                        b2.write_buffer = (uintptr_t)&rply;
                        ioctl(fd, BINDER_WRITE_READ, &b2);
                    } else {
                        struct __attribute__((packed)) { uint32_t cmd; struct binder_transaction_data txn; } rply;
                        rply.cmd = BC_REPLY;
                        memset(&rply.txn, 0, sizeof(rply.txn));
                        rply.txn.code = 0;

                        struct binder_write_read b2; memset(&b2, 0, sizeof(b2));
                        b2.write_size = sizeof(rply);
                        b2.write_buffer = (uintptr_t)&rply;
                        ioctl(fd, BINDER_WRITE_READ, &b2);
                    }
                }
            }
        }
    }
    close(fd); return 0;
}
