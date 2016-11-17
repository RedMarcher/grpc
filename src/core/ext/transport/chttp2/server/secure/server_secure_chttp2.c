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

#include <grpc/grpc.h>

#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>
#include <grpc/support/useful.h>
#include "src/core/ext/transport/chttp2/transport/chttp2_transport.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/handshaker.h"
#include "src/core/lib/channel/http_server_filter.h"
#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/tcp_server.h"
#include "src/core/lib/security/context/security_context.h"
#include "src/core/lib/security/credentials/credentials.h"
#include "src/core/lib/security/transport/auth_filters.h"
#include "src/core/lib/security/transport/security_connector.h"
#include "src/core/lib/surface/api_trace.h"
#include "src/core/lib/surface/server.h"

typedef struct pending_handshake_manager_node {
  grpc_handshake_manager* handshake_mgr;
  struct pending_handshake_manager_node *next;
} pending_handshake_manager_node;

typedef struct server_secure_state {
  grpc_server *server;
  grpc_tcp_server *tcp_server;
  grpc_server_security_connector *sc;
  grpc_server_credentials *creds;
  gpr_mu mu;
  bool shutdown;
  grpc_closure tcp_server_shutdown_complete;
  grpc_closure *server_destroy_listener_done;
  pending_handshake_manager_node *pending_handshake_mgrs;
} server_secure_state;

typedef struct server_secure_connection_state {
  server_secure_state *server_state;
  grpc_pollset *accepting_pollset;
  grpc_tcp_server_acceptor *acceptor;
  grpc_handshake_manager *handshake_mgr;
} server_secure_connection_state;

static void pending_handshake_manager_add_locked(
    server_secure_state* state, grpc_handshake_manager* handshake_mgr) {
  pending_handshake_manager_node* node = gpr_malloc(sizeof(*node));
  node->handshake_mgr = handshake_mgr;
  node->next = state->pending_handshake_mgrs;
  state->pending_handshake_mgrs = node;
}

static void pending_handshake_manager_remove_locked(
    server_secure_state* state, grpc_handshake_manager* handshake_mgr) {
  pending_handshake_manager_node** prev_node = &state->pending_handshake_mgrs;
  for (pending_handshake_manager_node* node = state->pending_handshake_mgrs;
       node != NULL; node = node->next) {
    if (node->handshake_mgr == handshake_mgr) {
      *prev_node = node->next;
      gpr_free(node);
      break;
    }
    prev_node = &node->next;
  }
}

static void pending_handshake_manager_shutdown_locked(
    grpc_exec_ctx* exec_ctx, server_secure_state* state) {
  pending_handshake_manager_node* prev_node = NULL;
  for (pending_handshake_manager_node* node = state->pending_handshake_mgrs;
       node != NULL; node = node->next) {
    grpc_handshake_manager_shutdown(exec_ctx, node->handshake_mgr);
    gpr_free(prev_node);
    prev_node = node;
  }
  gpr_free(prev_node);
  state->pending_handshake_mgrs = NULL;
}

static void on_handshake_done(grpc_exec_ctx *exec_ctx, void *arg,
                              grpc_error *error) {
  grpc_handshaker_args *args = arg;
  server_secure_connection_state *connection_state = args->user_data;
  if (error != GRPC_ERROR_NONE) {
    const char *error_str = grpc_error_string(error);
    gpr_log(GPR_ERROR, "Handshaking failed: %s", error_str);
    grpc_error_free_string(error_str);
    grpc_endpoint_destroy(exec_ctx, args->endpoint);
    gpr_free(args->read_buffer);
    gpr_mu_lock(&connection_state->server_state->mu);
  } else {
    gpr_mu_lock(&connection_state->server_state->mu);
    if (!connection_state->server_state->shutdown) {
      grpc_arg channel_arg = grpc_server_credentials_to_arg(
          connection_state->server_state->creds);
      grpc_channel_args *args_copy =
          grpc_channel_args_copy_and_add(args->args, &channel_arg, 1);
      grpc_transport *transport =
          grpc_create_chttp2_transport(exec_ctx, args_copy, args->endpoint, 0);
      grpc_server_setup_transport(
          exec_ctx, connection_state->server_state->server, transport,
          connection_state->accepting_pollset, args_copy);
      grpc_channel_args_destroy(args_copy);
      grpc_chttp2_transport_start_reading(exec_ctx, transport,
                                          args->read_buffer);
    } else {
      // Need to destroy this here, because the server may have already
      // gone away.
      grpc_endpoint_destroy(exec_ctx, args->endpoint);
    }
  }
  pending_handshake_manager_remove_locked(connection_state->server_state,
                                          connection_state->handshake_mgr);
  gpr_mu_unlock(&connection_state->server_state->mu);
  grpc_handshake_manager_destroy(exec_ctx, connection_state->handshake_mgr);
  grpc_tcp_server_unref(exec_ctx, connection_state->server_state->tcp_server);
  gpr_free(connection_state);
  grpc_channel_args_destroy(args->args);
}

static void on_accept(grpc_exec_ctx *exec_ctx, void *arg, grpc_endpoint *tcp,
                      grpc_pollset *accepting_pollset,
                      grpc_tcp_server_acceptor *acceptor) {
  server_secure_state *state = arg;
  gpr_mu_lock(&state->mu);
  if (state->shutdown) {
    gpr_mu_unlock(&state->mu);
    grpc_endpoint_destroy(exec_ctx, tcp);
    return;
  }
  grpc_handshake_manager* handshake_mgr = grpc_handshake_manager_create();
  pending_handshake_manager_add_locked(state, handshake_mgr);
  gpr_mu_unlock(&state->mu);
  grpc_tcp_server_ref(state->tcp_server);
  server_secure_connection_state *connection_state =
      gpr_malloc(sizeof(*connection_state));
  connection_state->server_state = state;
  connection_state->accepting_pollset = accepting_pollset;
  connection_state->acceptor = acceptor;
  connection_state->handshake_mgr = handshake_mgr;
  grpc_server_security_connector_create_handshakers(
      exec_ctx, state->sc, connection_state->handshake_mgr);
  // TODO(roth): We should really get this timeout value from channel
  // args instead of hard-coding it.
  const gpr_timespec deadline = gpr_time_add(
      gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_seconds(120, GPR_TIMESPAN));
  grpc_handshake_manager_do_handshake(
      exec_ctx, connection_state->handshake_mgr, tcp,
      grpc_server_get_channel_args(state->server),
      deadline, acceptor, on_handshake_done, connection_state);
}

/* Server callback: start listening on our ports */
static void server_start_listener(grpc_exec_ctx *exec_ctx, grpc_server *server,
                                  void *arg, grpc_pollset **pollsets,
                                  size_t pollset_count) {
  server_secure_state *state = arg;
  gpr_mu_lock(&state->mu);
  state->shutdown = false;
  gpr_mu_unlock(&state->mu);
  grpc_tcp_server_start(exec_ctx, state->tcp_server, pollsets, pollset_count,
                        on_accept, state);
}

static void tcp_server_shutdown_complete(grpc_exec_ctx *exec_ctx, void *arg,
                                         grpc_error *error) {
  server_secure_state *state = arg;
  /* ensure all threads have unlocked */
  gpr_mu_lock(&state->mu);
  grpc_closure *destroy_done = state->server_destroy_listener_done;
  GPR_ASSERT(state->shutdown);
  pending_handshake_manager_shutdown_locked(exec_ctx, state);
  gpr_mu_unlock(&state->mu);
  /* Flush queued work before a synchronous unref. */
  grpc_exec_ctx_flush(exec_ctx);
  GRPC_SECURITY_CONNECTOR_UNREF(&state->sc->base, "server");
  grpc_server_credentials_unref(state->creds);
  if (destroy_done != NULL) {
    destroy_done->cb(exec_ctx, destroy_done->cb_arg, GRPC_ERROR_REF(error));
    grpc_exec_ctx_flush(exec_ctx);
  }
  gpr_mu_destroy(&state->mu);
  gpr_free(state);
}

static void server_destroy_listener(grpc_exec_ctx *exec_ctx,
                                    grpc_server *server, void *arg,
                                    grpc_closure *destroy_done) {
  server_secure_state *state = arg;
  gpr_mu_lock(&state->mu);
  state->shutdown = true;
  state->server_destroy_listener_done = destroy_done;
  grpc_tcp_server *tcp_server = state->tcp_server;
  gpr_mu_unlock(&state->mu);
  grpc_tcp_server_shutdown_listeners(exec_ctx, tcp_server);
  grpc_tcp_server_unref(exec_ctx, tcp_server);
}

int grpc_server_add_secure_http2_port(grpc_server *server, const char *addr,
                                      grpc_server_credentials *creds) {
  grpc_resolved_addresses *resolved = NULL;
  grpc_tcp_server *tcp_server = NULL;
  server_secure_state *state = NULL;
  size_t i;
  size_t count = 0;
  int port_num = -1;
  int port_temp;
  grpc_security_status status = GRPC_SECURITY_ERROR;
  grpc_server_security_connector *sc = NULL;
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  grpc_error *err = GRPC_ERROR_NONE;
  grpc_error **errors = NULL;

  GRPC_API_TRACE(
      "grpc_server_add_secure_http2_port("
      "server=%p, addr=%s, creds=%p)",
      3, (server, addr, creds));

  /* create security context */
  if (creds == NULL) {
    err = GRPC_ERROR_CREATE(
        "No credentials specified for secure server port (creds==NULL)");
    goto error;
  }
  status = grpc_server_credentials_create_security_connector(creds, &sc);
  if (status != GRPC_SECURITY_OK) {
    char *msg;
    gpr_asprintf(&msg,
                 "Unable to create secure server with credentials of type %s.",
                 creds->type);
    err = grpc_error_set_int(GRPC_ERROR_CREATE(msg),
                             GRPC_ERROR_INT_SECURITY_STATUS, status);
    gpr_free(msg);
    goto error;
  }

  /* resolve address */
  err = grpc_blocking_resolve_address(addr, "https", &resolved);
  if (err != GRPC_ERROR_NONE) {
    goto error;
  }
  state = gpr_malloc(sizeof(*state));
  memset(state, 0, sizeof(*state));
  grpc_closure_init(&state->tcp_server_shutdown_complete,
                    tcp_server_shutdown_complete, state);
  err = grpc_tcp_server_create(&exec_ctx,
                               &state->tcp_server_shutdown_complete,
                               grpc_server_get_channel_args(server),
                               &tcp_server);
  if (err != GRPC_ERROR_NONE) {
    goto error;
  }

  state->server = server;
  state->tcp_server = tcp_server;
  state->sc = sc;
  state->creds = grpc_server_credentials_ref(creds);
  state->shutdown = true;
  gpr_mu_init(&state->mu);

  errors = gpr_malloc(sizeof(*errors) * resolved->naddrs);
  for (i = 0; i < resolved->naddrs; i++) {
    errors[i] =
        grpc_tcp_server_add_port(tcp_server, &resolved->addrs[i], &port_temp);
    if (errors[i] == GRPC_ERROR_NONE) {
      if (port_num == -1) {
        port_num = port_temp;
      } else {
        GPR_ASSERT(port_num == port_temp);
      }
      count++;
    }
  }
  if (count == 0) {
    char *msg;
    gpr_asprintf(&msg, "No address added out of total %" PRIuPTR " resolved",
                 resolved->naddrs);
    err = GRPC_ERROR_CREATE_REFERENCING(msg, errors, resolved->naddrs);
    gpr_free(msg);
    goto error;
  } else if (count != resolved->naddrs) {
    char *msg;
    gpr_asprintf(&msg, "Only %" PRIuPTR
                       " addresses added out of total %" PRIuPTR " resolved",
                 count, resolved->naddrs);
    err = GRPC_ERROR_CREATE_REFERENCING(msg, errors, resolved->naddrs);
    gpr_free(msg);

    const char *warning_message = grpc_error_string(err);
    gpr_log(GPR_INFO, "WARNING: %s", warning_message);
    grpc_error_free_string(warning_message);
    /* we managed to bind some addresses: continue */
  } else {
    for (i = 0; i < resolved->naddrs; i++) {
      GRPC_ERROR_UNREF(errors[i]);
    }
  }
  gpr_free(errors);
  errors = NULL;
  grpc_resolved_addresses_destroy(resolved);

  /* Register with the server only upon success */
  grpc_server_add_listener(&exec_ctx, server, state,
                           server_start_listener, server_destroy_listener);

  grpc_exec_ctx_finish(&exec_ctx);
  return port_num;

/* Error path: cleanup and return */
error:
  GPR_ASSERT(err != GRPC_ERROR_NONE);
  if (errors != NULL) {
    for (i = 0; i < resolved->naddrs; i++) {
      GRPC_ERROR_UNREF(errors[i]);
    }
    gpr_free(errors);
  }
  if (resolved) {
    grpc_resolved_addresses_destroy(resolved);
  }
  if (tcp_server) {
    grpc_tcp_server_unref(&exec_ctx, tcp_server);
  } else {
    if (sc) {
      grpc_exec_ctx_flush(&exec_ctx);
      GRPC_SECURITY_CONNECTOR_UNREF(&sc->base, "server");
    }
    if (state) {
      gpr_free(state);
    }
  }
  grpc_exec_ctx_finish(&exec_ctx);
  const char *msg = grpc_error_string(err);
  GRPC_ERROR_UNREF(err);
  gpr_log(GPR_ERROR, "%s", msg);
  grpc_error_free_string(msg);
  return 0;
}
