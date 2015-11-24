#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define DEFAULT_UDP_PORT 60228

// Plan:
// Use `ngx_udp_connection_t` with `off` flag as custom config
// Use `ngx_parse_url` to get everything we need from settings
// Use `ngx_pool_cleanup_add` to add handler for cleaning up udp connections
// Check setting for `off` in order to be able to turn off the module
typedef struct {
    ngx_udp_connection_t   *udp_connection;
    ngx_flag_t              off;
} ngx_http_udp_log_conf_t;

// This is very useful function from `ngx_resolver.c`,
// for some reason not declared in `ngx_resolver.h`
ngx_int_t ngx_udp_connect(ngx_udp_connection_t *uc);

// Setup functions headers
static void *ngx_http_udp_log_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_udp_log_merge_loc_conf(ngx_conf_t *cf, void *parent,
                                             void *child);

static char *ngx_http_udp_log_set_url(ngx_conf_t *cf, ngx_command_t *cmd,
                                      void *conf);

static ngx_int_t ngx_http_udp_log_init(ngx_conf_t *cf);


static ngx_command_t  ngx_http_udp_log_commands[] = {
    { ngx_string("udp_logging"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_udp_log_set_url,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_udp_log_module_ctx = {
    NULL,                          /* preconfiguration */
    ngx_http_udp_log_init,         /* postconfiguration */

    NULL,                          /* create main configuration */
    NULL,                          /* init main configuration */

    NULL,                          /* create server configuration */
    NULL,                          /* merge server configuration */

    ngx_http_udp_log_create_loc_conf,  /* create location configuration */
    ngx_http_udp_log_merge_loc_conf    /* merge location configuration */
};


ngx_module_t  ngx_http_udp_log_module = {
    NGX_MODULE_V1,
    &ngx_http_udp_log_module_ctx,  /* module context */
    ngx_http_udp_log_commands,     /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    NULL,                          /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    NULL,                          /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};


ngx_int_t
ngx_http_udp_log_send(ngx_udp_connection_t *udp_conn, u_char *buf, size_t len)
{
    // We don't connect to UDP server until it's time to send something
    // This is done so in order to provide
    if (udp_conn->connection == NULL) {
        if (ngx_udp_connect(udp_conn) != NGX_OK) {
            udp_conn->connection = NULL;
            return NGX_ERROR;
        }
    }

    ssize_t n = ngx_send(udp_conn->connection, buf, len);
    if (n == -1) {
        return NGX_ERROR;
    }
    return NGX_OK;
}


ngx_int_t
ngx_http_udp_log_handler(ngx_http_request_t *r)
{
    ngx_http_udp_log_conf_t *lcf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "udp log handler");

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_udp_log_module);

    if (lcf->off) {
        return NGX_OK;
    }

    return ngx_http_udp_log_send(lcf->udp_connection, r->uri.data, r->uri.len);
}


static char *
ngx_http_udp_log_set_url(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_udp_log_conf_t *ulcf = conf;
    ngx_url_t                serv;
    ngx_str_t               *value;

    value = cf->args->elts;

    // Check if udp logging is disabled
    if (ngx_strcmp(value[1].data, "off") == 0) {
        ulcf->off = 1;
        return NGX_CONF_OK;
    }

    // Try to parse given host:port string
    ngx_memzero(&serv, sizeof(ngx_url_t));
    serv.url = value[1];
    serv.default_port = DEFAULT_UDP_PORT;

    if (ngx_parse_url(cf->pool, &serv) != NGX_OK) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "%V: %s", &serv.host, serv.err);
        return NGX_CONF_ERROR;
    }

    // Copy data from `ngx_url_t` to `ngx_udp_connection_t`
    struct sockaddr       *udp_addr = ngx_palloc(cf->pool, sizeof(struct sockaddr));
    ngx_udp_connection_t  *udp_conn = ngx_palloc(cf->pool, sizeof(ngx_udp_connection_t));

    *udp_addr             = *serv.addrs[0].sockaddr;
    udp_conn->connection  = NULL;
    udp_conn->sockaddr    = udp_addr;
    udp_conn->socklen     = serv.addrs[0].socklen;
    udp_conn->server.len  = serv.addrs[0].name.len;
    udp_conn->server.data = ngx_pstrdup(cf->pool, &serv.addrs[0].name);

    if (udp_conn->server.data == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "failed to allocate buffer for connection server name");
        return NGX_CONF_ERROR;
    }

    // Set dummy log struct
    udp_conn->log         = cf->cycle->new_log;
    udp_conn->log.data    = NULL;
    udp_conn->log.handler = NULL;
    udp_conn->log.action  = "logging";

    return NGX_CONF_OK;
}


static void *
ngx_http_udp_log_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_udp_log_conf_t *conf;

    conf = ngx_palloc(cf->pool, sizeof(ngx_http_udp_log_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }
    conf->udp_connection = NGX_CONF_UNSET_PTR;
    conf->off = 0;
    return conf;
}


static char *
ngx_http_udp_log_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_udp_log_conf_t *prev = parent;
    ngx_http_udp_log_conf_t *conf = child;

    ngx_conf_merge_ptr_value(conf->udp_connection, prev->udp_connection,
                             NGX_CONF_UNSET_PTR);
    ngx_conf_merge_value(conf->off, prev->off, 0);

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_udp_log_init(ngx_conf_t *cf)
{
    ngx_http_core_main_conf_t  *cmcf;
    ngx_http_handler_pt        *h;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_udp_log_handler;

    return NGX_OK;
}