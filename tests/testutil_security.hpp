/*
    Copyright (c) 2007-2017 Contributors as noted in the AUTHORS file

    This file is part of libzmq, the ZeroMQ core engine in C++.

    libzmq is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    As a special exception, the Contributors give you permission to link
    this library with independent modules to produce an executable,
    regardless of the license terms of these independent modules, and to
    copy and distribute the resulting executable under terms of your choice,
    provided that you also meet, for each linked independent module, the
    terms and conditions of the license of that module. An independent
    module is a module which is not derived from or based on this library.
    If you modify this library, you must extend this exception to your
    version of the library.

    libzmq is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
    License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __TESTUTIL_SECURITY_HPP_INCLUDED__
#define __TESTUTIL_SECURITY_HPP_INCLUDED__

#include "testutil_unity.hpp"
#include "testutil_monitoring.hpp"

//  security test utils

typedef void(socket_config_fn) (void *, void *);

const char *test_zap_domain = "ZAPTEST";

//  NULL specific functions
void socket_config_null_client (void *server_, void *server_secret_)
{
    LIBZMQ_UNUSED (server_);
    LIBZMQ_UNUSED (server_secret_);
}

void socket_config_null_server (void *server_, void *server_secret_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (
      server_, ZMQ_ZAP_DOMAIN, test_zap_domain, strlen (test_zap_domain)));
#ifdef ZMQ_ZAP_ENFORCE_DOMAIN
    int required = server_secret_ ? *(int *) server_secret_ : 0;
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (server_, ZMQ_ZAP_ENFORCE_DOMAIN,
                                               &required, sizeof (int)));
#else
    LIBZMQ_UNUSED (server_secret_);
#endif
}

//  PLAIN specific functions
const char *test_plain_username = "testuser";
const char *test_plain_password = "testpass";

void socket_config_plain_client (void *server_, void *server_secret_)
{
    LIBZMQ_UNUSED (server_secret_);

    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_setsockopt (server_, ZMQ_PLAIN_PASSWORD, test_plain_password, 8));
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_setsockopt (server_, ZMQ_PLAIN_USERNAME, test_plain_username, 8));
}

void socket_config_plain_server (void *server_, void *server_secret_)
{
    LIBZMQ_UNUSED (server_secret_);

    int as_server = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_setsockopt (server_, ZMQ_PLAIN_SERVER, &as_server, sizeof (int)));
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (
      server_, ZMQ_ZAP_DOMAIN, test_zap_domain, strlen (test_zap_domain)));
}

//  CURVE specific functions

//  We'll generate random test keys at startup
char valid_client_public[41];
char valid_client_secret[41];
char valid_server_public[41];
char valid_server_secret[41];

void setup_testutil_security_curve ()
{
    //  Generate new keypairs for these tests
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_curve_keypair (valid_client_public, valid_client_secret));
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_curve_keypair (valid_server_public, valid_server_secret));
}

void socket_config_curve_server (void *server_, void *server_secret_)
{
    int as_server = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_setsockopt (server_, ZMQ_CURVE_SERVER, &as_server, sizeof (int)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_setsockopt (server_, ZMQ_CURVE_SECRETKEY, server_secret_, 41));
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (
      server_, ZMQ_ZAP_DOMAIN, test_zap_domain, strlen (test_zap_domain)));

#ifdef ZMQ_ZAP_ENFORCE_DOMAIN
    int required = 1;
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (server_, ZMQ_ZAP_ENFORCE_DOMAIN,
                                               &required, sizeof (int)));
#endif
}

struct curve_client_data_t
{
    const char *server_public;
    const char *client_public;
    const char *client_secret;
};

void socket_config_curve_client (void *client_, void *data_)
{
    curve_client_data_t *curve_client_data =
      static_cast<curve_client_data_t *> (data_);

    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (
      client_, ZMQ_CURVE_SERVERKEY, curve_client_data->server_public, 41));
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (
      client_, ZMQ_CURVE_PUBLICKEY, curve_client_data->client_public, 41));
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (
      client_, ZMQ_CURVE_SECRETKEY, curve_client_data->client_secret, 41));
}

//  --------------------------------------------------------------------------
//  This methods receives and validates ZAP requests (allowing or denying
//  each client connection).

enum zap_protocol_t
{
    zap_ok,
    // ZAP-compliant non-standard cases
    zap_status_temporary_failure,
    zap_status_internal_error,
    // ZAP protocol errors
    zap_wrong_version,
    zap_wrong_request_id,
    zap_status_invalid,
    zap_too_many_parts,
    zap_disconnect,
    zap_do_not_recv,
    zap_do_not_send
};

void *zap_requests_handled;

void zap_handler_generic (zap_protocol_t zap_protocol_,
                          const char *expected_routing_id_ = "IDENT")
{
    void *control = zmq_socket (get_test_context (), ZMQ_REQ);
    TEST_ASSERT_NOT_NULL (control);
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_connect (control, "inproc://handler-control"));

    void *handler = zmq_socket (get_test_context (), ZMQ_REP);
    TEST_ASSERT_NOT_NULL (handler);
    TEST_ASSERT_SUCCESS_ERRNO (zmq_bind (handler, "inproc://zeromq.zap.01"));

    //  Signal main thread that we are ready
    send_string_expect_success (control, "GO", 0);

    zmq_pollitem_t items[] = {
      {control, 0, ZMQ_POLLIN, 0},
      {handler, 0, ZMQ_POLLIN, 0},
    };

    // if ordered not to receive the request, ignore the second poll item
    const int numitems = (zap_protocol_ == zap_do_not_recv) ? 1 : 2;

    //  Process ZAP requests forever
    while (zmq_poll (items, numitems, -1) >= 0) {
        if (items[0].revents & ZMQ_POLLIN) {
            recv_string_expect_success (control, "STOP", 0);
            break; //  Terminating - main thread signal
        }
        if (!(items[1].revents & ZMQ_POLLIN))
            continue;

        char *version = s_recv (handler);
        if (!version)
            break; //  Terminating - peer's socket closed
        if (zap_protocol_ == zap_disconnect) {
            free (version);
            break;
        }

        char *sequence = s_recv (handler);
        char *domain = s_recv (handler);
        char *address = s_recv (handler);
        char *routing_id = s_recv (handler);
        char *mechanism = s_recv (handler);
        bool authentication_succeeded = false;
        if (streq (mechanism, "CURVE")) {
            uint8_t client_key[32];
            TEST_ASSERT_EQUAL_INT (32, TEST_ASSERT_SUCCESS_ERRNO (zmq_recv (
                                         handler, client_key, 32, 0)));

            char client_key_text[41];
            zmq_z85_encode (client_key_text, client_key, 32);

            authentication_succeeded =
              streq (client_key_text, valid_client_public);
        } else if (streq (mechanism, "PLAIN")) {
            char client_username[32];
            int size = TEST_ASSERT_SUCCESS_ERRNO (
              zmq_recv (handler, client_username, 32, 0));
            client_username[size] = 0;

            char client_password[32];
            size = TEST_ASSERT_SUCCESS_ERRNO (
              zmq_recv (handler, client_password, 32, 0));
            client_password[size] = 0;

            authentication_succeeded =
              streq (test_plain_username, client_username)
              && streq (test_plain_password, client_password);
        } else if (streq (mechanism, "NULL")) {
            authentication_succeeded = true;
        } else {
            char msg[128];
            printf ("Unsupported mechanism: %s\n", mechanism);
            TEST_FAIL_MESSAGE (msg);
        }

        TEST_ASSERT_EQUAL_STRING ("1.0", version);
        TEST_ASSERT_EQUAL_STRING (expected_routing_id_, routing_id);

        s_sendmore (handler, zap_protocol_ == zap_wrong_version
                               ? "invalid_version"
                               : version);
        s_sendmore (handler, zap_protocol_ == zap_wrong_request_id
                               ? "invalid_request_id"
                               : sequence);

        if (authentication_succeeded) {
            const char *status_code;
            switch (zap_protocol_) {
                case zap_status_internal_error:
                    status_code = "500";
                    break;
                case zap_status_temporary_failure:
                    status_code = "300";
                    break;
                case zap_status_invalid:
                    status_code = "invalid_status";
                    break;
                default:
                    status_code = "200";
            }
            s_sendmore (handler, status_code);
            s_sendmore (handler, "OK");
            s_sendmore (handler, "anonymous");
            if (zap_protocol_ == zap_too_many_parts) {
                s_sendmore (handler, "");
            }
            if (zap_protocol_ != zap_do_not_send)
                s_send (handler, "");
        } else {
            s_sendmore (handler, "400");
            s_sendmore (handler, "Invalid client public key");
            s_sendmore (handler, "");
            if (zap_protocol_ != zap_do_not_send)
                s_send (handler, "");
        }
        free (version);
        free (sequence);
        free (domain);
        free (address);
        free (routing_id);
        free (mechanism);

        zmq_atomic_counter_inc (zap_requests_handled);
    }
    TEST_ASSERT_SUCCESS_ERRNO (zmq_unbind (handler, "inproc://zeromq.zap.01"));
    close_zero_linger (handler);

    if (zap_protocol_ != zap_disconnect) {
        send_string_expect_success (control, "STOPPED", 0);
    }
    close_zero_linger (control);
}

void zap_handler (void * /*unused_*/)
{
    zap_handler_generic (zap_ok);
}

//  Security-specific monitor event utilities

// assert_* are macros rather than functions, to allow assertion failures be
// attributed to the causing source code line
#define assert_no_more_monitor_events_with_timeout(monitor, timeout)                  \
    {                                                                                 \
        int event_count = 0;                                                          \
        int event, err;                                                               \
        while ((event = get_monitor_event_with_timeout ((monitor), &err, NULL,        \
                                                        (timeout)))                   \
               != -1) {                                                               \
            if (event == ZMQ_EVENT_HANDSHAKE_FAILED_NO_DETAIL                         \
                && (err == EPIPE || err == ECONNRESET                                 \
                    || err == ECONNABORTED)) {                                        \
                fprintf (stderr,                                                      \
                         "Ignored event (skipping any further events): %x "           \
                         "(err = %i == %s)\n",                                        \
                         event, err, zmq_strerror (err));                             \
                continue;                                                             \
            }                                                                         \
            ++event_count;                                                            \
            /* TODO write this into a buffer and attach to the assertion msg below */ \
            print_unexpected_event_stderr (event, err, 0, 0);                         \
        }                                                                             \
        TEST_ASSERT_EQUAL_INT (0, event_count);                                       \
    }

void setup_handshake_socket_monitor (void *server_,
                                     void **server_mon_,
                                     const char *monitor_endpoint_)
{
    //  Monitor handshake events on the server
    TEST_ASSERT_SUCCESS_ERRNO (zmq_socket_monitor (
      server_, monitor_endpoint_,
      ZMQ_EVENT_HANDSHAKE_SUCCEEDED | ZMQ_EVENT_HANDSHAKE_FAILED_NO_DETAIL
        | ZMQ_EVENT_HANDSHAKE_FAILED_AUTH
        | ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL));

    //  Create socket for collecting monitor events
    *server_mon_ = test_context_socket (ZMQ_PAIR);
    int linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_setsockopt (*server_mon_, ZMQ_LINGER, &linger, sizeof (linger)));

    //  Connect it to the inproc endpoints so they'll get events
    TEST_ASSERT_SUCCESS_ERRNO (zmq_connect (*server_mon_, monitor_endpoint_));
}

void setup_context_and_server_side (
  void **zap_control_,
  void **zap_thread_,
  void **server_,
  void **server_mon_,
  char *my_endpoint_,
  zmq_thread_fn zap_handler_ = &zap_handler,
  socket_config_fn socket_config_ = &socket_config_curve_server,
  void *socket_config_data_ = valid_server_secret,
  const char *routing_id_ = "IDENT")
{
    //  Spawn ZAP handler
    zap_requests_handled = zmq_atomic_counter_new ();
    TEST_ASSERT_NOT_NULL (zap_requests_handled);

    *zap_control_ = test_context_socket (ZMQ_REP);
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_bind (*zap_control_, "inproc://handler-control"));
    int linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_setsockopt (*zap_control_, ZMQ_LINGER, &linger, sizeof (linger)));

    if (zap_handler_ != NULL) {
        *zap_thread_ = zmq_threadstart (zap_handler_, NULL);

        recv_string_expect_success (*zap_control_, "GO", 0);
    } else
        *zap_thread_ = NULL;

    //  Server socket will accept connections
    *server_ = test_context_socket (ZMQ_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_setsockopt (*server_, ZMQ_LINGER, &linger, sizeof (linger)));

    socket_config_ (*server_, socket_config_data_);

    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (
      *server_, ZMQ_ROUTING_ID, routing_id_, strlen (routing_id_)));

    bind_loopback_ipv4 (*server_, my_endpoint_, MAX_SOCKET_STRING);

    const char server_monitor_endpoint[] = "inproc://monitor-server";
    setup_handshake_socket_monitor (*server_, server_mon_,
                                    server_monitor_endpoint);
}

void shutdown_context_and_server_side (void *zap_thread_,
                                       void *server_,
                                       void *server_mon_,
                                       void *zap_control_,
                                       bool zap_handler_stopped_ = false)
{
    if (zap_thread_ && !zap_handler_stopped_) {
        send_string_expect_success (zap_control_, "STOP", 0);
        recv_string_expect_success (zap_control_, "STOPPED", 0);
        TEST_ASSERT_SUCCESS_ERRNO (
          zmq_unbind (zap_control_, "inproc://handler-control"));
    }
    test_context_socket_close (zap_control_);
    test_context_socket_close (server_mon_);
    test_context_socket_close (server_);

    //  Wait until ZAP handler terminates
    if (zap_thread_)
        zmq_threadclose (zap_thread_);

    zmq_atomic_counter_destroy (&zap_requests_handled);
}

void *create_and_connect_client (char *my_endpoint_,
                                 socket_config_fn socket_config_,
                                 void *socket_config_data_,
                                 void **client_mon_ = NULL)
{
    void *client = test_context_socket (ZMQ_DEALER);

    socket_config_ (client, socket_config_data_);

    TEST_ASSERT_SUCCESS_ERRNO (zmq_connect (client, my_endpoint_));

    if (client_mon_) {
        setup_handshake_socket_monitor (client, client_mon_,
                                        "inproc://client-monitor");
    }

    return client;
}

void expect_new_client_bounce_fail (char *my_endpoint_,
                                    void *server_,
                                    socket_config_fn socket_config_,
                                    void *socket_config_data_,
                                    void **client_mon_ = NULL,
                                    int expected_client_event_ = 0,
                                    int expected_client_value_ = 0)
{
    void *my_client_mon;
    TEST_ASSERT_TRUE (client_mon_ == NULL || expected_client_event_ == 0);
    if (expected_client_event_ != 0)
        client_mon_ = &my_client_mon;
    void *client = create_and_connect_client (my_endpoint_, socket_config_,
                                              socket_config_data_, client_mon_);
    expect_bounce_fail (server_, client);

    if (expected_client_event_ != 0) {
        int events_received = 0;
        events_received = expect_monitor_event_multiple (
          my_client_mon, expected_client_event_, expected_client_value_, false);

        TEST_ASSERT_EQUAL_INT (1, events_received);

        test_context_socket_close (my_client_mon);
    }

    test_context_socket_close_zero_linger (client);
}

#endif
