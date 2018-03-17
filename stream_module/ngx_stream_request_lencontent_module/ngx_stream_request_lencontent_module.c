//
//  ngx_stream_request_lencontent_module.c
//  ngx-1.10.1-xcode
//
//  Created by xpwu on 2016/11/17.
//  Copyright © 2016年 xpwu. All rights reserved.
//

/**
 *
 *  lencontent protocol:
 *
 *  1, handshake protocol:
 *
 *        client ------------------ server
 *          |                          |
 *          |                          |
 *        ABCDEF (A^...^F = 0xff) --->  check(A^...^F == 0xff) -N--> over
 *          |                          |
 *          |                          |Y
 *          |                          |
 *         data      <-------->       data
 *
 *
 *  2, data protocol:
 *    1) length | content
 *      length: 4 bytes, net order; length=sizeof(content)+4; length=0 => heartbeat
 *
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream_request.h>

#define PROTOCOL_RESPONSE_SUCCESS 0
#define PROTOCOL_RESPONSE_FAILED 1

#ifdef this_module
#undef this_module
#endif
#define this_module ngx_stream_request_lencontent_module

#ifdef core_module
#undef core_module
#endif
#define core_module ngx_stream_request_core_module

static void *ngx_stream_lencontent_create_srv_conf(ngx_conf_t *cf);
static char *ngx_stream_lencontent_merge_srv_conf(ngx_conf_t *cf
                                                 , void *parent, void *child);
char *lencontent_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

typedef struct lencontent_srv_conf_s {
  ngx_msec_t handshake_timeout;
}lencontent_srv_conf_t;

static ngx_command_t  ngx_stream_lencontent_commands[] = {
  
  { ngx_string("lencontent_protocol"),
    NGX_STREAM_MAIN_CONF|NGX_STREAM_SRV_CONF|NGX_CONF_NOARGS,
    lencontent_conf,
    NGX_STREAM_SRV_CONF_OFFSET,
    0,
    NULL },
  { ngx_string("lenc_handshake_timeout"),
    NGX_STREAM_MAIN_CONF|NGX_STREAM_SRV_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_msec_slot,
    NGX_STREAM_SRV_CONF_OFFSET,
    offsetof(lencontent_srv_conf_t, handshake_timeout),
    NULL},
  
  ngx_null_command
};


static ngx_stream_module_t  ngx_stream_lencontent_module_ctx = {
  NULL,
  NULL,            /* postconfiguration */
  
  NULL,                               /* create main configuration */
  NULL,                                  /* init main configuration */
  
  ngx_stream_lencontent_create_srv_conf,   /* create server configuration */
  ngx_stream_lencontent_merge_srv_conf     /* merge server configuration */
};


ngx_module_t  ngx_stream_request_lencontent_module = {
  NGX_MODULE_V1,
  &ngx_stream_lencontent_module_ctx,           /* module context */
  ngx_stream_lencontent_commands,              /* module directives */
  NGX_STREAM_MODULE,                     /* module type */
  NULL,                                  /* init master */
  NULL,                                  /* init module */
  NULL,                                  /* init process */
  NULL,                                  /* init thread */
  NULL,                                  /* exit thread */
  NULL,                                  /* exit process */
  NULL,                                  /* exit master */
  NGX_MODULE_V1_PADDING
};

#if defined ( __clang__ ) && defined ( __llvm__ )
#pragma mark - conf
#endif

static void *ngx_stream_lencontent_create_srv_conf(ngx_conf_t *cf) {
  lencontent_srv_conf_t  *wscf;
  
  wscf = ngx_pcalloc(cf->pool, sizeof(lencontent_srv_conf_t));
  if (wscf == NULL) {
    return NULL;
  }
  
  wscf->handshake_timeout = NGX_CONF_UNSET_MSEC;
  
  return wscf;
}

static char *ngx_stream_lencontent_merge_srv_conf(ngx_conf_t *cf
                                                 , void *parent, void *child) {
  lencontent_srv_conf_t *prev = parent;
  lencontent_srv_conf_t *conf = child;
  
  ngx_conf_merge_msec_value(conf->handshake_timeout
                            , prev->handshake_timeout, 5000);
  
  return NGX_CONF_OK;
}

#if defined ( __clang__ ) && defined ( __llvm__ )
#pragma mark - process request
#endif

#define REQUEST_AGAIN (ngx_stream_request_t*) NGX_AGAIN
#define REQUEST_DONE (ngx_stream_request_t*) NGX_DONE
/*  return ngx_stream_request_t*: 解析到一个request
 return REQUEST_AGAIN: 解析数据不够
 return REQUEST_DONE: 进行下一步
 */
typedef ngx_stream_request_t* (*request_handler_t)(ngx_stream_session_t*);

typedef struct {
  u_char temp_buffer[6];
  u_char len;
  
  ngx_event_t timer;
  
  ngx_stream_request_t* r;
  
  request_handler_t handler;
  
  ngx_stream_request_t* (*get_request)(ngx_stream_session_t*);
  ngx_int_t (*handle_request)(ngx_stream_request_t*);
  ngx_int_t (*build_response)(ngx_stream_request_t*);
  
} lencontent_ctx_t;

static void ngx_stream_cleanup_event(void *data) {
  ngx_event_t* timer = data;
  
  if (timer->timer_set) {
    ngx_del_timer(timer);
  }
}

static void init_parse(ngx_stream_session_t* s);
static ngx_stream_request_t* get_handshake(ngx_stream_session_t*);
static ngx_int_t handle_handshake(ngx_stream_request_t* r);
static ngx_int_t build_handshake(ngx_stream_request_t* r);

static void init_parse_request(ngx_stream_session_t* s);
static ngx_stream_request_t* get_request(ngx_stream_session_t* s);
static ngx_int_t handle_request(ngx_stream_request_t* r);
static ngx_int_t build_response(ngx_stream_request_t* r);

static ngx_stream_request_t* parser_get_request (ngx_stream_session_t* s);
static ngx_int_t parser_handle_request(ngx_stream_request_t* r);
static ngx_int_t parser_build_response(ngx_stream_request_t* r);

char *lencontent_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
  
  ngx_stream_request_core_srv_conf_t* cscf;
  cscf = ngx_stream_conf_get_module_srv_conf(cf, core_module);
  
  cscf->protocol.get_request = parser_get_request;
  cscf->protocol.init_parser = init_parse;
  cscf->protocol.handler.name = "lencontent";
  cscf->protocol.handler.build_response = parser_build_response;
  cscf->protocol.handler.handle_request = parser_handle_request;
  
  ngx_stream_core_srv_conf_t  *scscf;
  scscf = ngx_stream_conf_get_module_srv_conf(cf, ngx_stream_core_module);
  scscf->handler = ngx_stream_request_core_handler;
  
  return NGX_CONF_OK;
}

static ngx_stream_request_t* parser_get_request(ngx_stream_session_t* s) {
  lencontent_ctx_t* ctx = ngx_stream_get_module_ctx(s, this_module);
  return ctx->get_request(s);
}

static ngx_int_t parser_handle_request (ngx_stream_request_t* r) {
  lencontent_ctx_t* ctx = ngx_stream_get_module_ctx(r->session, this_module);
  return ctx->handle_request(r);
}

static ngx_int_t parser_build_response(ngx_stream_request_t* r) {
  ngx_log_debug0(NGX_LOG_DEBUG_STREAM, r->session->connection->log
                 , 0, "lencontent parser_build_response");
  lencontent_ctx_t* ctx = ngx_stream_get_module_ctx(r->session, this_module);
  return ctx->build_response(r);
}

static void init_parse(ngx_stream_session_t* s) {
  ngx_connection_t* c = s->connection;
  lencontent_srv_conf_t* cscf;
  
  cscf = ngx_stream_get_module_srv_conf(s, this_module);
  
  lencontent_ctx_t* ctx = ngx_pcalloc(c->pool, sizeof(lencontent_ctx_t));
  ngx_stream_set_ctx(s, ctx, this_module);
  
  ngx_add_timer(c->read, cscf->handshake_timeout);
  
  ctx->get_request = get_handshake;
  ctx->handle_request = handle_handshake;
  ctx->build_response = build_handshake;
  
  ctx->len = 0;
  
  ngx_stream_cleanup_t* cln = ngx_stream_cleanup_add(s);
  cln->handler = ngx_stream_cleanup_event;
  cln->data = &ctx->timer;
}

static ngx_stream_request_t* get_handshake(ngx_stream_session_t* s) {
  ngx_connection_t* c = s->connection;
  lencontent_srv_conf_t* cscf
  = ngx_stream_get_module_srv_conf(s, this_module);
  lencontent_ctx_t* ctx = ngx_stream_get_module_ctx(s, this_module);
  ngx_log_t* log = s->connection->log;
  
  ssize_t re = c->recv(c, ctx->temp_buffer+ctx->len, 6-ctx->len);
  if (re <= 0 && re != NGX_AGAIN) {
    return NGX_STREAM_REQUEST_ERROR;
  }
  if (re == NGX_AGAIN) {
    ngx_add_timer(c->read, cscf->handshake_timeout);
    return NGX_STREAM_REQUEST_AGAIN;
  }
  
  ctx->len += re;
  
  if (ctx->len > 6) {
    ngx_log_error(NGX_LOG_ERR, log, 0
                  , "handshake len more than 6, which is %d", ctx->len);
    return NGX_STREAM_REQUEST_ERROR;
  }
  if (ctx->len < 6) {
    ngx_add_timer(c->read, cscf->handshake_timeout);
    return NGX_STREAM_REQUEST_AGAIN;
  }
  
  u_char xor = 0;
  int i = 0;
  for (i = 0; i < 6; ++i) {
    xor ^= ctx->temp_buffer[i];
  }
  
  if (xor != 0xff) {
    ngx_log_error(NGX_LOG_ERR, log, 0
                  , "handshake xor is not 0xff, which is %d", xor);
    return NGX_STREAM_REQUEST_ERROR;
  }
  
  init_parse_request(s);
  
  return NGX_STREAM_REQUEST_AGAIN;
}

static ngx_int_t handle_handshake(ngx_stream_request_t* r) {return NGX_OK;}
static ngx_int_t build_handshake(ngx_stream_request_t* r) {
  ngx_log_debug0(NGX_LOG_DEBUG_STREAM, r->session->connection->log
                 , 0, "lencontent build_handshake");
  return NGX_OK;}

typedef enum{
  HEART_BEAT,
  DATA
} request_type;

typedef struct{
  request_type type;
} request_ctx;

static void timer_heartbeat_handler(ngx_event_t* e) {
  ngx_connection_t* c = e->data;
  ngx_stream_session_t* s = c->data;
  ngx_stream_request_t* r = ngx_stream_new_request(s);
  
  ngx_log_debug1(NGX_LOG_DEBUG_STREAM, e->log, 0, "send heartbeat <r=%p>", r);
  
  request_ctx* extra = ngx_pcalloc(r->pool, sizeof(request_ctx));
  extra->type = HEART_BEAT;
  ngx_stream_request_set_ctx(r, extra, this_module);
  
  ngx_stream_handle_request_from(r, 0, 1);

  ngx_stream_request_core_srv_conf_t* cscf
  = ngx_stream_get_module_srv_conf(s, core_module);
  lencontent_ctx_t* ctx = ngx_stream_get_module_ctx(s, this_module);
  ngx_add_timer(&ctx->timer, cscf->heartbeat);
}

static ngx_stream_request_t* parse_length(ngx_stream_session_t* s);
static ngx_stream_request_t* parse_data(ngx_stream_session_t* s);

static void init_parse_request(ngx_stream_session_t* s) {
  lencontent_ctx_t* ctx = ngx_stream_get_module_ctx(s, this_module);
  ngx_stream_request_core_srv_conf_t* cscf
  = ngx_stream_get_module_srv_conf(s, core_module);
  ngx_connection_t* c = s->connection;
  
  if (c->read->timer_set) {
    ngx_del_timer(c->read);
  }
  ngx_add_timer(c->read, 2*cscf->heartbeat);
  
  ctx->timer.handler = timer_heartbeat_handler;
  ctx->timer.data = c;
  ctx->timer.log = c->log;
  ngx_add_timer(&ctx->timer, cscf->heartbeat);
  
  ctx->get_request = get_request;
  ctx->handle_request = handle_request;
  ctx->build_response = build_response;
  ctx->len = 0;
  
  ctx->handler = parse_length;
}

static ngx_stream_request_t* parse_length(ngx_stream_session_t* s) {
  ngx_connection_t* c = s->connection;
  ngx_stream_request_core_srv_conf_t* cscf
  = ngx_stream_get_module_srv_conf(s, core_module);
  lencontent_ctx_t* ctx = ngx_stream_get_module_ctx(s, this_module);
  ngx_log_t* log = s->connection->log;
  
  ssize_t re = c->recv(c, ctx->temp_buffer+ctx->len, 4-ctx->len);
  if (re < 0 && re != NGX_AGAIN) {
    if (c->read->eof) {
      ngx_log_error(NGX_LOG_INFO, log, 0
                    , "connection closed. lencontent parse_length re < 0 && re != NGX_AGAIN, which is %z", re);
    } else {
      ngx_log_error(NGX_LOG_ERR, log, 0
                    , "lencontent parse_length re < 0 && re != NGX_AGAIN, which is %z", re);
    }
    
    return NGX_STREAM_REQUEST_ERROR;
  }
  if (re == NGX_AGAIN || re == 0) {
    // 实际使用中，部分事件模型对ready的状态改变有延时性，故这里再加一层判断
    if (ctx->len == 0) { // 没有新的数据帧
      ngx_add_timer(c->read, 2*cscf->heartbeat);
    } else {
      ngx_add_timer(c->read, cscf->receive_from_client_timeout);
    }
    return REQUEST_AGAIN;
  }
  
  ctx->len += re;
  
  if (ctx->len < 4) {
    ngx_add_timer(c->read, cscf->receive_from_client_timeout);
    return REQUEST_AGAIN;
  }
  
  uint32_t *p = (uint32_t*)ctx->temp_buffer;
  ngx_int_t len = ntohl(*p);
//  ngx_int_t len = ntohl(*(uint32_t*)ctx->temp_buffer);
  ctx->len = 0;
  
  if (len == 0) { // heartbeat
    if (c->read->timer_set) {
      ngx_del_timer(c->read);
    }
    ngx_add_timer(c->read, 2*cscf->heartbeat);
    ngx_log_error(NGX_LOG_INFO, log, 0, "lencontent receive heartbeat");
    return REQUEST_AGAIN;
  }
  
  if (len <= 4) {
    ngx_log_error(NGX_LOG_ERR, log, 0, "request length <= 4, which is %d", len);
    return NGX_STREAM_REQUEST_ERROR;
  }

  len -= 4;
  
  ctx->r = ngx_stream_new_request(s);
  request_ctx* extra = ngx_pcalloc(ctx->r->pool, sizeof(request_ctx));
  extra->type = DATA;
  ngx_stream_request_set_ctx(ctx->r, extra, this_module);
  ctx->r->data = ngx_pcalloc(ctx->r->pool, sizeof(ngx_chain_t));
  ctx->r->data->buf = ngx_create_temp_buf(ctx->r->pool, len);
  
  ctx->handler = parse_data;
  return REQUEST_DONE;
}

static ngx_stream_request_t* parse_data(ngx_stream_session_t* s) {
  ngx_connection_t* c = s->connection;
  ngx_stream_request_core_srv_conf_t* cscf
  = ngx_stream_get_module_srv_conf(s, core_module);
  lencontent_ctx_t* ctx = ngx_stream_get_module_ctx(s, this_module);
  ngx_log_t* log = s->connection->log;
  ngx_stream_request_t* r = ctx->r;
  
  ssize_t re = c->recv(c, r->data->buf->last
                       , r->data->buf->end - r->data->buf->last);
  if (re < 0 && re != NGX_AGAIN) {
    ngx_log_error(NGX_LOG_ERR, log, 0
                  , "lencontent parse_data re <= 0 && re != NGX_AGAIN, which is %z", re);
    return NGX_STREAM_REQUEST_ERROR;
  }
  if (re == NGX_AGAIN || re == 0) {
    ngx_add_timer(c->read, cscf->receive_from_client_timeout);
    return REQUEST_AGAIN;
  }
  
  r->data->buf->last += re;
  
  if (r->data->buf->end != r->data->buf->last) {
    ngx_add_timer(c->read, cscf->receive_from_client_timeout);
    return REQUEST_AGAIN;
  }
  
  ctx->handler = parse_length;
  
  if (c->read->timer_set) {
    ngx_del_timer(c->read);
  }
  ngx_add_timer(c->read, 2*cscf->heartbeat);
  return r;
}

static ngx_stream_request_t* get_request(ngx_stream_session_t* s) {
  lencontent_ctx_t* ctx = ngx_stream_get_module_ctx(s, this_module);
  
  ngx_stream_request_t* r = NULL;
  do {
    r = ctx->handler(s);
  } while (r == REQUEST_DONE);
  if (r == REQUEST_AGAIN) {
    return NGX_STREAM_REQUEST_AGAIN;
  }
  if (r == NGX_STREAM_REQUEST_ERROR) {
    return NGX_STREAM_REQUEST_ERROR;
  }
  
  ctx->r = NULL;
  
  return r;
}

static void build_head(ngx_stream_request_t* r) {
  ngx_int_t len = ngx_chain_len(r->data) + 4;
  ngx_chain_t* ch = ngx_pcalloc(r->pool, sizeof(ngx_chain_t));
  ch->buf = ngx_create_temp_buf(r->pool, 4);
  *(u_int32_t*)ch->buf->last = htonl((u_int32_t)len);
  ch->buf->last += 4;
  ch->next = r->data;
  r->data = ch;
}

static ngx_int_t handle_request(ngx_stream_request_t* r) {
  request_ctx* r_ctx = ngx_stream_request_get_module_ctx(r, this_module);
  
  if (r_ctx->type == HEART_BEAT) {
    return NGX_HANDLER_STOP;
  }
  
  return NGX_OK;
}

static ngx_int_t build_response(ngx_stream_request_t* r) {
  request_ctx* r_ctx = ngx_stream_request_get_module_ctx(r, this_module);
  
  ngx_log_debug0(NGX_LOG_DEBUG_STREAM, r->session->connection->log
                 , 0, "lencontent build_response");
  
  /**
   * 如果r_ctx == NULL, 则说明r 可能是由其他模块创建，这里补充创建r_ctx
   */
  if (r_ctx == NULL) {
    r_ctx = ngx_pcalloc(r->pool, sizeof(request_ctx));
    ngx_stream_request_set_ctx(r, r_ctx, this_module);
    r_ctx->type = DATA;
  }
  
  switch (r_ctx->type) {
    case HEART_BEAT:
      r->data->buf = ngx_create_temp_buf(r->pool, 4); // set 0
      ngx_memzero(r->data->buf->pos, 4);
      ngx_log_debug5(NGX_LOG_DEBUG_STREAM, r->session->connection->log
                     , 0, "session<%p> build lencontent heartbeat %d %d %d %d"
                     , r->session
                     , r->data->buf->pos[0], r->data->buf->pos[1]
                     , r->data->buf->pos[2], r->data->buf->pos[3]);
      
      r->data->next = NULL;
      r->data->buf->last += 4;
      break;
    default: // DATA
      build_head(r);
      break;
  }
  
  return NGX_OK;
}



