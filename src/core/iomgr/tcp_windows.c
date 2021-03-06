/*
 *
 * Copyright 2015, Google Inc.
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

#include <grpc/support/port_platform.h>

#ifdef GPR_WINSOCK_SOCKET

#include "src/core/iomgr/sockaddr_win32.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/log_win32.h>
#include <grpc/support/slice_buffer.h>
#include <grpc/support/useful.h>

#include "src/core/iomgr/alarm.h"
#include "src/core/iomgr/iocp_windows.h"
#include "src/core/iomgr/sockaddr.h"
#include "src/core/iomgr/sockaddr_utils.h"
#include "src/core/iomgr/socket_windows.h"
#include "src/core/iomgr/tcp_client.h"

static int set_non_block(SOCKET sock) {
  int status;
  unsigned long param = 1;
  DWORD ret;
  status = WSAIoctl(sock, FIONBIO, &param, sizeof(param), NULL, 0, &ret,
                    NULL, NULL);
  return status == 0;
}

static int set_dualstack(SOCKET sock) {
  int status;
  unsigned long param = 0;
  status = setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
                      (const char *) &param, sizeof(param));
  return status == 0;
}

int grpc_tcp_prepare_socket(SOCKET sock) {
  if (!set_non_block(sock))
    return 0;
  if (!set_dualstack(sock))
    return 0;
  return 1;
}

typedef struct grpc_tcp {
  grpc_endpoint base;
  grpc_winsocket *socket;
  gpr_refcount refcount;

  grpc_endpoint_read_cb read_cb;
  void *read_user_data;
  gpr_slice read_slice;
  int outstanding_read;

  grpc_endpoint_write_cb write_cb;
  void *write_user_data;
  gpr_slice_buffer write_slices;
  int outstanding_write;

} grpc_tcp;

static void tcp_ref(grpc_tcp *tcp) {
  gpr_ref(&tcp->refcount);
}

static void tcp_unref(grpc_tcp *tcp) {
  if (gpr_unref(&tcp->refcount)) {
    gpr_slice_buffer_destroy(&tcp->write_slices);
    grpc_winsocket_orphan(tcp->socket);
    gpr_free(tcp);
  }
}

static void on_read(void *tcpp, int success) {
  grpc_tcp *tcp = (grpc_tcp *) tcpp;
  grpc_winsocket *socket = tcp->socket;
  gpr_slice sub;
  gpr_slice *slice = NULL;
  size_t nslices = 0;
  grpc_endpoint_cb_status status;
  grpc_endpoint_read_cb cb = tcp->read_cb;
  grpc_winsocket_callback_info *info = &socket->read_info;
  void *opaque = tcp->read_user_data;

  GPR_ASSERT(tcp->outstanding_read);

  if (!success) {
    tcp_unref(tcp);
    cb(opaque, NULL, 0, GRPC_ENDPOINT_CB_SHUTDOWN);
    return;
  }

  tcp->outstanding_read = 0;

  if (socket->read_info.wsa_error != 0) {
    char *utf8_message = gpr_format_message(info->wsa_error);
    gpr_log(GPR_ERROR, "ReadFile overlapped error: %s", utf8_message);
    gpr_free(utf8_message);
    status = GRPC_ENDPOINT_CB_ERROR;
  } else {
    if (info->bytes_transfered != 0) {
      sub = gpr_slice_sub(tcp->read_slice, 0, info->bytes_transfered);
      status = GRPC_ENDPOINT_CB_OK;
      slice = &sub;
      nslices = 1;
    } else {
      gpr_slice_unref(tcp->read_slice);
      status = GRPC_ENDPOINT_CB_EOF;
    }
  }
  tcp_unref(tcp);
  cb(opaque, slice, nslices, status);
}

static void win_notify_on_read(grpc_endpoint *ep,
                               grpc_endpoint_read_cb cb, void *arg) {
  grpc_tcp *tcp = (grpc_tcp *) ep;
  grpc_winsocket *handle = tcp->socket;
  grpc_winsocket_callback_info *info = &handle->read_info;
  int status;
  DWORD bytes_read = 0;
  DWORD flags = 0;
  int error;
  WSABUF buffer;

  GPR_ASSERT(!tcp->outstanding_read);
  tcp_ref(tcp);
  tcp->outstanding_read = 1;
  tcp->read_cb = cb;
  tcp->read_user_data = arg;

  tcp->read_slice = gpr_slice_malloc(8192);

  buffer.len = GPR_SLICE_LENGTH(tcp->read_slice);
  buffer.buf = (char *)GPR_SLICE_START_PTR(tcp->read_slice);

  status = WSARecv(tcp->socket->socket, &buffer, 1, &bytes_read, &flags,
                   NULL, NULL);
  info->wsa_error = status == 0 ? 0 : WSAGetLastError();

  if (info->wsa_error != WSAEWOULDBLOCK) {
    info->bytes_transfered = bytes_read;
    /* This might heavily recurse. */
    on_read(tcp, 1);
    return;
  }

  memset(&tcp->socket->read_info.overlapped, 0, sizeof(OVERLAPPED));
  status = WSARecv(tcp->socket->socket, &buffer, 1, &bytes_read, &flags,
                   &info->overlapped, NULL);

  if (status == 0) {
    grpc_socket_notify_on_read(tcp->socket, on_read, tcp);
    return;
  }

  error = WSAGetLastError();

  if (error != WSA_IO_PENDING) {
    char *utf8_message = gpr_format_message(WSAGetLastError());
    __debugbreak();
    gpr_log(GPR_ERROR, "WSARecv error: %s", utf8_message);
    gpr_free(utf8_message);
    /* would the IO completion port be called anyway... ? Let's assume not. */
    tcp->outstanding_read = 0;
    tcp_unref(tcp);
    cb(arg, NULL, 0, GRPC_ENDPOINT_CB_ERROR);
    return;
  }

  grpc_socket_notify_on_read(tcp->socket, on_read, tcp);
}

static void on_write(void *tcpp, int success) {
  grpc_tcp *tcp = (grpc_tcp *) tcpp;
  grpc_winsocket *handle = tcp->socket;
  grpc_winsocket_callback_info *info = &handle->write_info;
  grpc_endpoint_cb_status status = GRPC_ENDPOINT_CB_OK;
  grpc_endpoint_write_cb cb = tcp->write_cb;
  void *opaque = tcp->write_user_data;

  GPR_ASSERT(tcp->outstanding_write);

  if (!success) {
    tcp_unref(tcp);
    cb(opaque, GRPC_ENDPOINT_CB_SHUTDOWN);
    return;
  }

  if (info->wsa_error != 0) {
    char *utf8_message = gpr_format_message(info->wsa_error);
    gpr_log(GPR_ERROR, "WSASend overlapped error: %s", utf8_message);
    gpr_free(utf8_message);
    status = GRPC_ENDPOINT_CB_ERROR;
  } else {
    GPR_ASSERT(info->bytes_transfered == tcp->write_slices.length);
  }

  gpr_slice_buffer_reset_and_unref(&tcp->write_slices);
  tcp->outstanding_write = 0;

  tcp_unref(tcp);
  cb(opaque, status);
}

static grpc_endpoint_write_status win_write(grpc_endpoint *ep,
                                            gpr_slice *slices, size_t nslices,
                                            grpc_endpoint_write_cb cb,
                                            void *arg) {
  grpc_tcp *tcp = (grpc_tcp *) ep;
  grpc_winsocket *socket = tcp->socket;
  grpc_winsocket_callback_info *info = &socket->write_info;
  unsigned i;
  DWORD bytes_sent;
  int status;
  WSABUF local_buffers[16];
  WSABUF *allocated = NULL;
  WSABUF *buffers = local_buffers;

  GPR_ASSERT(!tcp->outstanding_write);
  tcp_ref(tcp);

  tcp->outstanding_write = 1;
  tcp->write_cb = cb;
  tcp->write_user_data = arg;
  gpr_slice_buffer_addn(&tcp->write_slices, slices, nslices);

  if (tcp->write_slices.count > GPR_ARRAY_SIZE(local_buffers)) {
    buffers = (WSABUF *) gpr_malloc(sizeof(WSABUF) * tcp->write_slices.count);
    allocated = buffers;
  }

  for (i = 0; i < tcp->write_slices.count; i++) {
    buffers[i].len = GPR_SLICE_LENGTH(tcp->write_slices.slices[i]);
    buffers[i].buf = (char *)GPR_SLICE_START_PTR(tcp->write_slices.slices[i]);
  }

  status = WSASend(socket->socket, buffers, tcp->write_slices.count,
                   &bytes_sent, 0, NULL, NULL);
  info->wsa_error = status == 0 ? 0 : WSAGetLastError();

  if (info->wsa_error != WSAEWOULDBLOCK) {
    grpc_endpoint_write_status ret = GRPC_ENDPOINT_WRITE_ERROR;
    if (status == 0) {
      ret = GRPC_ENDPOINT_WRITE_DONE;
      GPR_ASSERT(bytes_sent == tcp->write_slices.length);
    } else {
      char *utf8_message = gpr_format_message(info->wsa_error);
      gpr_log(GPR_ERROR, "WSASend error: %s", utf8_message);
      gpr_free(utf8_message);
    }
    if (allocated) gpr_free(allocated);
    gpr_slice_buffer_reset_and_unref(&tcp->write_slices);
    tcp->outstanding_write = 0;
    tcp_unref(tcp);
    return ret;
  }

  memset(&socket->write_info, 0, sizeof(OVERLAPPED));
  status = WSASend(socket->socket, buffers, tcp->write_slices.count,
                   &bytes_sent, 0, &socket->write_info.overlapped, NULL);
  if (allocated) gpr_free(allocated);

  if (status != 0) {
    int error = WSAGetLastError();
    if (error != WSA_IO_PENDING) {
      char *utf8_message = gpr_format_message(WSAGetLastError());
      __debugbreak();
      gpr_log(GPR_ERROR, "WSASend error: %s", utf8_message);
      gpr_free(utf8_message);
      /* would the IO completion port be called anyway ? Let's assume not. */
      tcp->outstanding_write = 0;
      tcp_unref(tcp);
      return GRPC_ENDPOINT_WRITE_ERROR;
    }
  }

  grpc_socket_notify_on_write(socket, on_write, tcp);
  return GRPC_ENDPOINT_WRITE_PENDING;
}

static void win_add_to_pollset(grpc_endpoint *ep, grpc_pollset *pollset) {
  grpc_tcp *tcp = (grpc_tcp *) ep;
  grpc_iocp_add_socket(tcp->socket);
}

static void win_shutdown(grpc_endpoint *ep) {
  grpc_tcp *tcp = (grpc_tcp *) ep;
  grpc_winsocket_shutdown(tcp->socket);
}

static void win_destroy(grpc_endpoint *ep) {
  grpc_tcp *tcp = (grpc_tcp *) ep;
  tcp_unref(tcp);
}

static grpc_endpoint_vtable vtable = {
  win_notify_on_read, win_write, win_add_to_pollset, win_shutdown, win_destroy
};

grpc_endpoint *grpc_tcp_create(grpc_winsocket *socket) {
  grpc_tcp *tcp = (grpc_tcp *) gpr_malloc(sizeof(grpc_tcp));
  memset(tcp, 0, sizeof(grpc_tcp));
  tcp->base.vtable = &vtable;
  tcp->socket = socket;
  gpr_slice_buffer_init(&tcp->write_slices);
  gpr_ref_init(&tcp->refcount, 1);
  return &tcp->base;
}

#endif  /* GPR_WINSOCK_SOCKET */
