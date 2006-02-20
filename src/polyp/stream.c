/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <string.h>

#include <polyp/def.h>
#include <polypcore/xmalloc.h>
#include <polypcore/pstream-util.h>
#include <polypcore/util.h>
#include <polypcore/log.h>

#include "internal.h"

#define LATENCY_IPOL_INTERVAL_USEC (10000L)

pa_stream *pa_stream_new(pa_context *c, const char *name, const pa_sample_spec *ss, const pa_channel_map *map) {
    pa_stream *s;
    
    assert(c);

    PA_CHECK_VALIDITY_RETURN_NULL(c, ss && pa_sample_spec_valid(ss), PA_ERR_INVALID);
    PA_CHECK_VALIDITY_RETURN_NULL(c, !map || (pa_channel_map_valid(map) && map->channels == ss->channels), PA_ERR_INVALID);

    s = pa_xnew(pa_stream, 1);
    s->ref = 1;
    s->context = c;
    s->mainloop = c->mainloop;

    s->read_callback = NULL;
    s->read_userdata = NULL;
    s->write_callback = NULL;
    s->write_userdata = NULL;
    s->state_callback = NULL;
    s->state_userdata = NULL;
    s->overflow_callback = NULL;
    s->overflow_userdata = NULL;
    s->underflow_callback = NULL;
    s->underflow_userdata = NULL;

    s->direction = PA_STREAM_NODIRECTION;
    s->name = pa_xstrdup(name);
    s->sample_spec = *ss;

    if (map)
        s->channel_map = *map;
    else
        pa_channel_map_init_auto(&s->channel_map, ss->channels);
    
    s->channel = 0;
    s->channel_valid = 0;
    s->syncid = c->csyncid++;
    s->device_index = PA_INVALID_INDEX;
    s->requested_bytes = 0;
    s->state = PA_STREAM_UNCONNECTED;
    memset(&s->buffer_attr, 0, sizeof(s->buffer_attr));

    s->peek_memchunk.index = 0;
    s->peek_memchunk.length = 0;
    s->peek_memchunk.memblock = NULL;

    s->record_memblockq = NULL;

    s->counter = 0;
    s->previous_time = 0;
    s->previous_ipol_time = 0;

    s->corked = 0;
    s->interpolate = 0;

    s->ipol_usec = 0;
    memset(&s->ipol_timestamp, 0, sizeof(s->ipol_timestamp));
    s->ipol_event = NULL;
    s->ipol_requested = 0;

    PA_LLIST_PREPEND(pa_stream, c->streams, s);

    return pa_stream_ref(s);
}

static void stream_free(pa_stream *s) {
    assert(s);

    if (s->ipol_event) {
        assert(s->mainloop);
        s->mainloop->time_free(s->ipol_event);
    }

    if (s->peek_memchunk.memblock)
        pa_memblock_unref(s->peek_memchunk.memblock);

    if (s->record_memblockq)
        pa_memblockq_free(s->record_memblockq);

    pa_xfree(s->name);
    pa_xfree(s);
}

void pa_stream_unref(pa_stream *s) {
    assert(s);
    assert(s->ref >= 1);

    if (--(s->ref) == 0)
        stream_free(s);
}

pa_stream* pa_stream_ref(pa_stream *s) {
    assert(s);
    assert(s->ref >= 1);
    
    s->ref++;
    return s;
}

pa_stream_state_t pa_stream_get_state(pa_stream *s) {
    assert(s);
    assert(s->ref >= 1);
    
    return s->state;
}

pa_context* pa_stream_get_context(pa_stream *s) {
    assert(s);
    assert(s->ref >= 1);
    
    return s->context;
}

uint32_t pa_stream_get_index(pa_stream *s) {
    assert(s);
    assert(s->ref >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, PA_INVALID_INDEX);
    
    return s->device_index;
}
    
void pa_stream_set_state(pa_stream *s, pa_stream_state_t st) {
    assert(s);
    assert(s->ref >= 1);

    if (s->state == st)
        return;
    
    pa_stream_ref(s);

    s->state = st;
    
    if ((st == PA_STREAM_FAILED || st == PA_STREAM_TERMINATED) && s->context) {
        /* Detach from context */
        
        if (s->channel_valid)
            pa_dynarray_put((s->direction == PA_STREAM_PLAYBACK) ? s->context->playback_streams : s->context->record_streams, s->channel, NULL);

        PA_LLIST_REMOVE(pa_stream, s->context->streams, s);
        pa_stream_unref(s);
    }

    if (s->state_callback)
        s->state_callback(s, s->state_userdata);

    pa_stream_unref(s);
}

void pa_command_stream_killed(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_context *c = userdata;
    pa_stream *s;
    uint32_t channel;
    
    assert(pd);
    assert(command == PA_COMMAND_PLAYBACK_STREAM_KILLED || command == PA_COMMAND_RECORD_STREAM_KILLED);
    assert(t);
    assert(c);

    pa_context_ref(c);
    
    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }
    
    if (!(s = pa_dynarray_get(command == PA_COMMAND_PLAYBACK_STREAM_KILLED ? c->playback_streams : c->record_streams, channel)))
        goto finish;

    pa_context_set_error(c, PA_ERR_KILLED);
    pa_stream_set_state(s, PA_STREAM_FAILED);

finish:
    pa_context_unref(c);
}

void pa_command_request(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_stream *s;
    pa_context *c = userdata;
    uint32_t bytes, channel;
    
    assert(pd);
    assert(command == PA_COMMAND_REQUEST);
    assert(t);
    assert(c);

    pa_context_ref(c);
    
    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        pa_tagstruct_getu32(t, &bytes) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }
    
    if (!(s = pa_dynarray_get(c->playback_streams, channel)))
        goto finish;

    if (s->state == PA_STREAM_READY) {
        s->requested_bytes += bytes;
        
        if (s->requested_bytes > 0 && s->write_callback)
            s->write_callback(s, s->requested_bytes, s->write_userdata);
    }

finish:
    pa_context_unref(c);
}

void pa_command_overflow_or_underflow(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_stream *s;
    pa_context *c = userdata;
    uint32_t channel;

    assert(pd);
    assert(command == PA_COMMAND_OVERFLOW || command == PA_COMMAND_UNDERFLOW);
    assert(t);
    assert(c);

    pa_context_ref(c);
    
    if (pa_tagstruct_getu32(t, &channel) < 0 ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(c, PA_ERR_PROTOCOL);
        goto finish;
    }
    
    if (!(s = pa_dynarray_get(c->playback_streams, channel)))
        goto finish;

    if (s->state == PA_STREAM_READY) {

        if (command == PA_COMMAND_OVERFLOW) {
            if (s->overflow_callback)
                s->overflow_callback(s, s->overflow_userdata);
        } else if (command == PA_COMMAND_UNDERFLOW) {
            if (s->underflow_callback)
                s->underflow_callback(s, s->underflow_userdata);
        }
    }

finish:
    pa_context_unref(c);
}

static void ipol_callback(pa_mainloop_api *m, pa_time_event *e, PA_GCC_UNUSED const struct timeval *tv, void *userdata) {
    struct timeval next;
    pa_stream *s = userdata;

    pa_stream_ref(s);

/*     pa_log("requesting new ipol data\n"); */
    
    if (s->state == PA_STREAM_READY && !s->ipol_requested) {
        pa_operation_unref(pa_stream_get_latency_info(s, NULL, NULL));
        s->ipol_requested = 1;
    }
    
    pa_gettimeofday(&next);
    pa_timeval_add(&next, LATENCY_IPOL_INTERVAL_USEC);
    m->time_restart(e, &next);
    
    pa_stream_unref(s);
}


void pa_create_stream_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_stream *s = userdata;
    
    assert(pd);
    assert(t);
    assert(s);
    assert(s->state == PA_STREAM_CREATING);
        
    pa_stream_ref(s);
    
    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(s->context, command, t) < 0)
            goto finish;
        
        pa_stream_set_state(s, PA_STREAM_FAILED);
        goto finish;
    }

    if (pa_tagstruct_getu32(t, &s->channel) < 0 ||
        ((s->direction != PA_STREAM_UPLOAD) && pa_tagstruct_getu32(t, &s->device_index) < 0) ||
        ((s->direction != PA_STREAM_RECORD) && pa_tagstruct_getu32(t, &s->requested_bytes) < 0) ||
        !pa_tagstruct_eof(t)) {
        pa_context_fail(s->context, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (s->direction == PA_STREAM_RECORD) {
        assert(!s->record_memblockq);
        
        s->record_memblockq = pa_memblockq_new(
                0, 
                s->buffer_attr.maxlength,
                0,
                pa_frame_size(&s->sample_spec),
                1,
                0,
                NULL,
                s->context->memblock_stat);
    }

    s->channel_valid = 1;
    pa_dynarray_put((s->direction == PA_STREAM_RECORD) ? s->context->record_streams : s->context->playback_streams, s->channel, s);

    if (s->interpolate) {
        struct timeval tv;
        pa_operation_unref(pa_stream_get_latency_info(s, NULL, NULL));

        pa_gettimeofday(&tv);
        tv.tv_usec += LATENCY_IPOL_INTERVAL_USEC; /* every 100 ms */

        assert(!s->ipol_event);
        s->ipol_event = s->mainloop->time_new(s->mainloop, &tv, &ipol_callback, s);
    }

    pa_stream_set_state(s, PA_STREAM_READY);
    
    if (s->requested_bytes > 0 && s->ref > 1 && s->write_callback)
        s->write_callback(s, s->requested_bytes, s->write_userdata);

finish:
    pa_stream_unref(s);
}

static int create_stream(
        pa_stream_direction_t direction,
        pa_stream *s,
        const char *dev,
        const pa_buffer_attr *attr,
        pa_stream_flags_t flags,
        const pa_cvolume *volume,
        pa_stream *sync_stream) {
    
    pa_tagstruct *t;
    uint32_t tag;
    
    assert(s);
    assert(s->ref >= 1);
    
    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_UNCONNECTED, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, !(flags & ~(PA_STREAM_START_CORKED|PA_STREAM_INTERPOLATE_LATENCY)), PA_ERR_INVALID);
    PA_CHECK_VALIDITY(s->context, direction == PA_STREAM_PLAYBACK || flags == 0, PA_ERR_INVALID);
    PA_CHECK_VALIDITY(s->context, !volume || volume->channels == s->sample_spec.channels, PA_ERR_INVALID);
    PA_CHECK_VALIDITY(s->context, !sync_stream || (direction == PA_STREAM_PLAYBACK && sync_stream->direction == PA_STREAM_PLAYBACK), PA_ERR_INVALID);

    pa_stream_ref(s);

    s->direction = direction;
    
    if (sync_stream)
        s->syncid = sync_stream->syncid;
    
    s->interpolate = !!(flags & PA_STREAM_INTERPOLATE_LATENCY);
    pa_stream_trash_ipol(s);
    
    if (attr)
        s->buffer_attr = *attr;
    else {
        /* half a second */
        s->buffer_attr.tlength = pa_bytes_per_second(&s->sample_spec)/2;
        s->buffer_attr.maxlength = (s->buffer_attr.tlength*3)/2;
        s->buffer_attr.minreq = s->buffer_attr.tlength/100;
        s->buffer_attr.prebuf = s->buffer_attr.tlength - s->buffer_attr.minreq;
        s->buffer_attr.fragsize = s->buffer_attr.tlength/100;
    }

    t = pa_tagstruct_new(NULL, 0);

    if (!dev)
        dev = s->direction == PA_STREAM_PLAYBACK ? s->context->conf->default_sink : s->context->conf->default_source;
    
    pa_tagstruct_put(
            t,
            PA_TAG_U32, s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_CREATE_PLAYBACK_STREAM : PA_COMMAND_CREATE_RECORD_STREAM,
            PA_TAG_U32, tag = s->context->ctag++,
            PA_TAG_STRING, s->name,
            PA_TAG_SAMPLE_SPEC, &s->sample_spec,
            PA_TAG_CHANNEL_MAP, &s->channel_map,
            PA_TAG_U32, PA_INVALID_INDEX,
            PA_TAG_STRING, dev,
            PA_TAG_U32, s->buffer_attr.maxlength,
            PA_TAG_BOOLEAN, !!(flags & PA_STREAM_START_CORKED),
            PA_TAG_INVALID);
    
    if (s->direction == PA_STREAM_PLAYBACK) {
        pa_cvolume cv;

        pa_tagstruct_put(
                t,
                PA_TAG_U32, s->buffer_attr.tlength,
                PA_TAG_U32, s->buffer_attr.prebuf,
                PA_TAG_U32, s->buffer_attr.minreq,
                PA_TAG_U32, s->syncid,
                PA_TAG_INVALID);

        if (!volume)
            volume = pa_cvolume_reset(&cv, s->sample_spec.channels);
        
        pa_tagstruct_put_cvolume(t, volume);
    } else
        pa_tagstruct_putu32(t, s->buffer_attr.fragsize);

    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_create_stream_callback, s);

    pa_stream_set_state(s, PA_STREAM_CREATING);
    
    pa_stream_unref(s);
    return 0;
}

int pa_stream_connect_playback(
        pa_stream *s,
        const char *dev,
        const pa_buffer_attr *attr,
        pa_stream_flags_t flags,
        pa_cvolume *volume,
        pa_stream *sync_stream) {
    
    assert(s);
    assert(s->ref >= 1);
    
    return create_stream(PA_STREAM_PLAYBACK, s, dev, attr, flags, volume, sync_stream);
}

int pa_stream_connect_record(
        pa_stream *s,
        const char *dev,
        const pa_buffer_attr *attr,
        pa_stream_flags_t flags) {
    
    assert(s);
    assert(s->ref >= 1);

    return create_stream(PA_STREAM_RECORD, s, dev, attr, flags, NULL, NULL);
}

int pa_stream_write(
        pa_stream *s,
        const void *data,
        size_t length,
        void (*free_cb)(void *p),
        int64_t offset,
        pa_seek_mode_t seek) {
    
    pa_memchunk chunk;
    
    assert(s);
    assert(s->ref >= 1);
    assert(data);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_PLAYBACK || s->direction == PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, seek <= PA_SEEK_RELATIVE_END, PA_ERR_INVALID);
    PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_PLAYBACK || (seek == PA_SEEK_RELATIVE && offset == 0), PA_ERR_INVALID);

    if (length <= 0)
        return 0;

    if (free_cb) 
        chunk.memblock = pa_memblock_new_user((void*) data, length, free_cb, 1, s->context->memblock_stat);
    else {
        chunk.memblock = pa_memblock_new(length, s->context->memblock_stat);
        memcpy(chunk.memblock->data, data, length);
    }
        
    chunk.index = 0;
    chunk.length = length;

    pa_pstream_send_memblock(s->context->pstream, s->channel, offset, seek, &chunk);
    pa_memblock_unref(chunk.memblock);
    
    if (length < s->requested_bytes)
        s->requested_bytes -= length;
    else
        s->requested_bytes = 0;

    s->counter += length;
    return 0;
}

int pa_stream_peek(pa_stream *s, const void **data, size_t *length) {
    assert(s);
    assert(s->ref >= 1);
    assert(data);
    assert(length);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_RECORD, PA_ERR_BADSTATE);
    
    if (!s->peek_memchunk.memblock) {

        if (pa_memblockq_peek(s->record_memblockq, &s->peek_memchunk) < 0) {
            *data = NULL;
            *length = 0;
            return 0;
        }
    }

    *data = (const char*) s->peek_memchunk.memblock->data + s->peek_memchunk.index;
    *length = s->peek_memchunk.length;
    return 0;
}

int pa_stream_drop(pa_stream *s) {
    assert(s);
    assert(s->ref >= 1);

    PA_CHECK_VALIDITY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->direction == PA_STREAM_RECORD, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->peek_memchunk.memblock, PA_ERR_BADSTATE);
    
    pa_memblockq_drop(s->record_memblockq, &s->peek_memchunk, s->peek_memchunk.length);
    
    pa_memblock_unref(s->peek_memchunk.memblock);
    s->peek_memchunk.length = 0;
    s->peek_memchunk.index = 0;
    s->peek_memchunk.memblock = NULL;

    s->counter += s->peek_memchunk.length;
    return 0;
}

size_t pa_stream_writable_size(pa_stream *s) {
    assert(s);
    assert(s->ref >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, (size_t) -1);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE, (size_t) -1);
    
    return s->requested_bytes;
}

size_t pa_stream_readable_size(pa_stream *s) {
    assert(s);
    assert(s->ref >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, (size_t) -1);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction == PA_STREAM_RECORD, PA_ERR_BADSTATE, (size_t) -1);

    return pa_memblockq_get_length(s->record_memblockq);
}

pa_operation * pa_stream_drain(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    
    assert(s);
    assert(s->ref >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, PA_COMMAND_DRAIN_PLAYBACK_STREAM);
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, o);

    return pa_operation_ref(o);
}

static void stream_get_latency_info_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    pa_latency_info i, *p = NULL;
    struct timeval local, remote, now;
    
    assert(pd);
    assert(o);
    assert(o->stream);
    assert(o->context);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

    } else if (pa_tagstruct_get_usec(t, &i.buffer_usec) < 0 ||
               pa_tagstruct_get_usec(t, &i.sink_usec) < 0 ||
               pa_tagstruct_get_usec(t, &i.source_usec) < 0 ||
               pa_tagstruct_get_boolean(t, &i.playing) < 0 ||
               pa_tagstruct_getu32(t, &i.queue_length) < 0 ||
               pa_tagstruct_get_timeval(t, &local) < 0 ||
               pa_tagstruct_get_timeval(t, &remote) < 0 ||
               pa_tagstruct_getu64(t, &i.counter) < 0 ||
               !pa_tagstruct_eof(t)) {
        pa_context_fail(o->context, PA_ERR_PROTOCOL);
        goto finish;
    } else {
        pa_gettimeofday(&now);
        
        if (pa_timeval_cmp(&local, &remote) <= 0 && pa_timeval_cmp(&remote, &now) <= 0) {
            /* local and remote seem to have synchronized clocks */
            
            if (o->stream->direction == PA_STREAM_PLAYBACK)
                i.transport_usec = pa_timeval_diff(&remote, &local);
            else
                i.transport_usec = pa_timeval_diff(&now, &remote);
            
            i.synchronized_clocks = 1;
            i.timestamp = remote;
        } else {
            /* clocks are not synchronized, let's estimate latency then */
            i.transport_usec = pa_timeval_diff(&now, &local)/2;
            i.synchronized_clocks = 0;
            i.timestamp = local;
            pa_timeval_add(&i.timestamp, i.transport_usec);
        }
        
        if (o->stream->interpolate) {
/*              pa_log("new interpol data\n");  */
            o->stream->ipol_timestamp = i.timestamp;
            o->stream->ipol_usec = pa_stream_get_time(o->stream, &i);
            o->stream->ipol_requested = 0;
        }

        p = &i;
    }
    
    if (o->callback) {
        pa_stream_get_latency_info_cb_t cb = (pa_stream_get_latency_info_cb_t) o->callback;
        cb(o->stream, p, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_stream_get_latency_info(pa_stream *s, pa_stream_get_latency_info_cb_t cb, void *userdata) {
    uint32_t tag;
    pa_operation *o;
    pa_tagstruct *t;
    struct timeval now;
    
    assert(s);
    assert(s->ref >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    
    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_GET_PLAYBACK_LATENCY : PA_COMMAND_GET_RECORD_LATENCY);
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_putu32(t, s->channel);

    pa_gettimeofday(&now);
    pa_tagstruct_put_timeval(t, &now);
    pa_tagstruct_putu64(t, s->counter);
    
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, stream_get_latency_info_callback, o);

    return pa_operation_ref(o);
}

void pa_stream_disconnect_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_stream *s = userdata;
    
    assert(pd);
    assert(s);
    assert(s->ref >= 1);

    pa_stream_ref(s);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(s->context, command, t) < 0)
            goto finish;

        pa_stream_set_state(s, PA_STREAM_FAILED);
        goto finish;
    } else if (!pa_tagstruct_eof(t)) {
        pa_context_fail(s->context, PA_ERR_PROTOCOL);
        goto finish;
    }

    pa_stream_set_state(s, PA_STREAM_TERMINATED);

finish:
    pa_stream_unref(s);
}

int pa_stream_disconnect(pa_stream *s) {
    pa_tagstruct *t;
    uint32_t tag;
    
    assert(s);
    assert(s->ref >= 1);

    PA_CHECK_VALIDITY(s->context, s->channel_valid, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY(s->context, s->context->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

    pa_stream_ref(s);

    t = pa_tagstruct_new(NULL, 0);
    
    pa_tagstruct_putu32(t, s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_DELETE_PLAYBACK_STREAM :
                        (s->direction == PA_STREAM_RECORD ? PA_COMMAND_DELETE_RECORD_STREAM : PA_COMMAND_DELETE_UPLOAD_STREAM));
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_disconnect_callback, s);

    pa_stream_unref(s);
    return 0;
}

void pa_stream_set_read_callback(pa_stream *s, pa_stream_request_cb_t cb, void *userdata) {
    assert(s);
    assert(s->ref >= 1);

    s->read_callback = cb;
    s->read_userdata = userdata;
}

void pa_stream_set_write_callback(pa_stream *s, pa_stream_request_cb_t cb, void *userdata) {
    assert(s);
    assert(s->ref >= 1);

    s->write_callback = cb;
    s->write_userdata = userdata;
}

void pa_stream_set_state_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    assert(s);
    assert(s->ref >= 1);

    s->state_callback = cb;
    s->state_userdata = userdata;
}

void pa_stream_set_overflow_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    assert(s);
    assert(s->ref >= 1);
    
    s->overflow_callback = cb;
    s->overflow_userdata = userdata;
}

void pa_stream_set_underflow_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *userdata) {
    assert(s);
    assert(s->ref >= 1);
    
    s->underflow_callback = cb;
    s->underflow_userdata = userdata;
}

void pa_stream_simple_ack_callback(pa_pdispatch *pd, uint32_t command, PA_GCC_UNUSED uint32_t tag, pa_tagstruct *t, void *userdata) {
    pa_operation *o = userdata;
    int success = 1;
    
    assert(pd);
    assert(o);
    assert(o->context);
    assert(o->ref >= 1);

    if (command != PA_COMMAND_REPLY) {
        if (pa_context_handle_error(o->context, command, t) < 0)
            goto finish;

        success = 0;
    } else if (!pa_tagstruct_eof(t)) {
        pa_context_fail(o->context, PA_ERR_PROTOCOL);
        goto finish;
    }

    if (o->callback) {
        pa_stream_success_cb_t cb = (pa_stream_success_cb_t) o->callback;
        cb(o->stream, success, o->userdata);
    }

finish:
    pa_operation_done(o);
    pa_operation_unref(o);
}

pa_operation* pa_stream_cork(pa_stream *s, int b, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    
    assert(s);
    assert(s->ref >= 1);
    
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

    if (s->interpolate) {
        if (!s->corked && b)
            /* Pausing */
            s->ipol_usec = pa_stream_get_interpolated_time(s);
        else if (s->corked && !b)
            /* Unpausing */
            pa_gettimeofday(&s->ipol_timestamp);
    }

    s->corked = b;
    
    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_CORK_PLAYBACK_STREAM : PA_COMMAND_CORK_RECORD_STREAM);
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_putu32(t, s->channel);
    pa_tagstruct_put_boolean(t, !!b);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, o);

    pa_operation_unref(pa_stream_get_latency_info(s, NULL, NULL));
    
    return pa_operation_ref(o);
}

static pa_operation* stream_send_simple_command(pa_stream *s, uint32_t command, pa_stream_success_cb_t cb, void *userdata) {
    pa_tagstruct *t;
    pa_operation *o;
    uint32_t tag;
    
    assert(s);
    assert(s->ref >= 1);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    
    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, command);
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_putu32(t, s->channel);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, o);

    return pa_operation_ref(o);
}

pa_operation* pa_stream_flush(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);
    
    if ((o = stream_send_simple_command(s, s->direction == PA_STREAM_PLAYBACK ? PA_COMMAND_FLUSH_PLAYBACK_STREAM : PA_COMMAND_FLUSH_RECORD_STREAM, cb, userdata)))
        pa_operation_unref(pa_stream_get_latency_info(s, NULL, NULL));
    
    return o;
}

pa_operation* pa_stream_prebuf(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);

    if ((o = stream_send_simple_command(s, PA_COMMAND_PREBUF_PLAYBACK_STREAM, cb, userdata)))
        pa_operation_unref(pa_stream_get_latency_info(s, NULL, NULL));
    
    return o;
}

pa_operation* pa_stream_trigger(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction == PA_STREAM_PLAYBACK, PA_ERR_BADSTATE);

    if ((o = stream_send_simple_command(s, PA_COMMAND_TRIGGER_PLAYBACK_STREAM, cb, userdata)))
        pa_operation_unref(pa_stream_get_latency_info(s, NULL, NULL));
    
    return o;
}

pa_operation* pa_stream_set_name(pa_stream *s, const char *name, pa_stream_success_cb_t cb, void *userdata) {
    pa_operation *o;
    pa_tagstruct *t;
    uint32_t tag;
    
    assert(s);
    assert(s->ref >= 1);
    assert(name);

    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE);
    PA_CHECK_VALIDITY_RETURN_NULL(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE);

    o = pa_operation_new(s->context, s, (pa_operation_cb_t) cb, userdata);

    t = pa_tagstruct_new(NULL, 0);
    pa_tagstruct_putu32(t, s->direction == PA_STREAM_RECORD ? PA_COMMAND_SET_RECORD_STREAM_NAME : PA_COMMAND_SET_PLAYBACK_STREAM_NAME);
    pa_tagstruct_putu32(t, tag = s->context->ctag++);
    pa_tagstruct_putu32(t, s->channel);
    pa_tagstruct_puts(t, name);
    pa_pstream_send_tagstruct(s->context->pstream, t);
    pa_pdispatch_register_reply(s->context->pdispatch, tag, DEFAULT_TIMEOUT, pa_stream_simple_ack_callback, o);

    return pa_operation_ref(o);
}

uint64_t pa_stream_get_counter(pa_stream *s) {
    assert(s);
    assert(s->ref >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, (uint64_t) -1);
    
    return s->counter;
}

pa_usec_t pa_stream_get_time(pa_stream *s, const pa_latency_info *i) {
    pa_usec_t usec;

    assert(s);
    assert(s->ref >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, (pa_usec_t) -1);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE, (pa_usec_t) -1);
    
    usec = pa_bytes_to_usec(i->counter, &s->sample_spec);

    if (i) {
        if (s->direction == PA_STREAM_PLAYBACK) {
            pa_usec_t latency = i->transport_usec + i->buffer_usec + i->sink_usec;
            if (usec < latency)
                usec = 0;
            else
                usec -= latency;
                
        } else if (s->direction == PA_STREAM_RECORD) {
            usec += i->source_usec + i->buffer_usec + i->transport_usec;

            if (usec > i->sink_usec)
                usec -= i->sink_usec;
            else
                usec = 0;
        }
    }

    if (usec < s->previous_time)
        usec = s->previous_time;

    s->previous_time = usec;

    return usec;
}

static pa_usec_t time_counter_diff(pa_stream *s, pa_usec_t t, pa_usec_t c, int *negative) {
    assert(s);
    assert(s->ref >= 1);
    
    if (negative)
        *negative = 0;

    if (c < t) {
        if (s->direction == PA_STREAM_RECORD) {
            if (negative)
                *negative = 1;

            return t-c;
        } else
            return 0;
    } else
        return c-t;
}

pa_usec_t pa_stream_get_latency(pa_stream *s, const pa_latency_info *i, int *negative) {
    pa_usec_t t, c;

    assert(s);
    assert(s->ref >= 1);
    assert(i);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, (pa_usec_t) -1);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE, (pa_usec_t) -1);
    
    t = pa_stream_get_time(s, i);
    c = pa_bytes_to_usec(s->counter, &s->sample_spec);

    return time_counter_diff(s, t, c, negative);
}

const pa_sample_spec* pa_stream_get_sample_spec(pa_stream *s) {
    assert(s);
    assert(s->ref >= 1);

    return &s->sample_spec;
}

const pa_channel_map* pa_stream_get_channel_map(pa_stream *s) {
    assert(s);
    assert(s->ref >= 1);

    return &s->channel_map;
}

void pa_stream_trash_ipol(pa_stream *s) {
    assert(s);
    assert(s->ref >= 1);

    if (!s->interpolate)
        return;

    memset(&s->ipol_timestamp, 0, sizeof(s->ipol_timestamp));
    s->ipol_usec = 0;
}

pa_usec_t pa_stream_get_interpolated_time(pa_stream *s) {
    pa_usec_t usec;

    assert(s);
    assert(s->ref >= 1);
    
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, (pa_usec_t) -1);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE, (pa_usec_t) -1);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->interpolate, PA_ERR_BADSTATE, (pa_usec_t) -1);
    
    if (s->corked)
        usec = s->ipol_usec;
    else {
        if (s->ipol_timestamp.tv_sec == 0)
            usec = 0;
        else
            usec = s->ipol_usec + pa_timeval_age(&s->ipol_timestamp);
    }
    
    if (usec < s->previous_ipol_time)
        usec = s->previous_ipol_time;

    s->previous_ipol_time = usec;

    return usec;
}

pa_usec_t pa_stream_get_interpolated_latency(pa_stream *s, int *negative) {
    pa_usec_t t, c;

    assert(s);
    assert(s->ref >= 1);

    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->state == PA_STREAM_READY, PA_ERR_BADSTATE, (pa_usec_t) -1);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->direction != PA_STREAM_UPLOAD, PA_ERR_BADSTATE, (pa_usec_t) -1);
    PA_CHECK_VALIDITY_RETURN_ANY(s->context, s->interpolate, PA_ERR_BADSTATE, (pa_usec_t) -1);
    
    t = pa_stream_get_interpolated_time(s);
    c = pa_bytes_to_usec(s->counter, &s->sample_spec);
    return time_counter_diff(s, t, c, negative);
}
