/*
 * Nitro
 *
 * frame.c - The frame object is the refcounted representation of a chunk
 *           of data sent from one nitro socket to another
 *
 *  -- LICENSE --
 *
 * Copyright 2013 Bump Technologies, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are
 * permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright notice, this list
 *       of conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY BUMP TECHNOLOGIES, INC. ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BUMP TECHNOLOGIES, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are those of the
 * authors and should not be interpreted as representing official policies, either expressed
 * or implied, of Bump Technologies, Inc.
 *
 */
#include "frame.h"
#include "cbuffer.h"

extern void nitro_frame_stack_pop(nitro_frame_t *f);
extern void nitro_frame_stack_push_sender(nitro_frame_t *f);
extern void *nitro_frame_data(nitro_frame_t *fr);
extern uint32_t nitro_frame_size(nitro_frame_t *fr);

static inline void nitro_frame_cleanup(void *fp, void *unused) {
    nitro_frame_t *f = (nitro_frame_t *)fp;
    nitro_counted_buffer_decref(f->buffer);

    if (f->ident_buffer) {
        nitro_counted_buffer_decref(f->ident_buffer);
    }

    if (f->sender_buffer) {
        nitro_counted_buffer_decref(f->sender_buffer);
    }

    free(f);
}

void nitro_frame_set_iovec(nitro_frame_t *f, struct iovec *vecs) {
    assert(f->iovec_set);
    memcpy(f->iovs, vecs, sizeof(struct iovec) * f->iovec_set);
}

nitro_frame_t *nitro_frame_copy_partial(nitro_frame_t *f, struct iovec *vecs) {
    assert(f->size < NITRO_MAX_FRAME);
    nitro_frame_t *result = malloc(sizeof(nitro_frame_t));
    memcpy(result, f, sizeof(nitro_frame_t));
    nitro_counted_buffer_incref(f->buffer);

    if (f->ident_buffer) {
        nitro_counted_buffer_incref(f->ident_buffer);
    }

    if (f->sender_buffer) {
        nitro_counted_buffer_incref(f->sender_buffer);
    }

    result->myref = nitro_counted_buffer_new(result, nitro_frame_cleanup, NULL);

    if (vecs) {
        nitro_frame_set_iovec(result, vecs);
    }

    return result;
}

nitro_frame_t *nitro_frame_new(void *data, uint32_t size, nitro_free_function ff, void *baton) {
    nitro_counted_buffer_t *buffer = nitro_counted_buffer_new(data, ff, baton);
    return nitro_frame_new_prealloc(data, size, buffer);
}

nitro_frame_t *nitro_frame_new_prealloc(void *data, uint32_t size, nitro_counted_buffer_t *buffer) {
    assert(size < NITRO_MAX_FRAME);
    nitro_frame_t *f;
    f = malloc(sizeof(nitro_frame_t));
    bzero(f, FRAME_BZERO_SIZE);
    f->buffer = buffer;
    f->size = size;
    f->data = data;
    f->myref = nitro_counted_buffer_new(f, nitro_frame_cleanup, NULL);
    return f;
}

void nitro_frame_set_stack(nitro_frame_t *f, const uint8_t *data,
                           nitro_counted_buffer_t *buf, uint8_t num) {
    f->ident_buffer = buf;
    nitro_counted_buffer_incref(buf);
    f->num_ident = num;
    f->ident_data = (uint8_t *)data;
}

void nitro_frame_clone_stack(nitro_frame_t *fr, nitro_frame_t *to) {
    if (fr->ident_buffer && (to->ident_buffer != fr->ident_buffer)) {
        if (to->ident_buffer) {
            nitro_counted_buffer_decref(to->ident_buffer);
        }

        to->num_ident = fr->num_ident;
        to->ident_data = fr->ident_data;
        to->ident_buffer = fr->ident_buffer;
        nitro_counted_buffer_incref(to->ident_buffer);
    }
}

void nitro_frame_extend_stack(nitro_frame_t *fr, nitro_frame_t *to) {
    if (to->ident_buffer) {
        nitro_counted_buffer_decref(to->ident_buffer);
    }

    to->num_ident = fr->num_ident + (fr->sender ? 1 : 0);
    to->ident_data = malloc(SOCKET_IDENT_LENGTH * to->num_ident);
    to->ident_buffer = nitro_counted_buffer_new(to->ident_data, just_free, NULL);

    if (fr->num_ident) {
        memcpy(to->ident_data, fr->ident_data, SOCKET_IDENT_LENGTH * fr->num_ident);
    }

    if (fr->sender) {
        memcpy(to->ident_data + (SOCKET_IDENT_LENGTH * fr->num_ident),
               fr->sender, SOCKET_IDENT_LENGTH);
    }
}

void nitro_frame_set_sender(nitro_frame_t *f,
                            uint8_t *sender, nitro_counted_buffer_t *buf) {
    if (f->sender_buffer) {
        nitro_counted_buffer_decref(f->sender_buffer);
    }

    f->sender_buffer = buf;
    f->sender = sender;

    if (buf) {
        nitro_counted_buffer_incref(buf);
    }
}

nitro_frame_t *nitro_frame_new_copy(void *data, uint32_t size) {
    char *n = malloc(size);
    memmove(n, data, size);
    return nitro_frame_new(n, size, just_free, NULL);
}

struct iovec *nitro_frame_iovs(nitro_frame_t *fr, int *num) {
    if (fr->iovec_set) {
        *num = fr->iovec_set;
        return (struct iovec *)fr->iovs;
    }

    //nitro_frame_make_tcp_header(fr);
    fr->tcp_header.protocol_version = 1;
    fr->tcp_header.packet_type = fr->type;

    fr->tcp_header.num_ident = fr->push_sender ?
                               fr->num_ident + 1 : fr->num_ident;
    fr->tcp_header.flags = 0;
    fr->tcp_header.frame_size = fr->size;

    fr->iovs[0].iov_base = (void *)&fr->tcp_header;
    fr->iovs[0].iov_len = sizeof(nitro_protocol_header);
    fr->iovs[1].iov_base = nitro_frame_data(fr);
    fr->iovs[1].iov_len = fr->size;

    if (fr->num_ident) {
        fr->iovs[2].iov_base = fr->ident_data;
        fr->iovs[2].iov_len = fr->num_ident * SOCKET_IDENT_LENGTH;

        if (fr->push_sender) {
            fr->iovs[3].iov_base = fr->sender;
            fr->iovs[3].iov_len = SOCKET_IDENT_LENGTH;
            fr->iovec_set = 4;
        } else {
            fr->iovec_set = 3;
            fr->iovs[3].iov_len = 0;
        }
    } else if (fr->push_sender) {
        fr->iovs[2].iov_base = fr->sender;
        fr->iovs[2].iov_len = SOCKET_IDENT_LENGTH;
        fr->iovec_set = 3;
        fr->iovs[3].iov_len = 0;
    } else {
        fr->iovec_set = 2;
        fr->iovs[2].iov_len = fr->iovs[3].iov_len = 0;
    }

    *num = fr->iovec_set;

    return (struct iovec *)fr->iovs;
}

int nitro_frame_iovs_advance(nitro_frame_t *fr,
                             struct iovec *vecs, int index, int offset, int *done) {
    int ret = -1;

    struct iovec *mv = &(vecs[index]);

    if (offset >= mv->iov_len) {
        ret = mv->iov_len;
        mv->iov_len = 0;
        mv->iov_base = NULL;
        *done = (index == fr->iovec_set - 1) ?
                1 : 0;
    } else {
        ret = offset;
        mv->iov_len -= offset;
        mv->iov_base = ((char *)mv->iov_base) + offset;
        *done = 0;
    }

    return ret;
}

void nitro_frame_iovs_reset(nitro_frame_t *fr) {
    fr->iovec_set = 0;
}

nitro_key_t *nitro_key_new(const uint8_t *data, uint8_t length,
                           nitro_counted_buffer_t *buf) {
    nitro_key_t *k;
    ZALLOC(k);
    nitro_counted_buffer_incref(buf);

    k->data = data;
    k->length = length;
    k->buf = buf;

    return k;
}

int nitro_key_compare(nitro_key_t *k1, nitro_key_t *k2) {
    if (k1->length < k2->length) {
        return -1;
    } else if (k1->length > k2->length) {
        return 1;
    } else {
        return memcmp(k1->data, k2->data, k1->length);
    }
}

void nitro_key_destroy(nitro_key_t *k) {
    nitro_counted_buffer_decref(k->buf);
    free(k);
}

void nitro_frame_clear(nitro_frame_t *fr) {
    nitro_counted_buffer_decref(fr->buffer);
    fr->data = NULL;
    fr->size = 0;
    fr->buffer = nitro_counted_buffer_new(NULL, free_nothing, NULL);
}
