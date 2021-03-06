/*
 * Promex is a Prometheus exporter for HAProxy
 *
 * It is highly inspired by the official Prometheus exporter.
 * See: https://github.com/prometheus/haproxy_exporter
 *
 * Copyright 2019 Christopher Faulet <cfaulet@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <haproxy/action-t.h>
#include <haproxy/api.h>
#include <haproxy/applet.h>
#include <haproxy/backend.h>
#include <haproxy/cfgparse.h>
#include <haproxy/compression.h>
#include <haproxy/dns.h>
#include <haproxy/frontend.h>
#include <haproxy/global.h>
#include <haproxy/http.h>
#include <haproxy/http_htx.h>
#include <haproxy/htx.h>
#include <haproxy/list.h>
#include <haproxy/listener.h>
#include <haproxy/log.h>
#include <haproxy/pipe.h>
#include <haproxy/pool.h>
#include <haproxy/proxy.h>
#include <haproxy/sample.h>
#include <haproxy/server.h>
#include <haproxy/ssl_sock.h>
#include <haproxy/stats.h>
#include <haproxy/stream.h>
#include <haproxy/stream_interface.h>
#include <haproxy/task.h>
#include <haproxy/version.h>

/* Prometheus exporter applet states (appctx->st0) */
enum {
        PROMEX_ST_INIT = 0,  /* initialized */
        PROMEX_ST_HEAD,      /* send headers before dump */
        PROMEX_ST_DUMP,      /* dumping stats */
        PROMEX_ST_DONE,      /* finished */
        PROMEX_ST_END,       /* treatment terminated */
};

/* Prometheus exporter dumper states (appctx->st1) */
enum {
        PROMEX_DUMPER_INIT = 0, /* initialized */
        PROMEX_DUMPER_GLOBAL,   /* dump metrics of globals */
        PROMEX_DUMPER_FRONT,    /* dump metrics of frontend proxies */
        PROMEX_DUMPER_BACK,     /* dump metrics of backend proxies */
        PROMEX_DUMPER_LI,       /* dump metrics of listeners */
        PROMEX_DUMPER_SRV,      /* dump metrics of servers */
	PROMEX_DUMPER_DONE,     /* finished */
};

/* Prometheus exporter flags (appctx->ctx.stats.flags) */
#define PROMEX_FL_METRIC_HDR    0x00000001
#define PROMEX_FL_INFO_METRIC   0x00000002
#define PROMEX_FL_FRONT_METRIC  0x00000004
#define PROMEX_FL_BACK_METRIC   0x00000008
#define PROMEX_FL_SRV_METRIC    0x00000010
#define PROMEX_FL_SCOPE_GLOBAL  0x00000020
#define PROMEX_FL_SCOPE_FRONT   0x00000040
#define PROMEX_FL_SCOPE_BACK    0x00000080
#define PROMEX_FL_SCOPE_SERVER  0x00000100
#define PROMEX_FL_NO_MAINT_SRV  0x00000200

#define PROMEX_FL_SCOPE_ALL (PROMEX_FL_SCOPE_GLOBAL|PROMEX_FL_SCOPE_FRONT|PROMEX_FL_SCOPE_BACK|PROMEX_FL_SCOPE_SERVER)

/* Promtheus metric type (gauge or counter) */
enum promex_mt_type {
	PROMEX_MT_GAUGE   = 1,
	PROMEX_MT_COUNTER = 2,
};

/* The max length for metrics name. It is a hard limit but it should be
 * enough.
 */
#define PROMEX_MAX_NAME_LEN 128

/* The expected max length for a metric dump, including its header lines. It is
 * just a soft limit to avoid extra work. We don't try to dump a metric if less
 * than this size is available in the HTX.
 */
#define PROMEX_MAX_METRIC_LENGTH 512

/* Some labels for build_info */
#define PROMEX_VERSION_LABEL "version=\"" HAPROXY_VERSION "\""
#define PROMEX_BUILDINFO_LABEL PROMEX_VERSION_LABEL

/* Describe a prometheus metric */
struct promex_metric {
	const struct ist    n;      /* The metric name */
	enum promex_mt_type type;   /* The metric type (gauge or counter) */
	unsigned int        flags;  /* PROMEX_FL_* flags */
};

/* Global metrics  */
const struct promex_metric promex_global_metrics[INF_TOTAL_FIELDS] = {
	//[INF_NAME]                           ignored
	//[INF_VERSION],                       ignored
	//[INF_RELEASE_DATE]                   ignored
	[INF_BUILD_INFO]                     = { .n = IST("build_info"),                    .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_NBTHREAD]                       = { .n = IST("nbthread"),                      .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_NBPROC]                         = { .n = IST("nbproc"),                        .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_PROCESS_NUM]                    = { .n = IST("relative_process_id"),           .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	//[INF_PID]                            ignored
	//[INF_UPTIME]                         ignored
	[INF_UPTIME_SEC]                     = { .n = IST("uptime_seconds"),                .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_START_TIME_SEC]                 = { .n = IST("start_time_seconds"),            .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_MEMMAX_BYTES]                   = { .n = IST("max_memory_bytes"),              .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_POOL_ALLOC_BYTES]               = { .n = IST("pool_allocated_bytes"),          .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_POOL_USED_BYTES]                = { .n = IST("pool_used_bytes"),               .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_POOL_FAILED]                    = { .n = IST("pool_failures_total"),           .type = PROMEX_MT_COUNTER, .flags = PROMEX_FL_INFO_METRIC },
	[INF_ULIMIT_N]                       = { .n = IST("max_fds"),                       .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_MAXSOCK]                        = { .n = IST("max_sockets"),                   .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_MAXCONN]                        = { .n = IST("max_connections"),               .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_HARD_MAXCONN]                   = { .n = IST("hard_max_connections"),          .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_CURR_CONN]                      = { .n = IST("current_connections"),           .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_CUM_CONN]                       = { .n = IST("connections_total"),             .type = PROMEX_MT_COUNTER, .flags = PROMEX_FL_INFO_METRIC },
	[INF_CUM_REQ]                        = { .n = IST("requests_total"),                .type = PROMEX_MT_COUNTER, .flags = PROMEX_FL_INFO_METRIC },
	[INF_MAX_SSL_CONNS]                  = { .n = IST("max_ssl_connections"),           .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_CURR_SSL_CONNS]                 = { .n = IST("current_ssl_connections"),       .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_CUM_SSL_CONNS]                  = { .n = IST("ssl_connections_total"),         .type = PROMEX_MT_COUNTER, .flags = PROMEX_FL_INFO_METRIC },
	[INF_MAXPIPES]                       = { .n = IST("max_pipes"),                     .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_PIPES_USED]                     = { .n = IST("pipes_used_total"),              .type = PROMEX_MT_COUNTER, .flags = PROMEX_FL_INFO_METRIC },
	[INF_PIPES_FREE]                     = { .n = IST("pipes_free_total"),              .type = PROMEX_MT_COUNTER, .flags = PROMEX_FL_INFO_METRIC },
	[INF_CONN_RATE]                      = { .n = IST("current_connection_rate"),       .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_CONN_RATE_LIMIT]                = { .n = IST("limit_connection_rate"),         .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_MAX_CONN_RATE]                  = { .n = IST("max_connection_rate"),           .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_SESS_RATE]                      = { .n = IST("current_session_rate"),          .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_SESS_RATE_LIMIT]                = { .n = IST("limit_session_rate"),            .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_MAX_SESS_RATE]                  = { .n = IST("max_session_rate"),              .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_SSL_RATE]                       = { .n = IST("current_ssl_rate"),              .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_SSL_RATE_LIMIT]                 = { .n = IST("limit_ssl_rate"),                .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_MAX_SSL_RATE]                   = { .n = IST("max_ssl_rate"),                  .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_SSL_FRONTEND_KEY_RATE]          = { .n = IST("current_frontend_ssl_key_rate"), .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_SSL_FRONTEND_MAX_KEY_RATE]      = { .n = IST("max_frontend_ssl_key_rate"),     .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_SSL_FRONTEND_SESSION_REUSE_PCT] = { .n = IST("frontend_ssl_reuse"),            .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_SSL_BACKEND_KEY_RATE]           = { .n = IST("current_backend_ssl_key_rate"),  .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_SSL_BACKEND_MAX_KEY_RATE]       = { .n = IST("max_backend_ssl_key_rate"),      .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_SSL_CACHE_LOOKUPS]              = { .n = IST("ssl_cache_lookups_total"),       .type = PROMEX_MT_COUNTER, .flags = PROMEX_FL_INFO_METRIC },
	[INF_SSL_CACHE_MISSES]               = { .n = IST("ssl_cache_misses_total"),        .type = PROMEX_MT_COUNTER, .flags = PROMEX_FL_INFO_METRIC },
	[INF_COMPRESS_BPS_IN]                = { .n = IST("http_comp_bytes_in_total"),      .type = PROMEX_MT_COUNTER, .flags = PROMEX_FL_INFO_METRIC },
	[INF_COMPRESS_BPS_OUT]               = { .n = IST("http_comp_bytes_out_total"),     .type = PROMEX_MT_COUNTER, .flags = PROMEX_FL_INFO_METRIC },
	[INF_COMPRESS_BPS_RATE_LIM]          = { .n = IST("limit_http_comp"),               .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_ZLIB_MEM_USAGE]                 = { .n = IST("current_zlib_memory"),           .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_MAX_ZLIB_MEM_USAGE]             = { .n = IST("max_zlib_memory"),               .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_TASKS]                          = { .n = IST("current_tasks"),                 .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_RUN_QUEUE]                      = { .n = IST("current_run_queue"),             .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_IDLE_PCT]                       = { .n = IST("idle_time_percent"),             .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	//[INF_NODE]                           ignored
	//[INF_DESCRIPTION]                    ignored
	[INF_STOPPING]                       = { .n = IST("stopping"),                      .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_JOBS]                           = { .n = IST("jobs"),                          .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_UNSTOPPABLE_JOBS]               = { .n = IST("unstoppable_jobs"),              .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_LISTENERS]                      = { .n = IST("listeners"),                     .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_ACTIVE_PEERS]                   = { .n = IST("active_peers"),                  .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_CONNECTED_PEERS]                = { .n = IST("connected_peers"),               .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_DROPPED_LOGS]                   = { .n = IST("dropped_logs_total"),            .type = PROMEX_MT_COUNTER, .flags = PROMEX_FL_INFO_METRIC },
	[INF_BUSY_POLLING]                   = { .n = IST("busy_polling_enabled"),          .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	[INF_FAILED_RESOLUTIONS]             = { .n = IST("failed_resolutions"),            .type = PROMEX_MT_COUNTER, .flags = PROMEX_FL_INFO_METRIC },
	[INF_TOTAL_BYTES_OUT]                = { .n = IST("bytes_out_total"),               .type = PROMEX_MT_COUNTER, .flags = PROMEX_FL_INFO_METRIC },
	[INF_TOTAL_SPLICED_BYTES_OUT]        = { .n = IST("spliced_bytes_out_total"),       .type = PROMEX_MT_COUNTER, .flags = PROMEX_FL_INFO_METRIC },
	[INF_BYTES_OUT_RATE]                 = { .n = IST("bytes_out_rate"),                .type = PROMEX_MT_GAUGE,   .flags = PROMEX_FL_INFO_METRIC },
	//[INF_DEBUG_COMMANDS_ISSUED]          ignored
};

/* frontend/backend/server fields */
const struct promex_metric promex_st_metrics[ST_F_TOTAL_FIELDS] = {
	//[ST_F_PXNAME]         ignored
	//[ST_F_SVNAME]         ignored
	[ST_F_QCUR]           = { .n = IST("current_queue"),                    .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_QMAX]           = { .n = IST("max_queue"),                        .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_SCUR]           = { .n = IST("current_sessions"),                 .type = PROMEX_MT_GAUGE,    .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_SMAX]           = { .n = IST("max_sessions"),                     .type = PROMEX_MT_GAUGE,    .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_SLIM]           = { .n = IST("limit_sessions"),                   .type = PROMEX_MT_GAUGE,    .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_STOT]           = { .n = IST("sessions_total"),                   .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_BIN]            = { .n = IST("bytes_in_total"),                   .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_BOUT]           = { .n = IST("bytes_out_total"),                  .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_DREQ]           = { .n = IST("requests_denied_total"),            .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC                       ) },
	[ST_F_DRESP]          = { .n = IST("responses_denied_total"),           .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_EREQ]           = { .n = IST("request_errors_total"),             .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC                                               ) },
	[ST_F_ECON]           = { .n = IST("connection_errors_total"),          .type = PROMEX_MT_COUNTER,  .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_ERESP]          = { .n = IST("response_errors_total"),            .type = PROMEX_MT_COUNTER,  .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_WRETR]          = { .n = IST("retry_warnings_total"),             .type = PROMEX_MT_COUNTER,  .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_WREDIS]         = { .n = IST("redispatch_warnings_total"),        .type = PROMEX_MT_COUNTER,  .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_STATUS]         = { .n = IST("status"),                           .type = PROMEX_MT_GAUGE,    .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_WEIGHT]         = { .n = IST("weight"),                           .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_ACT]            = { .n = IST("active_servers"),                   .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC                       ) },
	[ST_F_BCK]            = { .n = IST("backup_servers"),                   .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC                       ) },
	[ST_F_CHKFAIL]        = { .n = IST("check_failures_total"),             .type = PROMEX_MT_COUNTER,  .flags = (                                                 PROMEX_FL_SRV_METRIC) },
	[ST_F_CHKDOWN]        = { .n = IST("check_up_down_total"),              .type = PROMEX_MT_COUNTER,  .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_LASTCHG]        = { .n = IST("check_last_change_seconds"),        .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_DOWNTIME]       = { .n = IST("downtime_seconds_total"),           .type = PROMEX_MT_COUNTER,  .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_QLIMIT]         = { .n = IST("queue_limit"),                      .type = PROMEX_MT_GAUGE,    .flags = (                                                 PROMEX_FL_SRV_METRIC) },
	//[ST_F_PID]            ignored
	//[ST_F_IID]            ignored
	//[ST_F_SID]            ignored
	[ST_F_THROTTLE]       = { .n = IST("current_throttle"),                 .type = PROMEX_MT_GAUGE,    .flags = (                                                 PROMEX_FL_SRV_METRIC) },
	[ST_F_LBTOT]          = { .n = IST("loadbalanced_total"),               .type = PROMEX_MT_COUNTER,  .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	//[ST_F_TRACKED]        ignored
	//[ST_F_TYPE]           ignored
	//[ST_F_RATE]           ignored
	[ST_F_RATE_LIM]       = { .n = IST("limit_session_rate"),               .type = PROMEX_MT_GAUGE,    .flags = (PROMEX_FL_FRONT_METRIC                                               ) },
	[ST_F_RATE_MAX]       = { .n = IST("max_session_rate"),                 .type = PROMEX_MT_GAUGE,    .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_CHECK_STATUS]   = { .n = IST("check_status"),                     .type = PROMEX_MT_GAUGE,    .flags = (                                                 PROMEX_FL_SRV_METRIC) },
	[ST_F_CHECK_CODE]     = { .n = IST("check_code"),                       .type = PROMEX_MT_GAUGE,    .flags = (                                                 PROMEX_FL_SRV_METRIC) },
	[ST_F_CHECK_DURATION] = { .n = IST("check_duration_seconds"),           .type = PROMEX_MT_GAUGE,    .flags = (                                                 PROMEX_FL_SRV_METRIC) },
	[ST_F_HRSP_1XX]       = { .n = IST("http_responses_total"),             .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_HRSP_2XX]       = { .n = IST("http_responses_total"),             .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_HRSP_3XX]       = { .n = IST("http_responses_total"),             .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_HRSP_4XX]       = { .n = IST("http_responses_total"),             .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_HRSP_5XX]       = { .n = IST("http_responses_total"),             .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_HRSP_OTHER]     = { .n = IST("http_responses_total"),             .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	//[ST_F_HANAFAIL]       ignored
	//[ST_F_REQ_RATE]       ignored
	[ST_F_REQ_RATE_MAX]   = { .n = IST("http_requests_rate_max"),           .type = PROMEX_MT_GAUGE,    .flags = (PROMEX_FL_FRONT_METRIC                                               ) },
	[ST_F_REQ_TOT]        = { .n = IST("http_requests_total"),              .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC                       ) },
	[ST_F_CLI_ABRT]       = { .n = IST("client_aborts_total"),              .type = PROMEX_MT_COUNTER,  .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_SRV_ABRT]       = { .n = IST("server_aborts_total"),              .type = PROMEX_MT_COUNTER,  .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_COMP_IN]        = { .n = IST("http_comp_bytes_in_total"),         .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC                       ) },
	[ST_F_COMP_OUT]       = { .n = IST("http_comp_bytes_out_total"),        .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC                       ) },
	[ST_F_COMP_BYP]       = { .n = IST("http_comp_bytes_bypassed_total"),   .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC                       ) },
	[ST_F_COMP_RSP]       = { .n = IST("http_comp_responses_total"),        .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC                       ) },
	[ST_F_LASTSESS]       = { .n = IST("last_session_seconds"),             .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	//[ST_F_LAST_CHK]       ignroed
	//[ST_F_LAST_AGT]       ignored
	[ST_F_QTIME]          = { .n = IST("queue_time_average_seconds"),       .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_CTIME]          = { .n = IST("connect_time_average_seconds"),     .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_RTIME]          = { .n = IST("response_time_average_seconds"),    .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_TTIME]          = { .n = IST("total_time_average_seconds"),       .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	//[ST_F_AGENT_STATUS]   ignored
	//[ST_F_AGENT_CODE]     ignored
	//[ST_F_AGENT_DURATION] ignored
	//[ST_F_CHECK_DESC]     ignored
	//[ST_F_AGENT_DESC]     ignored
	//[ST_F_CHECK_RISE]     ignored
	//[ST_F_CHECK_FALL]     ignored
	//[ST_F_CHECK_HEALTH]   ignored
	//[ST_F_AGENT_RISE]     ignored
	//[ST_F_AGENT_FALL]     ignored
	//[ST_F_AGENT_HEALTH]   ignored
	//[ST_F_ADDR]           ignored
	//[ST_F_COOKIE]         ignored
	//[ST_F_MODE]           ignored
	//[ST_F_ALGO]           ignored
	//[ST_F_CONN_RATE]      ignored
	[ST_F_CONN_RATE_MAX]  = { .n = IST("connections_rate_max"),             .type = PROMEX_MT_GAUGE,    .flags = (PROMEX_FL_FRONT_METRIC                                               ) },
	[ST_F_CONN_TOT]       = { .n = IST("connections_total"),                .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC                                               ) },
	[ST_F_INTERCEPTED]    = { .n = IST("intercepted_requests_total"),       .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC                                               ) },
	[ST_F_DCON]           = { .n = IST("denied_connections_total"),         .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC                                               ) },
	[ST_F_DSES]           = { .n = IST("denied_sessions_total"),            .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC                                               ) },
	[ST_F_WREW]           = { .n = IST("failed_header_rewriting_total"),    .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_CONNECT]        = { .n = IST("connection_attempts_total"),        .type = PROMEX_MT_COUNTER,  .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_REUSE]          = { .n = IST("connection_reuses_total"),          .type = PROMEX_MT_COUNTER,  .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_CACHE_LOOKUPS]  = { .n = IST("http_cache_lookups_total"),         .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC                       ) },
	[ST_F_CACHE_HITS]     = { .n = IST("http_cache_hits_total"),            .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC                       ) },
	[ST_F_SRV_ICUR]       = { .n = IST("idle_connections_current"),         .type = PROMEX_MT_GAUGE,    .flags = (                                                 PROMEX_FL_SRV_METRIC) },
	[ST_F_SRV_ILIM]       = { .n = IST("idle_connections_limit"),           .type = PROMEX_MT_GAUGE,    .flags = (                                                 PROMEX_FL_SRV_METRIC) },
	[ST_F_QT_MAX]         = { .n = IST("max_queue_time_seconds"),           .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_CT_MAX]         = { .n = IST("max_connect_time_seconds"),         .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_RT_MAX]         = { .n = IST("max_response_time_seconds"),        .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_TT_MAX]         = { .n = IST("max_total_time_seconds"),           .type = PROMEX_MT_GAUGE,    .flags = (                         PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_EINT]           = { .n = IST("internal_errors_total"),            .type = PROMEX_MT_COUNTER,  .flags = (PROMEX_FL_FRONT_METRIC | PROMEX_FL_BACK_METRIC | PROMEX_FL_SRV_METRIC) },
	[ST_F_IDLE_CONN_CUR]  = { .n = IST("unsafe_idle_connections_current"),  .type = PROMEX_MT_GAUGE,    .flags = (                                                 PROMEX_FL_SRV_METRIC) },
	[ST_F_SAFE_CONN_CUR]=   { .n = IST("safe_idle_connections_current"),    .type = PROMEX_MT_GAUGE,    .flags = (                                                 PROMEX_FL_SRV_METRIC) },
	[ST_F_USED_CONN_CUR]  = { .n = IST("used_connections_current"),         .type = PROMEX_MT_GAUGE,    .flags = (                                                 PROMEX_FL_SRV_METRIC) },
	[ST_F_NEED_CONN_EST]  = { .n = IST("need_connections_current"),         .type = PROMEX_MT_GAUGE,    .flags = (                                                 PROMEX_FL_SRV_METRIC) },
};

/* Description of all stats fields */
const struct ist promex_st_metric_desc[ST_F_TOTAL_FIELDS] = {
	[ST_F_PXNAME]         = IST("The proxy name."),
	[ST_F_SVNAME]         = IST("The service name (FRONTEND for frontend, BACKEND for backend, any name for server/listener)."),
	[ST_F_QCUR]           = IST("Current number of queued requests."),
	[ST_F_QMAX]           = IST("Maximum observed number of queued requests."),
	[ST_F_SCUR]           = IST("Current number of active sessions."),
	[ST_F_SMAX]           = IST("Maximum observed number of active sessions."),
	[ST_F_SLIM]           = IST("Configured session limit."),
	[ST_F_STOT]           = IST("Total number of sessions."),
	[ST_F_BIN]            = IST("Current total of incoming bytes."),
	[ST_F_BOUT]           = IST("Current total of outgoing bytes."),
	[ST_F_DREQ]           = IST("Total number of denied requests."),
	[ST_F_DRESP]          = IST("Total number of denied responses."),
	[ST_F_EREQ]           = IST("Total number of request errors."),
	[ST_F_ECON]           = IST("Total number of connection errors."),
	[ST_F_ERESP]          = IST("Total number of response errors."),
	[ST_F_WRETR]          = IST("Total number of retry warnings."),
	[ST_F_WREDIS]         = IST("Total number of redispatch warnings."),
	[ST_F_STATUS]         = IST("Current status of the service (frontend: 0=STOP, 1=UP - backend: 0=DOWN, 1=UP - server: 0=DOWN, 1=UP, 2=MAINT, 3=DRAIN, 4=NOLB)."),
	[ST_F_WEIGHT]         = IST("Service weight."),
	[ST_F_ACT]            = IST("Current number of active servers."),
	[ST_F_BCK]            = IST("Current number of backup servers."),
	[ST_F_CHKFAIL]        = IST("Total number of failed check (Only counts checks failed when the server is up)."),
	[ST_F_CHKDOWN]        = IST("Total number of UP->DOWN transitions."),
	[ST_F_LASTCHG]        = IST("Number of seconds since the last UP<->DOWN transition."),
	[ST_F_DOWNTIME]       = IST("Total downtime (in seconds) for the service."),
	[ST_F_QLIMIT]         = IST("Configured maxqueue for the server (0 meaning no limit)."),
	[ST_F_PID]            = IST("Process id (0 for first instance, 1 for second, ...)"),
	[ST_F_IID]            = IST("Unique proxy id."),
	[ST_F_SID]            = IST("Server id (unique inside a proxy)."),
	[ST_F_THROTTLE]       = IST("Current throttle percentage for the server, when slowstart is active, or no value if not in slowstart."),
	[ST_F_LBTOT]          = IST("Total number of times a service was selected, either for new sessions, or when redispatching."),
	[ST_F_TRACKED]        = IST("Id of proxy/server if tracking is enabled."),
	[ST_F_TYPE]           = IST("Service type (0=frontend, 1=backend, 2=server, 3=socket/listener)."),
	[ST_F_RATE]           = IST("Current number of sessions per second over last elapsed second."),
	[ST_F_RATE_LIM]       = IST("Configured limit on new sessions per second."),
	[ST_F_RATE_MAX]       = IST("Maximum observed number of sessions per second."),
	[ST_F_CHECK_STATUS]   = IST("Status of last health check (HCHK_STATUS_* values)."),
	[ST_F_CHECK_CODE]     = IST("layer5-7 code, if available of the last health check."),
	[ST_F_CHECK_DURATION] = IST("Total duration of the latest server health check, in seconds."),
	[ST_F_HRSP_1XX]       = IST("Total number of HTTP responses."),
	[ST_F_HRSP_2XX]       = IST("Total number of HTTP responses."),
	[ST_F_HRSP_3XX]       = IST("Total number of HTTP responses."),
	[ST_F_HRSP_4XX]       = IST("Total number of HTTP responses."),
	[ST_F_HRSP_5XX]       = IST("Total number of HTTP responses."),
	[ST_F_HRSP_OTHER]     = IST("Total number of HTTP responses."),
	[ST_F_HANAFAIL]       = IST("Total number of failed health checks."),
	[ST_F_REQ_RATE]       = IST("Current number of HTTP requests per second over last elapsed second."),
	[ST_F_REQ_RATE_MAX]   = IST("Maximum observed number of HTTP requests per second."),
	[ST_F_REQ_TOT]        = IST("Total number of HTTP requests received."),
	[ST_F_CLI_ABRT]       = IST("Total number of data transfers aborted by the client."),
	[ST_F_SRV_ABRT]       = IST("Total number of data transfers aborted by the server."),
	[ST_F_COMP_IN]        = IST("Total number of HTTP response bytes fed to the compressor."),
	[ST_F_COMP_OUT]       = IST("Total number of HTTP response bytes emitted by the compressor."),
	[ST_F_COMP_BYP]       = IST("Total number of bytes that bypassed the HTTP compressor (CPU/BW limit)."),
	[ST_F_COMP_RSP]       = IST("Total number of HTTP responses that were compressed."),
	[ST_F_LASTSESS]       = IST("Number of seconds since last session assigned to server/backend."),
	[ST_F_LAST_CHK]       = IST("Last health check contents or textual error"),
	[ST_F_LAST_AGT]       = IST("Last agent check contents or textual error"),
	[ST_F_QTIME]          = IST("Avg. queue time for last 1024 successful connections."),
	[ST_F_CTIME]          = IST("Avg. connect time for last 1024 successful connections."),
	[ST_F_RTIME]          = IST("Avg. response time for last 1024 successful connections."),
	[ST_F_TTIME]          = IST("Avg. total time for last 1024 successful connections."),
	[ST_F_AGENT_STATUS]   = IST("Status of last agent check."),
	[ST_F_AGENT_CODE]     = IST("Numeric code reported by agent if any (unused for now)."),
	[ST_F_AGENT_DURATION] = IST("Time in ms taken to finish last agent check."),
	[ST_F_CHECK_DESC]     = IST("Short human-readable description of the last health status."),
	[ST_F_AGENT_DESC]     = IST("Short human-readable description of the last agent status."),
	[ST_F_CHECK_RISE]     = IST("Server's \"rise\" parameter used by health checks"),
	[ST_F_CHECK_FALL]     = IST("Server's \"fall\" parameter used by health checks"),
	[ST_F_CHECK_HEALTH]   = IST("Server's health check value between 0 and rise+fall-1"),
	[ST_F_AGENT_RISE]     = IST("Agent's \"rise\" parameter, normally 1."),
	[ST_F_AGENT_FALL]     = IST("Agent's \"fall\" parameter, normally 1."),
	[ST_F_AGENT_HEALTH]   = IST("Agent's health parameter, between 0 and rise+fall-1"),
	[ST_F_ADDR]           = IST("address:port or \"unix\". IPv6 has brackets around the address."),
	[ST_F_COOKIE]         = IST("Server's cookie value or backend's cookie name."),
	[ST_F_MODE]           = IST("Proxy mode (tcp, http, health, unknown)."),
	[ST_F_ALGO]           = IST("Load balancing algorithm."),
	[ST_F_CONN_RATE]      = IST("Current number of connections per second over the last elapsed second."),
	[ST_F_CONN_RATE_MAX]  = IST("Maximum observed number of connections per second."),
	[ST_F_CONN_TOT]       = IST("Total number of connections."),
	[ST_F_INTERCEPTED]    = IST("Total number of intercepted HTTP requests."),
	[ST_F_DCON]           = IST("Total number of requests denied by \"tcp-request connection\" rules."),
	[ST_F_DSES]           = IST("Total number of requests denied by \"tcp-request session\" rules."),
	[ST_F_WREW]           = IST("Total number of failed header rewriting warnings."),
	[ST_F_CONNECT]        = IST("Total number of connection establishment attempts."),
	[ST_F_REUSE]          = IST("Total number of connection reuses."),
	[ST_F_CACHE_LOOKUPS]  = IST("Total number of HTTP cache lookups."),
	[ST_F_CACHE_HITS]     = IST("Total number of HTTP cache hits."),
	[ST_F_SRV_ICUR]       = IST("Current number of idle connections available for reuse"),
	[ST_F_SRV_ILIM]       = IST("Limit on the number of available idle connections"),
	[ST_F_QT_MAX]         = IST("Maximum observed time spent in the queue"),
	[ST_F_CT_MAX]         = IST("Maximum observed time spent waiting for a connection to complete"),
	[ST_F_RT_MAX]         = IST("Maximum observed time spent waiting for a server response"),
	[ST_F_TT_MAX]         = IST("Maximum observed total request+response time (request+queue+connect+response+processing)"),
	[ST_F_EINT]           = IST("Total number of internal errors."),
	[ST_F_IDLE_CONN_CUR]  = IST("Current number of unsafe idle connections."),
	[ST_F_SAFE_CONN_CUR]  = IST("Current number of safe idle connections."),
	[ST_F_USED_CONN_CUR]  = IST("Current number of connections in use."),
	[ST_F_NEED_CONN_EST]  = IST("Estimated needed number of connections."),
};

/* Specific labels for all info fields. Empty by default. */
const struct ist promex_inf_metric_labels[INF_TOTAL_FIELDS] = {
	[INF_BUILD_INFO]  = IST(PROMEX_BUILDINFO_LABEL),
};

/* Specific labels for all stats fields. Empty by default. */
const struct ist promex_st_metric_labels[ST_F_TOTAL_FIELDS] = {
	[ST_F_HRSP_1XX]   = IST("code=\"1xx\""),
	[ST_F_HRSP_2XX]   = IST("code=\"2xx\""),
	[ST_F_HRSP_3XX]   = IST("code=\"3xx\""),
	[ST_F_HRSP_4XX]   = IST("code=\"4xx\""),
	[ST_F_HRSP_5XX]   = IST("code=\"5xx\""),
	[ST_F_HRSP_OTHER] = IST("code=\"other\""),
};

/* Return the server status: 0=DOWN, 1=UP, 2=MAINT, 3=DRAIN, 4=NOLB. */
static int promex_srv_status(struct server *sv)
{
	int state = 0;

	if (sv->cur_state == SRV_ST_RUNNING || sv->cur_state == SRV_ST_STARTING) {
		state = 1;
		if (sv->cur_admin & SRV_ADMF_DRAIN)
			state = 3;
	}
	else if (sv->cur_state == SRV_ST_STOPPING)
		state = 4;

	if (sv->cur_admin & SRV_ADMF_MAINT)
		state = 2;

	return state;
}

/* Convert a field to its string representation and write it in <out>, followed
 * by a newline, if there is enough space. non-numeric value are converted in
 * "Nan" because Prometheus only support numerical values (but it is unexepceted
 * to process this kind of value). It returns 1 on success. Otherwise, it
 * returns 0. The buffer's length must not exceed <max> value.
 */
static int promex_metric_to_str(struct buffer *out, struct field *f, size_t max)
{
	int ret = 0;

	switch (field_format(f, 0)) {
		case FF_EMPTY: ret = chunk_strcat(out,  "Nan\n"); break;
		case FF_S32:   ret = chunk_appendf(out, "%d\n", f->u.s32); break;
		case FF_U32:   ret = chunk_appendf(out, "%u\n", f->u.u32); break;
		case FF_S64:   ret = chunk_appendf(out, "%lld\n", (long long)f->u.s64); break;
		case FF_U64:   ret = chunk_appendf(out, "%llu\n", (unsigned long long)f->u.u64); break;
		case FF_FLT:   ret = chunk_appendf(out, "%f\n", f->u.flt); break;
		case FF_STR:   ret = chunk_strcat(out,  "Nan\n"); break;
		default:       ret = chunk_strcat(out,  "Nan\n"); break;
	}
	if (!ret || out->data > max)
		return 0;
	return 1;
}

/* Dump the header lines for <metric>. It is its #HELP and #TYPE strings. It
 * returns 1 on success. Otherwise, if <out> length exceeds <max>, it returns 0.
 */
static int promex_dump_metric_header(struct appctx *appctx, struct htx *htx,
				     const struct promex_metric *metric, const struct ist name,
				     struct ist *out, size_t max)
{
	struct ist type;

	switch (metric->type) {
		case PROMEX_MT_COUNTER:
			type = ist("counter");
			break;
		default:
			type = ist("gauge");
	}

	if (istcat(out, ist("# HELP "), max) == -1 ||
	    istcat(out, name, max) == -1 ||
	    istcat(out, ist(" "), max) == -1)
		goto full;

	if (metric->flags & PROMEX_FL_INFO_METRIC) {
		if (istcat(out, ist(info_fields[appctx->st2].desc), max) == -1)
			goto full;
	}
	else {
		if (istcat(out, promex_st_metric_desc[appctx->st2], max) == -1)
			goto full;
	}

	if (istcat(out, ist("\n# TYPE "), max) == -1 ||
	    istcat(out, name, max) == -1 ||
	    istcat(out, ist(" "), max) == -1 ||
	    istcat(out, type, max) == -1 ||
	    istcat(out, ist("\n"), max) == -1)
		goto full;

	return 1;

  full:
	return 0;
}

/* Dump the line for <metric>. It starts by the metric name followed by its
 * labels (proxy name, server name...) between braces and finally its value. If
 * not already done, the header lines are dumped first. It returns 1 on
 * success. Otherwise if <out> length exceeds <max>, it returns 0.
 */
static int promex_dump_metric(struct appctx *appctx, struct htx *htx, struct ist prefix,
			     const  struct promex_metric *metric, struct field *val,
			      struct ist *out, size_t max)
{
	struct ist name = { .ptr = (char[PROMEX_MAX_NAME_LEN]){ 0 }, .len = 0 };
	size_t len = out->len;

	if (out->len + PROMEX_MAX_METRIC_LENGTH > max)
		return 0;

	/* Fill the metric name */
	istcat(&name, prefix, PROMEX_MAX_NAME_LEN);
	istcat(&name, metric->n, PROMEX_MAX_NAME_LEN);


	if ((appctx->ctx.stats.flags & PROMEX_FL_METRIC_HDR) &&
	    !promex_dump_metric_header(appctx, htx, metric, name, out, max))
		goto full;

	if (appctx->ctx.stats.flags & PROMEX_FL_INFO_METRIC) {
		const struct ist label = promex_inf_metric_labels[appctx->st2];

		if (istcat(out, name, max) == -1 ||
		    (label.len && istcat(out, ist("{"), max) == -1) ||
		    (label.len && istcat(out, label, max) == -1) ||
		    (label.len && istcat(out, ist("}"), max) == -1) ||
		    istcat(out, ist(" "), max) == -1)
			goto full;
	}
	else {
		struct proxy *px = appctx->ctx.stats.obj1;
		struct server *srv = appctx->ctx.stats.obj2;
		const struct ist label = promex_st_metric_labels[appctx->st2];

		if (istcat(out, name, max) == -1 ||
		    istcat(out, ist("{proxy=\""), max) == -1 ||
		    istcat(out, ist2(px->id, strlen(px->id)), max) == -1 ||
		    istcat(out, ist("\""), max) == -1 ||
		    (srv && istcat(out, ist(",server=\""), max) == -1) ||
		    (srv && istcat(out, ist2(srv->id, strlen(srv->id)), max) == -1) ||
		    (srv && istcat(out, ist("\""), max) == -1) ||
		    (label.len && istcat(out, ist(","), max) == -1) ||
		    (label.len && istcat(out, label, max) == -1) ||
		    istcat(out, ist("} "), max) == -1)
			goto full;
	}

	trash.data = out->len;
	if (!promex_metric_to_str(&trash, val, max))
		goto full;
	out->len = trash.data;

	appctx->ctx.stats.flags &= ~PROMEX_FL_METRIC_HDR;
	return 1;
  full:
	// Restore previous length
	out->len = len;
	return 0;

}


/* Dump global metrics (prefixed by "haproxy_process_"). It returns 1 on success,
 * 0 if <htx> is full and -1 in case of any error. */
static int promex_dump_global_metrics(struct appctx *appctx, struct htx *htx)
{
	static struct ist prefix = IST("haproxy_process_");
	struct field val;
	struct channel *chn = si_ic(appctx->owner);
	struct ist out = ist2(trash.area, 0);
	size_t max = htx_get_max_blksz(htx, channel_htx_recv_max(chn, htx));
	int ret = 1;

	if (!stats_fill_info(info, INF_TOTAL_FIELDS))
		return -1;

	for (; appctx->st2 < INF_TOTAL_FIELDS; appctx->st2++) {
		if (!(promex_global_metrics[appctx->st2].flags & appctx->ctx.stats.flags))
			continue;

		switch (appctx->st2) {
			case INF_BUILD_INFO:
				val = mkf_u32(FN_GAUGE, 1);
				break;

			default:
				val = info[appctx->st2];
		}

		if (!promex_dump_metric(appctx, htx, prefix, &promex_global_metrics[appctx->st2], &val, &out, max))
			goto full;

		appctx->ctx.stats.flags |= PROMEX_FL_METRIC_HDR;
	}

  end:
	if (out.len) {
		if (!htx_add_data_atonce(htx, out))
			return -1; /* Unexpected and unrecoverable error */
		channel_add_input(chn, out.len);
	}
	return ret;
  full:
	ret = 0;
	goto end;
}

/* Dump frontends metrics (prefixed by "haproxy_frontend_"). It returns 1 on success,
 * 0 if <htx> is full and -1 in case of any error. */
static int promex_dump_front_metrics(struct appctx *appctx, struct htx *htx)
{
	static struct ist prefix = IST("haproxy_frontend_");
	struct proxy *px;
	struct field val;
	struct channel *chn = si_ic(appctx->owner);
	struct ist out = ist2(trash.area, 0);
	size_t max = htx_get_max_blksz(htx, channel_htx_recv_max(chn, htx));
	struct field *stats = stat_l[STATS_DOMAIN_PROXY];
	int ret = 1;

	for (;appctx->st2 < ST_F_TOTAL_FIELDS; appctx->st2++) {
		if (!(promex_st_metrics[appctx->st2].flags & appctx->ctx.stats.flags))
			continue;

		while (appctx->ctx.stats.obj1) {
			px = appctx->ctx.stats.obj1;

			/* skip the disabled proxies, global frontend and non-networked ones */
			if (px->disabled || px->uuid <= 0 || !(px->cap & PR_CAP_FE))
				goto next_px;

			if (!stats_fill_fe_stats(px, stats, ST_F_TOTAL_FIELDS, &(appctx->st2)))
				return -1;

			switch (appctx->st2) {
				case ST_F_STATUS:
					val = mkf_u32(FO_STATUS, !px->disabled);
					break;
				case ST_F_REQ_RATE_MAX:
				case ST_F_REQ_TOT:
				case ST_F_HRSP_1XX:
				case ST_F_INTERCEPTED:
				case ST_F_CACHE_LOOKUPS:
				case ST_F_CACHE_HITS:
				case ST_F_COMP_IN:
				case ST_F_COMP_OUT:
				case ST_F_COMP_BYP:
				case ST_F_COMP_RSP:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					val = stats[appctx->st2];
					break;
				case ST_F_HRSP_2XX:
				case ST_F_HRSP_3XX:
				case ST_F_HRSP_4XX:
				case ST_F_HRSP_5XX:
				case ST_F_HRSP_OTHER:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					val = stats[appctx->st2];
					appctx->ctx.stats.flags &= ~PROMEX_FL_METRIC_HDR;
					break;

				default:
					val = stats[appctx->st2];
			}

			if (!promex_dump_metric(appctx, htx, prefix, &promex_st_metrics[appctx->st2], &val, &out, max))
				goto full;
		  next_px:
			appctx->ctx.stats.obj1 = px->next;
		}
		appctx->ctx.stats.flags |= PROMEX_FL_METRIC_HDR;
		appctx->ctx.stats.obj1 = proxies_list;
	}

  end:
	if (out.len) {
		if (!htx_add_data_atonce(htx, out))
			return -1; /* Unexpected and unrecoverable error */
		channel_add_input(chn, out.len);
	}
	return ret;
  full:
	ret = 0;
	goto end;
}

/* Dump backends metrics (prefixed by "haproxy_backend_"). It returns 1 on success,
 * 0 if <htx> is full and -1 in case of any error. */
static int promex_dump_back_metrics(struct appctx *appctx, struct htx *htx)
{
	static struct ist prefix = IST("haproxy_backend_");
	struct proxy *px;
	struct field val;
	struct channel *chn = si_ic(appctx->owner);
	struct ist out = ist2(trash.area, 0);
	size_t max = htx_get_max_blksz(htx, channel_htx_recv_max(chn, htx));
	int ret = 1;
	uint32_t weight;
	double secs;

	for (;appctx->st2 < ST_F_TOTAL_FIELDS; appctx->st2++) {
		if (!(promex_st_metrics[appctx->st2].flags & appctx->ctx.stats.flags))
			continue;

		while (appctx->ctx.stats.obj1) {
			px = appctx->ctx.stats.obj1;

			/* skip the disabled proxies, global frontend and non-networked ones */
			if (px->disabled || px->uuid <= 0 || !(px->cap & PR_CAP_BE))
				goto next_px;

			switch (appctx->st2) {
				case ST_F_STATUS:
					val = mkf_u32(FO_STATUS, (px->lbprm.tot_weight > 0 || !px->srv) ? 1 : 0);
					break;
				case ST_F_SCUR:
					val = mkf_u32(0, px->beconn);
					break;
				case ST_F_SMAX:
					val = mkf_u32(FN_MAX, px->be_counters.conn_max);
					break;
				case ST_F_SLIM:
					val = mkf_u32(FO_CONFIG|FN_LIMIT, px->fullconn);
					break;
				case ST_F_STOT:
					val = mkf_u64(FN_COUNTER, px->be_counters.cum_conn);
					break;
				case ST_F_RATE_MAX:
					val = mkf_u32(0, px->be_counters.sps_max);
					break;
				case ST_F_LASTSESS:
					val = mkf_s32(FN_AGE, be_lastsession(px));
					break;
				case ST_F_QCUR:
					val = mkf_u32(0, px->nbpend);
					break;
				case ST_F_QMAX:
					val = mkf_u32(FN_MAX, px->be_counters.nbpend_max);
					break;
				case ST_F_CONNECT:
					val = mkf_u64(FN_COUNTER, px->be_counters.connect);
					break;
				case ST_F_REUSE:
					val = mkf_u64(FN_COUNTER, px->be_counters.reuse);
					break;
				case ST_F_BIN:
					val = mkf_u64(FN_COUNTER, px->be_counters.bytes_in);
					break;
				case ST_F_BOUT:
					val = mkf_u64(FN_COUNTER, px->be_counters.bytes_out);
					break;
				case ST_F_QTIME:
					secs = (double)swrate_avg(px->be_counters.q_time, TIME_STATS_SAMPLES) / 1000.0;
					val = mkf_flt(FN_AVG, secs);
					break;
				case ST_F_CTIME:
					secs = (double)swrate_avg(px->be_counters.c_time, TIME_STATS_SAMPLES) / 1000.0;
					val = mkf_flt(FN_AVG, secs);
					break;
				case ST_F_RTIME:
					secs = (double)swrate_avg(px->be_counters.d_time, TIME_STATS_SAMPLES) / 1000.0;
					val = mkf_flt(FN_AVG, secs);
					break;
				case ST_F_TTIME:
					secs = (double)swrate_avg(px->be_counters.t_time, TIME_STATS_SAMPLES) / 1000.0;
					val = mkf_flt(FN_AVG, secs);
					break;
				case ST_F_QT_MAX:
					secs = (double)px->be_counters.qtime_max / 1000.0;
					val = mkf_flt(FN_MAX, secs);
					break;
				case ST_F_CT_MAX:
					secs = (double)px->be_counters.ctime_max / 1000.0;
					val = mkf_flt(FN_MAX, secs);
					break;
				case ST_F_RT_MAX:
					secs = (double)px->be_counters.dtime_max / 1000.0;
					val = mkf_flt(FN_MAX, secs);
					break;
				case ST_F_TT_MAX:
					secs = (double)px->be_counters.ttime_max / 1000.0;
					val = mkf_flt(FN_MAX, secs);
					break;
				case ST_F_DREQ:
					val = mkf_u64(FN_COUNTER, px->be_counters.denied_req);
					break;
				case ST_F_DRESP:
					val = mkf_u64(FN_COUNTER, px->be_counters.denied_resp);
					break;
				case ST_F_ECON:
					val = mkf_u64(FN_COUNTER, px->be_counters.failed_conns);
					break;
				case ST_F_ERESP:
					val = mkf_u64(FN_COUNTER, px->be_counters.failed_resp);
					break;
				case ST_F_WRETR:
					val = mkf_u64(FN_COUNTER, px->be_counters.retries);
					break;
				case ST_F_WREDIS:
					val = mkf_u64(FN_COUNTER, px->be_counters.redispatches);
					break;
				case ST_F_WREW:
					val = mkf_u64(FN_COUNTER, px->be_counters.failed_rewrites);
					break;
				case ST_F_EINT:
					val = mkf_u64(FN_COUNTER, px->be_counters.internal_errors);
					break;
				case ST_F_CLI_ABRT:
					val = mkf_u64(FN_COUNTER, px->be_counters.cli_aborts);
					break;
				case ST_F_SRV_ABRT:
					val = mkf_u64(FN_COUNTER, px->be_counters.srv_aborts);
					break;
				case ST_F_WEIGHT:
					weight = (px->lbprm.tot_weight * px->lbprm.wmult + px->lbprm.wdiv - 1) / px->lbprm.wdiv;
					val = mkf_u32(FN_AVG, weight);
					break;
				case ST_F_ACT:
					val = mkf_u32(0, px->srv_act);
					break;
				case ST_F_BCK:
					val = mkf_u32(0, px->srv_bck);
					break;
				case ST_F_CHKDOWN:
					val = mkf_u64(FN_COUNTER, px->down_trans);
					break;
				case ST_F_LASTCHG:
					val = mkf_u32(FN_AGE, now.tv_sec - px->last_change);
					break;
				case ST_F_DOWNTIME:
					val = mkf_u32(FN_COUNTER, be_downtime(px));
					break;
				case ST_F_LBTOT:
					val = mkf_u64(FN_COUNTER, px->be_counters.cum_lbconn);
					break;
				case ST_F_REQ_TOT:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					val = mkf_u64(FN_COUNTER, px->be_counters.p.http.cum_req);
					break;
				case ST_F_HRSP_1XX:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					val = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[1]);
					break;
				case ST_F_HRSP_2XX:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					appctx->ctx.stats.flags &= ~PROMEX_FL_METRIC_HDR;
					val = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[2]);
					break;
				case ST_F_HRSP_3XX:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					appctx->ctx.stats.flags &= ~PROMEX_FL_METRIC_HDR;
					val = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[3]);
					break;
				case ST_F_HRSP_4XX:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					appctx->ctx.stats.flags &= ~PROMEX_FL_METRIC_HDR;
					val = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[4]);
					break;
				case ST_F_HRSP_5XX:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					appctx->ctx.stats.flags &= ~PROMEX_FL_METRIC_HDR;
					val = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[5]);
					break;
				case ST_F_HRSP_OTHER:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					appctx->ctx.stats.flags &= ~PROMEX_FL_METRIC_HDR;
					val = mkf_u64(FN_COUNTER, px->be_counters.p.http.rsp[0]);
					break;
				case ST_F_CACHE_LOOKUPS:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					val = mkf_u64(FN_COUNTER, px->be_counters.p.http.cache_lookups);
					break;
				case ST_F_CACHE_HITS:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					val = mkf_u64(FN_COUNTER, px->be_counters.p.http.cache_hits);
					break;
				case ST_F_COMP_IN:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					val = mkf_u64(FN_COUNTER, px->be_counters.comp_in);
					break;
				case ST_F_COMP_OUT:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					val = mkf_u64(FN_COUNTER, px->be_counters.comp_out);
					break;
				case ST_F_COMP_BYP:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					val = mkf_u64(FN_COUNTER, px->be_counters.comp_byp);
					break;
				case ST_F_COMP_RSP:
					if (px->mode != PR_MODE_HTTP)
						goto next_px;
					val = mkf_u64(FN_COUNTER, px->be_counters.p.http.comp_rsp);
					break;

				default:
					goto next_metric;
			}

			if (!promex_dump_metric(appctx, htx, prefix, &promex_st_metrics[appctx->st2], &val, &out, max))
				goto full;
		  next_px:
			appctx->ctx.stats.obj1 = px->next;
		}
	  next_metric:
		appctx->ctx.stats.flags |= PROMEX_FL_METRIC_HDR;
		appctx->ctx.stats.obj1 = proxies_list;
	}

  end:
	if (out.len) {
		if (!htx_add_data_atonce(htx, out))
			return -1; /* Unexpected and unrecoverable error */
		channel_add_input(chn, out.len);
	}
	return ret;
  full:
	ret = 0;
	goto end;
}

/* Dump servers metrics (prefixed by "haproxy_server_"). It returns 1 on success,
 * 0 if <htx> is full and -1 in case of any error. */
static int promex_dump_srv_metrics(struct appctx *appctx, struct htx *htx)
{
	static struct ist prefix = IST("haproxy_server_");
	struct proxy *px;
	struct server *sv;
	struct field val;
	struct channel *chn = si_ic(appctx->owner);
	struct ist out = ist2(trash.area, 0);
	size_t max = htx_get_max_blksz(htx, channel_htx_recv_max(chn, htx));
	int ret = 1;
	uint32_t weight;
	double secs;

	for (;appctx->st2 < ST_F_TOTAL_FIELDS; appctx->st2++) {
		if (!(promex_st_metrics[appctx->st2].flags & appctx->ctx.stats.flags))
			continue;

		while (appctx->ctx.stats.obj1) {
			px = appctx->ctx.stats.obj1;

			/* skip the disabled proxies, global frontend and non-networked ones */
			if (px->disabled || px->uuid <= 0 || !(px->cap & PR_CAP_BE))
				goto next_px;

			while (appctx->ctx.stats.obj2) {
				sv = appctx->ctx.stats.obj2;

				if ((appctx->ctx.stats.flags & PROMEX_FL_NO_MAINT_SRV) && (sv->cur_admin & SRV_ADMF_MAINT))
					goto next_sv;

				switch (appctx->st2) {
					case ST_F_STATUS:
						val = mkf_u32(FO_STATUS, promex_srv_status(sv));
						break;
					case ST_F_SCUR:
						val = mkf_u32(0, sv->cur_sess);
						break;
					case ST_F_SMAX:
						val = mkf_u32(FN_MAX, sv->counters.cur_sess_max);
						break;
					case ST_F_SLIM:
						val = mkf_u32(FO_CONFIG|FN_LIMIT, sv->maxconn);
						break;
					case ST_F_STOT:
						val = mkf_u64(FN_COUNTER, sv->counters.cum_sess);
						break;
					case ST_F_RATE_MAX:
						val = mkf_u32(FN_MAX, sv->counters.sps_max);
						break;
					case ST_F_LASTSESS:
						val = mkf_s32(FN_AGE, srv_lastsession(sv));
						break;
					case ST_F_QCUR:
						val = mkf_u32(0, sv->nbpend);
						break;
					case ST_F_QMAX:
						val = mkf_u32(FN_MAX, sv->counters.nbpend_max);
						break;
					case ST_F_QLIMIT:
						val = mkf_u32(FO_CONFIG|FS_SERVICE, sv->maxqueue);
						break;
					case ST_F_BIN:
						val = mkf_u64(FN_COUNTER, sv->counters.bytes_in);
						break;
					case ST_F_BOUT:
						val = mkf_u64(FN_COUNTER, sv->counters.bytes_out);
						break;
					case ST_F_QTIME:
						secs = (double)swrate_avg(sv->counters.q_time, TIME_STATS_SAMPLES) / 1000.0;
						val = mkf_flt(FN_AVG, secs);
						break;
					case ST_F_CTIME:
						secs = (double)swrate_avg(sv->counters.c_time, TIME_STATS_SAMPLES) / 1000.0;
						val = mkf_flt(FN_AVG, secs);
						break;
					case ST_F_RTIME:
						secs = (double)swrate_avg(sv->counters.d_time, TIME_STATS_SAMPLES) / 1000.0;
						val = mkf_flt(FN_AVG, secs);
						break;
					case ST_F_TTIME:
						secs = (double)swrate_avg(sv->counters.t_time, TIME_STATS_SAMPLES) / 1000.0;
						val = mkf_flt(FN_AVG, secs);
						break;
					case ST_F_QT_MAX:
						secs = (double)sv->counters.qtime_max / 1000.0;
						val = mkf_flt(FN_MAX, secs);
						break;
					case ST_F_CT_MAX:
						secs = (double)sv->counters.ctime_max / 1000.0;
						val = mkf_flt(FN_MAX, secs);
						break;
					case ST_F_RT_MAX:
						secs = (double)sv->counters.dtime_max / 1000.0;
						val = mkf_flt(FN_MAX, secs);
						break;
					case ST_F_TT_MAX:
						secs = (double)sv->counters.ttime_max / 1000.0;
						val = mkf_flt(FN_MAX, secs);
						break;
					case ST_F_CONNECT:
						val = mkf_u64(FN_COUNTER, sv->counters.connect);
						break;
					case ST_F_REUSE:
						val = mkf_u64(FN_COUNTER, sv->counters.reuse);
						break;
					case ST_F_DRESP:
						val = mkf_u64(FN_COUNTER, sv->counters.denied_resp);
						break;
					case ST_F_ECON:
						val = mkf_u64(FN_COUNTER, sv->counters.failed_conns);
						break;
					case ST_F_ERESP:
						val = mkf_u64(FN_COUNTER, sv->counters.failed_resp);
						break;
					case ST_F_WRETR:
						val = mkf_u64(FN_COUNTER, sv->counters.retries);
						break;
					case ST_F_WREDIS:
						val = mkf_u64(FN_COUNTER, sv->counters.redispatches);
						break;
					case ST_F_WREW:
						val = mkf_u64(FN_COUNTER, sv->counters.failed_rewrites);
						break;
					case ST_F_EINT:
						val = mkf_u64(FN_COUNTER, sv->counters.internal_errors);
						break;
					case ST_F_CLI_ABRT:
						val = mkf_u64(FN_COUNTER, sv->counters.cli_aborts);
						break;
					case ST_F_SRV_ABRT:
						val = mkf_u64(FN_COUNTER, sv->counters.srv_aborts);
						break;
					case ST_F_WEIGHT:
						weight = (sv->cur_eweight * px->lbprm.wmult + px->lbprm.wdiv - 1) / px->lbprm.wdiv;
						val = mkf_u32(FN_AVG, weight);
						break;
					case ST_F_CHECK_STATUS:
						if ((sv->check.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) != CHK_ST_ENABLED)
							goto next_sv;
						val = mkf_u32(FN_OUTPUT, sv->check.status);
						break;
					case ST_F_CHECK_CODE:
						if ((sv->check.state & (CHK_ST_ENABLED|CHK_ST_PAUSED)) != CHK_ST_ENABLED)
							goto next_sv;
						val = mkf_u32(FN_OUTPUT, (sv->check.status < HCHK_STATUS_L57DATA) ? 0 : sv->check.code);
						break;
					case ST_F_CHECK_DURATION:
						if (sv->check.status < HCHK_STATUS_CHECKED)
						    goto next_sv;
						secs = (double)sv->check.duration / 1000.0;
						val = mkf_flt(FN_DURATION, secs);
						break;
					case ST_F_CHKFAIL:
						val = mkf_u64(FN_COUNTER, sv->counters.failed_checks);
						break;
					case ST_F_CHKDOWN:
						val = mkf_u64(FN_COUNTER, sv->counters.down_trans);
						break;
					case ST_F_DOWNTIME:
						val = mkf_u32(FN_COUNTER, srv_downtime(sv));
						break;
					case ST_F_LASTCHG:
						val = mkf_u32(FN_AGE, now.tv_sec - sv->last_change);
						break;
					case ST_F_THROTTLE:
						val = mkf_u32(FN_AVG, server_throttle_rate(sv));
						break;
					case ST_F_LBTOT:
						val = mkf_u64(FN_COUNTER, sv->counters.cum_lbconn);
						break;
					case ST_F_REQ_TOT:
						if (px->mode != PR_MODE_HTTP)
							goto next_px;
						val = mkf_u64(FN_COUNTER, sv->counters.p.http.cum_req);
						break;
					case ST_F_HRSP_1XX:
						if (px->mode != PR_MODE_HTTP)
							goto next_px;
						val = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[1]);
						break;
					case ST_F_HRSP_2XX:
						if (px->mode != PR_MODE_HTTP)
							goto next_px;
						appctx->ctx.stats.flags &= ~PROMEX_FL_METRIC_HDR;
						val = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[2]);
						break;
					case ST_F_HRSP_3XX:
						if (px->mode != PR_MODE_HTTP)
							goto next_px;
						appctx->ctx.stats.flags &= ~PROMEX_FL_METRIC_HDR;
						val = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[3]);
						break;
					case ST_F_HRSP_4XX:
						if (px->mode != PR_MODE_HTTP)
							goto next_px;
						appctx->ctx.stats.flags &= ~PROMEX_FL_METRIC_HDR;
						val = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[4]);
						break;
					case ST_F_HRSP_5XX:
						if (px->mode != PR_MODE_HTTP)
							goto next_px;
						appctx->ctx.stats.flags &= ~PROMEX_FL_METRIC_HDR;
						val = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[5]);
						break;
					case ST_F_HRSP_OTHER:
						if (px->mode != PR_MODE_HTTP)
							goto next_px;
						appctx->ctx.stats.flags &= ~PROMEX_FL_METRIC_HDR;
						val = mkf_u64(FN_COUNTER, sv->counters.p.http.rsp[0]);
						break;
					case ST_F_SRV_ICUR:
						val = mkf_u32(0, sv->curr_idle_conns);
						break;
					case ST_F_SRV_ILIM:
						val = mkf_u32(FO_CONFIG|FN_LIMIT, (sv->max_idle_conns == -1) ? 0 : sv->max_idle_conns);
						break;
					case ST_F_IDLE_CONN_CUR:
						val = mkf_u32(0, sv->curr_idle_nb);
						break;
					case ST_F_SAFE_CONN_CUR:
						val = mkf_u32(0, sv->curr_safe_nb);
						break;
					case ST_F_USED_CONN_CUR:
						val = mkf_u32(0, sv->curr_used_conns);
						break;
					case ST_F_NEED_CONN_EST:
						val = mkf_u32(0, sv->est_need_conns);
						break;

					default:
						goto next_metric;
				}

				if (!promex_dump_metric(appctx, htx, prefix, &promex_st_metrics[appctx->st2], &val, &out, max))
					goto full;

			  next_sv:
				appctx->ctx.stats.obj2 = sv->next;
			}

		  next_px:
			appctx->ctx.stats.obj1 = px->next;
			appctx->ctx.stats.obj2 = (appctx->ctx.stats.obj1 ? ((struct proxy *)appctx->ctx.stats.obj1)->srv : NULL);
		}
	  next_metric:
		appctx->ctx.stats.flags |= PROMEX_FL_METRIC_HDR;
		appctx->ctx.stats.obj1 = proxies_list;
		appctx->ctx.stats.obj2 = (appctx->ctx.stats.obj1 ? ((struct proxy *)appctx->ctx.stats.obj1)->srv : NULL);
	}


  end:
	if (out.len) {
		if (!htx_add_data_atonce(htx, out))
			return -1; /* Unexpected and unrecoverable error */
		channel_add_input(chn, out.len);
	}
	return ret;
  full:
	ret = 0;
	goto end;
}

/* Dump all metrics (global, frontends, backends and servers) depending on the
 * dumper state (appctx->st1). It returns 1 on success, 0 if <htx> is full and
 * -1 in case of any error.
 * Uses <appctx.ctx.stats.obj1> as a pointer to the current proxy and <obj2> as
 * a pointer to the current server/listener. */
static int promex_dump_metrics(struct appctx *appctx, struct stream_interface *si, struct htx *htx)
{
	int ret;

	switch (appctx->st1) {
		case PROMEX_DUMPER_INIT:
			appctx->ctx.stats.obj1 = NULL;
			appctx->ctx.stats.obj2 = NULL;
			appctx->ctx.stats.flags |= (PROMEX_FL_METRIC_HDR|PROMEX_FL_INFO_METRIC);
			appctx->st2 = INF_NAME;
			appctx->st1 = PROMEX_DUMPER_GLOBAL;
			/* fall through */

		case PROMEX_DUMPER_GLOBAL:
			if (appctx->ctx.stats.flags & PROMEX_FL_SCOPE_GLOBAL) {
				ret = promex_dump_global_metrics(appctx, htx);
				if (ret <= 0) {
					if (ret == -1)
						goto error;
					goto full;
				}
			}

			appctx->ctx.stats.obj1 = proxies_list;
			appctx->ctx.stats.obj2 = NULL;
			appctx->ctx.stats.flags &= ~PROMEX_FL_INFO_METRIC;
			appctx->ctx.stats.flags |= (PROMEX_FL_METRIC_HDR|PROMEX_FL_FRONT_METRIC);
			appctx->st2 = ST_F_PXNAME;
			appctx->st1 = PROMEX_DUMPER_FRONT;
			/* fall through */

		case PROMEX_DUMPER_FRONT:
			if (appctx->ctx.stats.flags & PROMEX_FL_SCOPE_FRONT) {
				ret = promex_dump_front_metrics(appctx, htx);
				if (ret <= 0) {
					if (ret == -1)
						goto error;
					goto full;
				}
			}

			appctx->ctx.stats.obj1 = proxies_list;
			appctx->ctx.stats.obj2 = NULL;
			appctx->ctx.stats.flags &= ~PROMEX_FL_FRONT_METRIC;
			appctx->ctx.stats.flags |= (PROMEX_FL_METRIC_HDR|PROMEX_FL_BACK_METRIC);
			appctx->st2 = ST_F_PXNAME;
			appctx->st1 = PROMEX_DUMPER_BACK;
			/* fall through */

		case PROMEX_DUMPER_BACK:
			if (appctx->ctx.stats.flags & PROMEX_FL_SCOPE_BACK) {
				ret = promex_dump_back_metrics(appctx, htx);
				if (ret <= 0) {
					if (ret == -1)
						goto error;
					goto full;
				}
			}

			appctx->ctx.stats.obj1 = proxies_list;
			appctx->ctx.stats.obj2 = (appctx->ctx.stats.obj1 ? ((struct proxy *)appctx->ctx.stats.obj1)->srv : NULL);
			appctx->ctx.stats.flags &= ~PROMEX_FL_BACK_METRIC;
			appctx->ctx.stats.flags |= (PROMEX_FL_METRIC_HDR|PROMEX_FL_SRV_METRIC);
			appctx->st2 = ST_F_PXNAME;
			appctx->st1 = PROMEX_DUMPER_SRV;
			/* fall through */

		case PROMEX_DUMPER_SRV:
			if (appctx->ctx.stats.flags & PROMEX_FL_SCOPE_SERVER) {
				ret = promex_dump_srv_metrics(appctx, htx);
				if (ret <= 0) {
					if (ret == -1)
						goto error;
					goto full;
				}
			}

			appctx->ctx.stats.obj1 = NULL;
			appctx->ctx.stats.obj2 = NULL;
			appctx->ctx.stats.flags &= ~(PROMEX_FL_METRIC_HDR|PROMEX_FL_SRV_METRIC);
			appctx->st2 = 0;
			appctx->st1 = PROMEX_DUMPER_DONE;
			/* fall through */

		case PROMEX_DUMPER_DONE:
		default:
			break;
	}

	return 1;

  full:
	si_rx_room_blk(si);
	return 0;
  error:
	/* unrecoverable error */
	appctx->ctx.stats.obj1 = NULL;
	appctx->ctx.stats.obj2 = NULL;
	appctx->ctx.stats.flags = 0;
	appctx->st2 = 0;
	appctx->st1 = PROMEX_DUMPER_DONE;
	return -1;
}

/* Parse the query string of request URI to filter the metrics. It returns 1 on
 * success and -1 on error. */
static int promex_parse_uri(struct appctx *appctx, struct stream_interface *si)
{
	struct channel *req = si_oc(si);
	struct channel *res = si_ic(si);
	struct htx *req_htx, *res_htx;
	struct htx_sl *sl;
	char *p, *key, *value;
	const char *end;
	struct buffer *err;
	int default_scopes = PROMEX_FL_SCOPE_ALL;
	int len;

	/* Get the query-string */
	req_htx = htxbuf(&req->buf);
	sl = http_get_stline(req_htx);
	if (!sl)
		goto error;
	p = http_find_param_list(HTX_SL_REQ_UPTR(sl), HTX_SL_REQ_ULEN(sl), '?');
	if (!p)
		goto end;
	end = HTX_SL_REQ_UPTR(sl) + HTX_SL_REQ_ULEN(sl);

	/* copy the query-string */
	len = end - p;
	chunk_reset(&trash);
	memcpy(trash.area, p, len);
	trash.area[len] = 0;
	p = trash.area;
	end = trash.area + len;

	/* Parse the query-string */
	while (p < end && *p && *p != '#') {
		value = NULL;

		/* decode parameter name */
		key = p;
		while (p < end && *p != '=' && *p != '&' && *p != '#')
			++p;
		/* found a value */
		if (*p == '=') {
			*(p++) = 0;
			value = p;
		}
		else if (*p == '&')
			*(p++) = 0;
		else if (*p == '#')
			*p = 0;
		len = url_decode(key, 1);
		if (len == -1)
			goto error;

		/* decode value */
		if (value) {
			while (p < end && *p != '=' && *p != '&' && *p != '#')
				++p;
			if (*p == '=')
				goto error;
			if (*p == '&')
				*(p++) = 0;
			else if (*p == '#')
				*p = 0;
			len = url_decode(value, 1);
			if (len == -1)
				goto error;
		}

		if (strcmp(key, "scope") == 0) {
			default_scopes = 0; /* at least a scope defined, unset default scopes */
			if (!value)
				goto error;
			else if (*value == 0)
				appctx->ctx.stats.flags &= ~PROMEX_FL_SCOPE_ALL;
			else if (*value == '*')
				appctx->ctx.stats.flags |= PROMEX_FL_SCOPE_ALL;
			else if (strcmp(value, "global") == 0)
				appctx->ctx.stats.flags |= PROMEX_FL_SCOPE_GLOBAL;
			else if (strcmp(value, "server") == 0)
				appctx->ctx.stats.flags |= PROMEX_FL_SCOPE_SERVER;
			else if (strcmp(value, "backend") == 0)
				appctx->ctx.stats.flags |= PROMEX_FL_SCOPE_BACK;
			else if (strcmp(value, "frontend") == 0)
				appctx->ctx.stats.flags |= PROMEX_FL_SCOPE_FRONT;
			else
				goto error;
		}
		else if (strcmp(key, "no-maint") == 0)
			appctx->ctx.stats.flags |= PROMEX_FL_NO_MAINT_SRV;
	}

  end:
	appctx->ctx.stats.flags |= default_scopes;
	return 1;

  error:
	err = &http_err_chunks[HTTP_ERR_400];
	channel_erase(res);
	res->buf.data = b_data(err);
	memcpy(res->buf.area, b_head(err), b_data(err));
	res_htx = htx_from_buf(&res->buf);
	channel_add_input(res, res_htx->data);
	appctx->st0 = PROMEX_ST_END;
	return -1;
}

/* Send HTTP headers of the response. It returns 1 on success and 0 if <htx> is
 * full. */
static int promex_send_headers(struct appctx *appctx, struct stream_interface *si, struct htx *htx)
{
	struct channel *chn = si_ic(appctx->owner);
	struct htx_sl *sl;
	unsigned int flags;

	flags = (HTX_SL_F_IS_RESP|HTX_SL_F_VER_11|HTX_SL_F_XFER_ENC|HTX_SL_F_XFER_LEN|HTX_SL_F_CHNK);
	sl = htx_add_stline(htx, HTX_BLK_RES_SL, flags, ist("HTTP/1.1"), ist("200"), ist("OK"));
	if (!sl)
		goto full;
	sl->info.res.status = 200;
	if (!htx_add_header(htx, ist("Cache-Control"), ist("no-cache")) ||
	    !htx_add_header(htx, ist("Content-Type"), ist("text/plain; version=0.0.4")) ||
	    !htx_add_header(htx, ist("Transfer-Encoding"), ist("chunked")) ||
	    !htx_add_endof(htx, HTX_BLK_EOH))
		goto full;

	channel_add_input(chn, htx->data);
	return 1;
  full:
	htx_reset(htx);
	si_rx_room_blk(si);
	return 0;
}

/* The function returns 1 if the initialisation is complete, 0 if
 * an errors occurs and -1 if more data are required for initializing
 * the applet.
 */
static int promex_appctx_init(struct appctx *appctx, struct proxy *px, struct stream *strm)
{
	appctx->st0 = PROMEX_ST_INIT;
	return 1;
}

/* The main I/O handler for the promex applet. */
static void promex_appctx_handle_io(struct appctx *appctx)
{
	struct stream_interface *si = appctx->owner;
	struct stream *s = si_strm(si);
	struct channel *req = si_oc(si);
	struct channel *res = si_ic(si);
	struct htx *req_htx, *res_htx;
	int ret;

	res_htx = htx_from_buf(&res->buf);
	if (unlikely(si->state == SI_ST_DIS || si->state == SI_ST_CLO))
		goto out;

	/* Check if the input buffer is available. */
	if (!b_size(&res->buf)) {
		si_rx_room_blk(si);
		goto out;
	}

	switch (appctx->st0) {
		case PROMEX_ST_INIT:
			ret = promex_parse_uri(appctx, si);
			if (ret <= 0) {
				if (ret == -1)
					goto error;
				goto out;
			}
			appctx->st0 = PROMEX_ST_HEAD;
			appctx->st1 = PROMEX_DUMPER_INIT;
			/* fall through */

		case PROMEX_ST_HEAD:
			if (!promex_send_headers(appctx, si, res_htx))
				goto out;
			appctx->st0 = ((s->txn->meth == HTTP_METH_HEAD) ? PROMEX_ST_DONE : PROMEX_ST_DUMP);
			/* fall through */

		case PROMEX_ST_DUMP:
			ret = promex_dump_metrics(appctx, si, res_htx);
			if (ret <= 0) {
				if (ret == -1)
					goto error;
				goto out;
			}
			appctx->st0 = PROMEX_ST_DONE;
			/* fall through */

		case PROMEX_ST_DONE:
			/* Don't add TLR because mux-h1 will take care of it */
			res_htx->flags |= HTX_FL_EOI; /* no more data are expected. Only EOM remains to add now */
			if (!htx_add_endof(res_htx, HTX_BLK_EOM)) {
				si_rx_room_blk(si);
				goto out;
			}
			channel_add_input(res, 1);
			appctx->st0 = PROMEX_ST_END;
			/* fall through */

		case PROMEX_ST_END:
			if (!(res->flags & CF_SHUTR)) {
				res->flags |= CF_READ_NULL;
				si_shutr(si);
			}
	}

  out:
	htx_to_buf(res_htx, &res->buf);

	/* eat the whole request */
	if (co_data(req)) {
		req_htx = htx_from_buf(&req->buf);
		co_htx_skip(req, req_htx, co_data(req));
	}
	return;

  error:
	res->flags |= CF_READ_NULL;
	si_shutr(si);
	si_shutw(si);
}

struct applet promex_applet = {
	.obj_type = OBJ_TYPE_APPLET,
	.name = "<PROMEX>", /* used for logging */
	.init = promex_appctx_init,
	.fct = promex_appctx_handle_io,
};

static enum act_parse_ret service_parse_prometheus_exporter(const char **args, int *cur_arg, struct proxy *px,
							    struct act_rule *rule, char **err)
{
	/* Prometheus exporter service is only available on "http-request" rulesets */
	if (rule->from != ACT_F_HTTP_REQ) {
		memprintf(err, "Prometheus exporter service only available on 'http-request' rulesets");
		return ACT_RET_PRS_ERR;
	}

	/* Add applet pointer in the rule. */
	rule->applet = promex_applet;

	return ACT_RET_PRS_OK;
}
static void promex_register_build_options(void)
{
        char *ptr = NULL;

        memprintf(&ptr, "Built with the Prometheus exporter as a service");
        hap_register_build_opts(ptr, 1);
}


static struct action_kw_list service_actions = { ILH, {
	{ "prometheus-exporter", service_parse_prometheus_exporter },
	{ /* END */ }
}};

INITCALL1(STG_REGISTER, service_keywords_register, &service_actions);
INITCALL0(STG_REGISTER, promex_register_build_options);
