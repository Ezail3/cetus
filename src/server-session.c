/* $%BEGINLICENSE%$
 Copyright (c) 2007, 2012, Oracle and/or its affiliates. All rights reserved.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License as
 published by the Free Software Foundation; version 2 of the
 License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA

 $%ENDLICENSE%$ */

#include "server-session.h"

#include <errno.h>
#include <sys/ioctl.h>
#include <mysql.h>
#include <mysqld_error.h>

#include "cetus-util.h"
#include "chassis-event.h"
#include "glib-ext.h"
#include "network-mysqld-proto.h"
#include "resultset_merge.h"
#include "plugin-common.h"

void
server_session_free(server_session_t *pmd)
{
    if (pmd) {
        if (pmd->server != NULL) {
            network_socket_free(pmd->server);
        }

        pmd->sql = NULL;

        g_free(pmd);
    }
}

void
server_sess_wait_for_event(server_session_t *pmd, short ev_type, struct timeval *timeout)
{
    event_set(&(pmd->server->event), pmd->server->fd, ev_type, server_session_con_handler, pmd);
    chassis_event_add_with_timeout(pmd->con->srv, &(pmd->server->event), timeout);
    pmd->server->is_waiting = 1;
}

static void
do_tcp_stream_after_recv_resp(network_mysqld_con *con, server_session_t *pmd)
{
    merge_parameters_t *data = con->data;
    if (data->candidates[pmd->index] == NULL) {
        data->candidates[pmd->index] = g_queue_peek_head_link(pmd->server->recv_queue->chunks);
    }

    if (con->server_closed) {
        if (con->servers) {
            remove_mul_server_recv_packets(con);
            proxy_put_shard_conn_to_pool(con);
        }
        g_message("%s: server conn is closed", G_STRLOC);
        network_mysqld_con_send_error_full(con->client, C("server closed prematurely"), ER_CETUS_RESULT_MERGE, "HY000");
        GList *err = con->client->send_queue->chunks->tail;
        GString *err_packet = err->data;
        merge_parameters_t *data = con->data;

        network_mysqld_proto_set_packet_id(err_packet, data->pkt_count + 1);
        con->state = ST_SEND_QUERY_RESULT;
        network_mysqld_con_handle(-1, 0, con);
        return;
    }

    if (!con->resp_too_long) {
        if (con->num_pending_servers == 0) {
            g_debug("%s: merge over", G_STRLOC);
            if (callback_merge(con, con->data, 1) == RM_FAIL) {
                network_queue_clear(con->client->send_queue);
                network_mysqld_con_send_error_full(con->client, C("merge failed"), ER_CETUS_RESULT_MERGE, "HY000");
            }
            con->state = ST_SEND_QUERY_RESULT;
            network_mysqld_con_handle(-1, 0, con);
        } else {
            if (callback_merge(con, con->data, 0) == RM_FAIL) {
                network_queue_clear(con->client->send_queue);
                network_mysqld_con_send_error_full(con->client, C("merge failed"), ER_CETUS_RESULT_MERGE, "HY000");
                con->state = ST_SEND_QUERY_RESULT;
                network_mysqld_con_handle(-1, 0, con);
            }
        }
    } else {
        g_message("%s: resp too long:%p, src port:%s, sql:%s",
                  G_STRLOC, con, con->client->src->name->str, con->orig_sql->str);
        network_mysqld_con_send_error_full(con->client, C("response too long for proxy"), ER_CETUS_LONG_RESP, "HY000");
        GList *err = con->client->send_queue->chunks->tail;
        GString *err_packet = err->data;
        merge_parameters_t *data = con->data;

        network_mysqld_proto_set_packet_id(err_packet, data->pkt_count + 1);
        con->state = ST_SEND_QUERY_RESULT;
        network_mysqld_con_handle(-1, 0, con);
    }
}

static int
process_read_event(network_mysqld_con *con, server_session_t *pmd)
{
    network_socket *sock = pmd->server;

    if (con->partially_merged) {
        pmd->state = NET_RW_STATE_READ;
    }

    int i, b = -1;

    if ((i = ioctl(sock->fd, FIONREAD, &b))) {
        pmd->state = NET_RW_STATE_ERROR;
        g_message("%s:NET_RW_STATE_ERROR is set i:%d,b:%d", G_STRLOC, i, b);
    } else if (b >= 0) {
        sock->to_read = b;
        sock->resp_len += b;
        if (b == 0) {
            if (con->dist_tran) {
                record_xa_log_for_mending(con, sock);
                sock->unavailable = 1;
                pmd->state = NET_RW_STATE_FINISHED;
                con->dist_tran_failed = 1;
                g_message("%s: b is zero, xid:%s, con:%p, xa state:%d",
                          G_STRLOC, con->xid_str, con, con->dist_tran_state);
            }
            g_debug("%s:b is zero, socket fd:%d, con:%p", G_STRLOC, sock->fd, con);
        } else {
            con->resp_cnt &= (1 << pmd->index);
        }
    } else {
        pmd->state = NET_RW_STATE_ERROR;
        g_message("%s:NET_RW_STATE_ERROR is set i:%d,b:%d", G_STRLOC, i, b);
    }

    if (b > 0) {
        return 1;
    } else {
        return 0;
    }
}

static void
process_timeout_event(network_mysqld_con *con, server_session_t *pmd)
{
    network_socket *sock = pmd->server;
    if (con->dist_tran) {
        record_xa_log_for_mending(con, sock);
        g_message("%s: EV_TIMEOUT for con xid:%s", G_STRLOC, con->xid_str);
    }
    con->is_timeout = 1;
    pmd->state = NET_RW_STATE_FINISHED;
    g_message("%s:EV_TIMEOUT, set server timeout, con:%p", G_STRLOC, con);
}

static void
process_read_part_finished(network_mysqld_con *con, server_session_t *pmd)
{
    network_socket *sock = pmd->server;

    if (pmd->read_cal_flag == 0) {
        con->num_read_pending--;
        pmd->read_cal_flag = 1;
    }

    g_debug("%s:part finished, num_read_pending:%d for con:%p, server fd:%d",
            G_STRLOC, con->num_read_pending, con, pmd->server->fd);
    if (!con->dist_tran_failed) {
        if (con->num_read_pending == 0) {
            if (!con->partially_merged) {
                network_mysqld_con_handle(-1, 0, con);
            } else {
                do_tcp_stream_after_recv_resp(con, pmd);
            }
        }
    } else {
        if (con->num_pending_servers == 0) {
            sock->is_closed = 1;
            network_mysqld_con_handle(-1, 0, con);
        }
    }
}

static void
process_read_finished(network_mysqld_con *con, server_session_t *pmd)
{
    network_socket *sock = pmd->server;

    if (pmd->read_cal_flag == 0) {
        con->num_read_pending--;
        pmd->read_cal_flag = 1;
    }

    con->num_pending_servers--;
    if (!con->dist_tran_failed) {
        if (con->num_pending_servers == 0) {
            network_mysqld_con_handle(-1, 0, con);
        }
    } else {
        if (con->num_pending_servers == 0) {
            sock->is_closed = 1;
            network_mysqld_con_handle(-1, 0, con);
        }
    }
}

static void
process_write_event(network_mysqld_con *con, server_session_t *pmd)
{
    network_socket *sock = pmd->server;

    switch (network_mysqld_write(con->srv, sock)) {
    case NETWORK_SOCKET_SUCCESS:
        con->num_pending_servers++;
        con->num_servers_visited++;
        con->num_read_pending++;
        pmd->read_cal_flag = 0;
        con->num_write_pending--;
        pmd->state = NET_RW_STATE_READ;
        if (con->num_write_pending == 0) {
            network_mysqld_con_handle(-1, 0, con);
        }
        break;
    case NETWORK_SOCKET_WAIT_FOR_EVENT:
        pmd->state = NET_RW_STATE_WRITE;
        g_debug("%s:write wait for con:%p", G_STRLOC, con);
        server_sess_wait_for_event(pmd, EV_WRITE, &con->write_timeout);
        break;
    default:
    {
        char *msg = "write error";
        con->state = ST_SEND_QUERY_RESULT;
        con->server_to_be_closed = 1;
        g_warning("%s:write error for con:%p", G_STRLOC, con);
        network_mysqld_con_send_error_full(con->client, L(msg), ER_NET_ERROR_ON_WRITE, "08S01");
        network_mysqld_con_handle(-1, 0, con);
        break;
    }
    }

}

static int
process_read_server(network_mysqld_con *con, server_session_t *pmd)
{
    int is_finished = 0;
    int ret = 0;

    network_socket *sock = pmd->server;

    if (sock->to_read == 0) {
        sock->is_closed = 1;
        is_finished = 1;
        ret = NETWORK_SOCKET_SUCCESS;
        con->server_to_be_closed = 1;
        con->server_closed = 1;
    } else {
        ret = network_mysqld_read_mul_packets(con->srv, con, sock, &is_finished);
    }

    switch (ret) {
    case NETWORK_SOCKET_SUCCESS:
        g_debug("%s: resp len:%d, attr adj state:%d", G_STRLOC, (int)sock->resp_len, con->attr_adj_state);

        if (is_finished) {
            set_conn_attr(con, pmd->server);
            pmd->state = NET_RW_STATE_FINISHED;
            pmd->server->is_read_finished = 1;
            pmd->server->is_waiting = 0;
        } else {
            if (con->candidate_tcp_streamed && con->num_servers_visited > 1) {
                set_conn_attr(con, pmd->server);
                pmd->state = NET_RW_STATE_PART_FINISHED;
                g_debug("%s:tcp stream is true for:%p", G_STRLOC, con);
            } else {
                if (con->candidate_tcp_streamed) {
                    GString *packet;
                    while ((packet = g_queue_pop_head(pmd->server->recv_queue->chunks)) != NULL) {
                        network_mysqld_queue_append_raw(con->client, con->client->send_queue, packet);
                    }

                    g_debug("%s: send_part_content_to_client", G_STRLOC);

                    send_part_content_to_client(con);
                }
                server_sess_wait_for_event(pmd, EV_READ, &con->read_timeout);
            }
        }
        break;
    case NETWORK_SOCKET_WAIT_FOR_EVENT:
        if (sock->resp_len > con->srv->max_resp_len) {
            pmd->state = NET_RW_STATE_FINISHED;
            con->server_to_be_closed = 1;
            con->resp_too_long = 1;
            g_debug("%s: resp len is too long for query:%s", G_STRLOC, con->orig_sql->str);
            if (!con->candidate_tcp_streamed || con->servers->len == 1) {
                g_message("%s: resp too long:%p, src port:%s, sql:%s",
                          G_STRLOC, con, con->client->src->name->str, con->orig_sql->str);
                network_mysqld_con_send_error_full(con->client,
                                                   C("response too long for proxy"), ER_CETUS_LONG_RESP, "HY000");
                con->state = ST_SEND_QUERY_RESULT;
                con->resultset_is_finished = TRUE;
                network_mysqld_con_handle(-1, 0, con);
                return 0;
            }

            break;
        } else {

            if (con->candidate_tcp_streamed && sock->max_header_size_reached && con->num_servers_visited > 1) {
                g_debug("%s: set NET_RW_STATE_PART_FINISHED here", G_STRLOC);
                pmd->state = NET_RW_STATE_PART_FINISHED;
                if (con->partially_merged) {
                    do_tcp_stream_after_recv_resp(con, pmd);
                }
                break;
            } else {
                if (con->num_servers_visited == 1 && sock->max_header_size_reached && con->candidate_tcp_streamed) {
                    GString *packet;
                    while ((packet = g_queue_pop_head(pmd->server->recv_queue->chunks)) != NULL) {
                        network_mysqld_queue_append_raw(con->client, con->client->send_queue, packet);
                    }
                    g_debug("%s: send_part_content_to_client", G_STRLOC);
                    send_part_content_to_client(con);
                }
                server_sess_wait_for_event(pmd, EV_READ, &con->read_timeout);
                return 0;
            }
        }
    case NETWORK_SOCKET_ERROR:
        pmd->state = NET_RW_STATE_ERROR;
        g_debug("%s: NET_RW_STATE_ERROR is set here", G_STRLOC);
        break;
    }

    return 1;
}

static void
process_after_read(network_mysqld_con *con, server_session_t *pmd)
{
    con->num_pending_servers--;
    if (pmd->read_cal_flag == 0) {
        con->num_read_pending--;
        pmd->read_cal_flag = 1;
    }
    if (!con->dist_tran_failed) {
        if (con->partially_merged) {
            do_tcp_stream_after_recv_resp(con, pmd);
        } else {
            if (con->num_pending_servers == 0 || (con->num_read_pending == 0)) {
                if (con->server_to_be_closed) {
                    g_message("%s:con:%p, state:%d", G_STRLOC, con, con->state);
                }

                network_mysqld_con_handle(-1, 0, con);
            }
        }
    } else {
        if (con->num_pending_servers == 0) {
            network_mysqld_con_handle(-1, 0, con);
        }
    }
}

void
server_session_con_handler(int event_fd, short events, void *user_data)
{
    server_session_t *pmd = (server_session_t *)user_data;
    network_mysqld_con *con = pmd->con;
    network_socket *sock = pmd->server;

    pmd->read_cal_flag = 0;
    g_debug("%s:visit server_session_con_handler, state:%d for con:%p, fd:%d", G_STRLOC, con->state, con, sock->fd);

    sock->is_waiting = 0;

    if (events == EV_READ) {
        if (con->srv->query_cache_enabled) {
            g_debug("%s: check if query can be cached, attr state:%d", G_STRLOC, con->attr_adj_state);
            if (con->is_read_ro_server_allowed && con->attr_adj_state == ATTR_START) {
                if (con->query_cache_judged == 0) {
                    do_check_qeury_cache(con);
                }
            }
        }

        process_read_event(con, pmd);
    } else if (events == EV_TIMEOUT) {
        process_timeout_event(con, pmd);
    }

    do {

        switch (pmd->state) {
        case NET_RW_STATE_PART_FINISHED:
            if (events == EV_READ || events == EV_TIMEOUT) {
                process_read_part_finished(con, pmd);
            }
            return;
        case NET_RW_STATE_ERROR:
        case NET_RW_STATE_FINISHED:
            if (events == EV_READ || events == EV_TIMEOUT) {
                process_read_finished(con, pmd);
            }
            return;
        case NET_RW_STATE_WRITE:
            process_write_event(con, pmd);
            return;
        case NET_RW_STATE_READ:
            if (!process_read_server(con, pmd)) {
                return;
            }
            break;
        default:
            break;
        }

        if (pmd->state == NET_RW_STATE_FINISHED) {
            if (events == EV_READ) {
                process_after_read(con, pmd);
            }
            return;
        }

    } while (pmd->state != NET_RW_STATE_NONE);

    g_debug("%s:server_session_con_handler over for con:%p, pmd:%d", G_STRLOC, con, pmd->index);
}
