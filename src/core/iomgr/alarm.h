/*
 *
 * Copyright 2014, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef __GRPC_INTERNAL_IOMGR_ALARM_H__
#define __GRPC_INTERNAL_IOMGR_ALARM_H__

#include "src/core/iomgr/iomgr.h"
#include <grpc/support/port_platform.h>
#include <grpc/support/time.h>

typedef struct grpc_alarm grpc_alarm;

/* One of the following headers should provide struct grpc_alarm */
#ifdef GPR_LIBEVENT
#include "src/core/iomgr/iomgr_libevent.h"
#endif

/* Initialize *alarm. When expired or canceled, alarm_cb will be called with
   *alarm_cb_arg and status to indicate if it expired (SUCCESS) or was
   canceled (CANCELLED). alarm_cb is guaranteed to be called exactly once,
   and application code should check the status to determine how it was
   invoked. The application callback is also responsible for maintaining
   information about when to free up any user-level state.
   Returns 1 on success, 0 on failure. */
int grpc_alarm_init(grpc_alarm *alarm, gpr_timespec deadline,
                    grpc_iomgr_cb_func alarm_cb, void *alarm_cb_arg,
                    gpr_timespec now);

/* Note that there is no alarm destroy function. This is because the
   alarm is a one-time occurrence with a guarantee that the callback will
   be called exactly once, either at expiration or cancellation. Thus, all
   the internal alarm event management state is destroyed just before
   that callback is invoked. If the user has additional state associated with
   the alarm, the user is responsible for determining when it is safe to
   destroy that state. */

/* Cancel an *alarm.
   There are three cases:
   1. We normally cancel the alarm
   2. The alarm has already run
   3. We can't cancel the alarm because it is "in flight".

   In all of these cases, the cancellation is still considered successful.
   They are essentially distinguished in that the alarm_cb will be run
   exactly once from either the cancellation (with status CANCELLED)
   or from the activation (with status SUCCESS)

   Requires:  cancel() must happen after add() on a given alarm */
int grpc_alarm_cancel(grpc_alarm *alarm);

#endif /* __GRPC_INTERNAL_IOMGR_ALARM_H__ */