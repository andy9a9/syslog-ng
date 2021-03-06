/*
 * Copyright (c) 2002-2014 Balabit
 * Copyright (c) 2014 Laszlo Budai
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "ack_tracker.h"
#include "bookmark.h"
#include "ringbuffer.h"
#include "syslog-ng.h"

typedef struct _LateAckRecord
{
  AckRecord super;
  gboolean acked;
  Bookmark bookmark;
} LateAckRecord;

typedef struct LateAckTracker
{
  AckTracker super;
  LateAckRecord *pending_ack_record;
  RingBuffer ack_record_storage;
  GStaticMutex storage_mutex;
} LateAckTracker;

static inline void
late_ack_record_destroy(LateAckRecord *self)
{
  if (self->bookmark.destroy)
    self->bookmark.destroy(&(self->bookmark));
}

static inline void
_late_tracker_lock(LateAckTracker *self)
{
  g_static_mutex_lock(&self->storage_mutex);
}

static inline void
_late_tracker_unlock(LateAckTracker *self)
{
  g_static_mutex_unlock(&self->storage_mutex);
}

static inline gboolean
_ack_range_is_continuous(void *data)
{
  LateAckRecord *ack_rec = (LateAckRecord *)data;

  return ack_rec->acked;
}

static inline guint32
_get_continuous_range_length(LateAckTracker *self)
{
  return ring_buffer_get_continual_range_length(&self->ack_record_storage, _ack_range_is_continuous);
}

static inline void
_drop_range(LateAckTracker *self, guint32 n)
{
  int i;
  LateAckRecord *ack_rec;

  for (i = 0; i < n; i++)
    {
      ack_rec = ring_buffer_element_at(&self->ack_record_storage, i);
      ack_rec->acked = FALSE;

      late_ack_record_destroy(ack_rec);

      ack_rec->bookmark.save = NULL;
      ack_rec->bookmark.destroy = NULL;
    }
  ring_buffer_drop(&self->ack_record_storage, n);
}

static void
late_ack_tracker_track_msg(AckTracker *s, LogMessage *msg)
{
  LateAckTracker *self = (LateAckTracker *)s;
  LogSource *source = self->super.source;

  g_assert(self->pending_ack_record != NULL);

  log_pipe_ref((LogPipe *)source);

  msg->ack_record = (AckRecord *)self->pending_ack_record;

  _late_tracker_lock(self);
  {
    LateAckRecord *ack_rec;
    ack_rec = (LateAckRecord *)ring_buffer_push(&self->ack_record_storage);
    g_assert(ack_rec == self->pending_ack_record);
  }
  _late_tracker_unlock(self);

  self->pending_ack_record = NULL;
}

static void
late_ack_tracker_manage_msg_ack(AckTracker *s, LogMessage *msg, AckType ack_type)
{
  LateAckTracker *self = (LateAckTracker *)s;
  LateAckRecord *ack_rec = (LateAckRecord *)msg->ack_record;
  LateAckRecord *last_in_range = NULL;
  guint32 ack_range_length = 0;

  ack_rec->acked = TRUE;

  _late_tracker_lock(self);
  {
    ack_range_length = _get_continuous_range_length(self);
    if (ack_range_length > 0)
      {
        last_in_range = ring_buffer_element_at(&self->ack_record_storage, ack_range_length - 1);
        if (ack_type != AT_ABORTED)
          {
            Bookmark *bookmark = &(last_in_range->bookmark);
            bookmark->save(bookmark);
          }
        _drop_range(self, ack_range_length);

        if (ack_type == AT_SUSPENDED)
          log_source_flow_control_suspend(self->super.source);
        else
          log_source_flow_control_adjust(self->super.source, ack_range_length);
      }
  }
  _late_tracker_unlock(self);

  log_msg_unref(msg);
  log_pipe_unref((LogPipe *)self->super.source);
}

static Bookmark *
late_ack_tracker_request_bookmark(AckTracker *s)
{
  LateAckTracker *self = (LateAckTracker *)s;

  _late_tracker_lock(self);
  {
    self->pending_ack_record = ring_buffer_tail(&self->ack_record_storage);
  }
  _late_tracker_unlock(self);

  if (self->pending_ack_record)
    {
      self->pending_ack_record->bookmark.persist_state = s->source->super.cfg->state;

      self->pending_ack_record->super.tracker = (AckTracker *)self;

      return &(self->pending_ack_record->bookmark);
    }

  return NULL;
}

static void
late_ack_tracker_init_instance(LateAckTracker *self, LogSource *source)
{
  self->super.late = TRUE;
  self->super.source = source;
  source->ack_tracker = (AckTracker *)self;
  self->super.request_bookmark = late_ack_tracker_request_bookmark;
  self->super.track_msg = late_ack_tracker_track_msg;
  self->super.manage_msg_ack = late_ack_tracker_manage_msg_ack;
  ring_buffer_alloc(&self->ack_record_storage, sizeof(LateAckRecord), log_source_get_init_window_size(source));
  g_static_mutex_init(&self->storage_mutex);
}

AckTracker *
late_ack_tracker_new(LogSource *source)
{
  LateAckTracker *self = g_new0(LateAckTracker, 1);

  late_ack_tracker_init_instance(self, source);

  return (AckTracker *)self;
}

void
late_ack_tracker_free(AckTracker *s)
{
  LateAckTracker *self = (LateAckTracker *)s;
  guint32 count = ring_buffer_count(&self->ack_record_storage);

  g_static_mutex_free(&self->storage_mutex);

  _drop_range(self, count);

  ring_buffer_free(&self->ack_record_storage);
  g_free(self);
}
