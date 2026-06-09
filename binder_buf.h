#pragma once
#include <cstdint>
#include <string.h>
#include <linux/android/binder.h>

// minimal binder buffer builder — eliminates manual alignment & offset tracking
struct BinderBuf {
    uint8_t   data[4096];
    uint64_t  offsets[64];
    int       n_offsets;
    int       cursor;     // write cursor in data[]

    BinderBuf() : n_offsets(0), cursor(0) { memset(data, 0, sizeof(data)); }

    void write_uint32(uint32_t v) {
        // uint32 is naturally 4-byte aligned, cursor always stays aligned
        memcpy(data + cursor, &v, 4);
        cursor += 4;
    }

    void write_string(const char *s) {
        int len = strlen(s) + 1;
        memcpy(data + cursor, s, len);
        cursor += len;
    }

    void write_binder(uint64_t ptr) {
        align8();
        struct flat_binder_object *fbo = (struct flat_binder_object*)(data + cursor);
        fbo->hdr.type = BINDER_TYPE_BINDER;
        fbo->flags    = 0;
        fbo->binder   = ptr;
        fbo->cookie   = 0;
        offsets[n_offsets++] = cursor;
        cursor += sizeof(struct flat_binder_object);
    }

    // write a handle that the kernel will translate for the receiver
    void write_translated_handle(uint32_t handle) {
        align8();
        struct flat_binder_object *fbo = (struct flat_binder_object*)(data + cursor);
        fbo->hdr.type = BINDER_TYPE_HANDLE;
        fbo->flags    = 0;
        fbo->handle   = handle;
        fbo->cookie   = 0;
        offsets[n_offsets++] = cursor;
        cursor += sizeof(struct flat_binder_object);
    }

private:
    void align8() { cursor = (cursor + 7) & ~7; }
};
