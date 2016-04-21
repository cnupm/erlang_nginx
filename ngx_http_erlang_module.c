// Copyright (c) 2008 Nick Gerakines <nick@gerakines.net>
// 
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "erl_interface.h"
#include "ei.h"

#define BUFSIZE 1000

extern const char *erl_thisnodename(void); 
extern short erl_thiscreation(void); 
#define SELF(fd) erl_mk_pid(erl_thisnodename(),fd,0,erl_thiscreation())

static char *ngx_http_erlang(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void *ngx_http_erlang_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_erlang_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

typedef struct {
     ngx_str_t node;
     ngx_str_t secret;
     ngx_str_t registered;
} ngx_http_erlang_loc_conf_t;

static ngx_command_t  ngx_http_erlang_commands[] = {
    {
        ngx_string("erlang"),
        NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
        ngx_http_erlang,
        0,
        0,
        NULL
    },
    {
        ngx_string("erlang_node"),
        NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_erlang_loc_conf_t, node),
        NULL
    },
    {
        ngx_string("erlang_secret"),
        NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_erlang_loc_conf_t, secret),
        NULL
    },
    {
        ngx_string("erlang_registered"),
        NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_erlang_loc_conf_t, registered),
        NULL
    },
    ngx_null_command
};

static ngx_http_module_t  ngx_http_erlang_module_ctx = {
    NULL,                               /* preconfiguration */
    NULL,                               /* postconfiguration */
    NULL,                               /* create main configuration */
    NULL,                               /* init main configuration */
    NULL,                               /* create server configuration */
    NULL,                               /* merge server configuration */
    ngx_http_erlang_create_loc_conf,    /* create location configuration */
    ngx_http_erlang_merge_loc_conf      /* merge location configuration */
};

ngx_module_t  ngx_http_erlang_module = {
    NGX_MODULE_V1,
    &ngx_http_erlang_module_ctx, /* module context */
    ngx_http_erlang_commands,   /* module directives */
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

static ngx_int_t ngx_http_erlang_handler(ngx_http_request_t *r) {
    ngx_int_t rc;
    ngx_buf_t *b;
    ngx_chain_t out;

    ngx_http_erlang_loc_conf_t  *conf;
    conf = ngx_http_get_module_loc_conf(r, ngx_http_erlang_module);
    
    if (!conf->node.len) {
        fprintf(stderr, "erlang_node not specified in configuration.\n");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    
    if (!conf->secret.len) {
        fprintf(stderr, "erlang_secret not specified in configuration.\n");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    
    if (!conf->registered.len) {
        fprintf(stderr, "erlang_registered not specified in configuration.\n");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    
    // TODO: Set this during the erlang message recieve loop.
    r->headers_out.content_type.len = sizeof("text/plain") - 1;
    r->headers_out.content_type.data = (u_char *) "text/plain";

    int fd;
    int got;
    unsigned char buf[BUFSIZE];
    ErlMessage emsg;
    
    ETERM *status, *retbody;
    
    erl_init(NULL, 0);
    
    if (erl_connect_init(1, (char *) conf->secret.data, 0) == -1) {
        erl_err_quit("erl_connect_init");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    
    if ((fd = erl_connect((char *) conf->node.data)) < 0) {
        erl_err_quit("erl_connect");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    fprintf(stderr, "Connected to handler@localhost\n\r");

    // TODO: Send the headers and request body
    ETERM *arr[3], *emsg2; 
    arr[0] = SELF(fd); 
    arr[1] = erl_mk_int(r->method); 
    arr[2] = erl_mk_string((const char *) r->uri.data);
    emsg2 = erl_mk_tuple(arr, 3);
    erl_reg_send(fd, (char *) conf->registered.data, emsg2);

    // TODO: Find a way to fail gracefully if a message isn't recieved within
    //       a specific amount of time.
    // TODO: Set the timeout duration in config.
    while (1) {    
        got = erl_receive_msg(fd, buf, BUFSIZE, &emsg);
        if (got == ERL_TICK) {
            continue;
        } else if (got == ERL_ERROR) {
            // An internal error has occured.
            r->headers_out.status = NGX_HTTP_INTERNAL_SERVER_ERROR;
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
            break;
        } else {
            if (emsg.type == ERL_SEND) {
                // TODO: Handle response tuple: {StatusCode, Headers, Body}
                status = erl_element(1, emsg.msg);
                retbody = erl_element(3, emsg.msg);
                int status_code = ERL_INT_VALUE(status);
                char *body = (char *) ERL_BIN_PTR(retbody);
                int body_length = strlen(body);
                
                // TODO: Set status code from response tuple
                if (status_code == 200) {
                    r->headers_out.status = NGX_HTTP_OK;
                } else {
                    r->headers_out.status = NGX_HTTP_INTERNAL_SERVER_ERROR;
                }
                
                b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
                if (b == NULL) {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }

                out.buf = b;
                out.next = NULL;

                b->pos = (u_char *) body;
                b->last = (u_char *) body + body_length;
                b->memory = 1;
                b->last_buf = 1;

                r->headers_out.status = NGX_HTTP_OK;
                r->headers_out.content_length_n = body_length;
                r->headers_out.last_modified_time = 23349600;

                rc = ngx_http_send_header(r);
                
                erl_free_term(emsg.msg);    
                erl_free_term(status);
                erl_free_term(retbody);
                
                // If there is a body component, run it through the next filter.
                if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
                    return rc;
                }

                break;
            }
        }
    }

    return ngx_http_output_filter(r, &out);
}

static char *ngx_http_erlang(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_core_loc_conf_t  *clcf;
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_erlang_handler;
    
    return NGX_CONF_OK;
}


static void *ngx_http_erlang_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_erlang_loc_conf_t  *conf;
    
    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_erlang_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }
    
    return conf;
}

static char *ngx_http_erlang_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_erlang_loc_conf_t *prev = parent;
    ngx_http_erlang_loc_conf_t *conf = child;
    
    ngx_conf_merge_str_value(conf->node, prev->node, "");
    ngx_conf_merge_str_value(conf->secret, prev->secret, "");
    ngx_conf_merge_str_value(conf->registered, prev->registered, "");
    
    return NGX_CONF_OK;
}
