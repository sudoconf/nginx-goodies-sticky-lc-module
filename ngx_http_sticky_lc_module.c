
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

// load md5 sha1 toolkit function
#include "ngx_http_sticky_misc.h"

#if (NGX_UPSTREAM_CHECK_MODULE)
#include "ngx_http_upstream_check_handler.h"
#endif

#define NGX_LB_ALG_RR 1
#define NGX_LB_ALG_LC 2

/* define a peer */
typedef struct {
    ngx_http_upstream_rr_peer_t *rr_peer;
    ngx_str_t                    digest;
} ngx_http_sticky_peer_t;

/* the configuration structure */
typedef struct {
    ngx_http_upstream_srv_conf_t  uscf;

    ngx_str_t                     cookie_name;
    ngx_str_t                     cookie_domain;
    ngx_str_t                     cookie_path;
    time_t                        cookie_expires;
    unsigned                      cookie_secure: 1;
    unsigned                      cookie_httponly: 1;

    ngx_str_t                     hmac_key;
    ngx_http_sticky_misc_hash_pt  hash;
    ngx_http_sticky_misc_hmac_pt  hmac;
    ngx_http_sticky_misc_text_pt  text;

    ngx_uint_t                    no_fallback;
    ngx_http_sticky_peer_t       *peers;

    ngx_uint_t                    lb_alg; /* select a load-balancing algorithm for default case */
} ngx_http_sticky_srv_conf_t;


/* the custom sticky struct used on each request */
typedef struct {
    /* the round robin data must be first */
    ngx_http_upstream_rr_peer_data_t   rrp;

    int                                selected_peer;
    int                                no_fallback;
    ngx_http_sticky_srv_conf_t        *sticky_conf;
    ngx_http_request_t                *request;

    ngx_uint_t                         lb_alg;
} ngx_http_sticky_peer_data_t;


static char *ngx_http_sticky_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void *ngx_http_sticky_create_conf(ngx_conf_t *cf);
static ngx_int_t ngx_http_init_sticky_peer(ngx_http_request_t *r,     ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_get_sticky_peer(ngx_peer_connection_t *pc, void *data);
/* INFO: may confused with function in src/http/modules/ngx_http_upstream_least_conn_module.c */
static ngx_int_t ngx_http_upstream_get_least_conn_peer(ngx_peer_connection_t *pc, void *data);

static ngx_command_t  ngx_http_sticky_commands[] = {
    {
        ngx_string("sticky"),
        NGX_HTTP_UPS_CONF | NGX_CONF_ANY,
        ngx_http_sticky_set,
        0,
        0,
        NULL
    },
    ngx_null_command
};


static ngx_http_module_t  ngx_http_sticky_lc_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    ngx_http_sticky_create_conf,           /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_sticky_lc_module = {
    NGX_MODULE_V1,
    &ngx_http_sticky_lc_module_ctx,        /* module context */
    ngx_http_sticky_commands,              /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


/*
 * function called by the upstream module to init itself.
 * it's called once per instance.
 */
ngx_int_t
ngx_http_init_upstream_sticky(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_rr_peers_t *rr_peers;
    ngx_http_sticky_srv_conf_t *conf;
    ngx_uint_t i;

    /* call the rr module on wich the sticky module is based on */
    if( NGX_OK != ngx_http_upstream_init_round_robin(cf, us) ) {
        return NGX_ERROR;
    }

    /* calculate each peer digest once and save */
    rr_peers = us->peer.data;

    /* do nothing there's only one peer */
    if( rr_peers->number <= 1 || rr_peers->single ) {
        return NGX_OK;
    }

    /* tell the upstream module to call ngx_http_init_sticky_peer when it inits peer */
    us->peer.init = ngx_http_init_sticky_peer;

    conf = ngx_http_conf_upstream_srv_conf( us, ngx_http_sticky_lc_module );

    /* if 'index', no need to alloc and generate digest */
    if( !conf->hash && !conf->hmac && !conf->text ) {
        conf->peers = NULL;
        return NGX_OK;
    }

    /* create our own upstream indexes */
    conf->peers = ngx_pcalloc( cf->pool, sizeof(ngx_http_sticky_peer_t) * rr_peers->number );

    if( NULL == conf->peers ) {
        return NGX_ERROR;
    }

    /* parse each peer and generate digest if necessary */
    for(i = 0; i < rr_peers->number; i++) {
        conf->peers[i].rr_peer = &rr_peers->peer[i];

        if(conf->hmac) {
            /* generate hmac */
            conf->hmac(cf->pool, rr_peers->peer[i].sockaddr, rr_peers->peer[i].socklen, &conf->hmac_key,
                       &conf->peers[i].digest);

        } else if(conf->text) {
            /* generate text */
            conf->text(cf->pool, rr_peers->peer[i].sockaddr, &conf->peers[i].digest);

        } else {
            /* generate hash */
            conf->hash(cf->pool, rr_peers->peer[i].sockaddr, rr_peers->peer[i].socklen, &conf->peers[i].digest);
        }

    }

    return NGX_OK;
}

/*
 * function called by the upstream module when it inits each peer
 * it's called once per request
 */
static ngx_int_t
ngx_http_init_sticky_peer(ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_sticky_peer_data_t  *iphp;
    ngx_str_t                     route;
    ngx_uint_t                    i;
    ngx_int_t                     n;

    /* alloc custom sticky struct */
    iphp = ngx_palloc( r->pool, sizeof(ngx_http_sticky_peer_data_t) );

    if( NULL == iphp ) {
        return NGX_ERROR;
    }

    /* attach it to the request upstream data */
    r->upstream->peer.data = &iphp->rrp;

    /* call the rr module on which the sticky is based on */
    if( NGX_OK != ngx_http_upstream_init_round_robin_peer(r, us) ) {
        return NGX_ERROR;
    }

    /* set the callback to select the next peer to use */
    r->upstream->peer.get = ngx_http_get_sticky_peer;

    /* init the custom sticky struct */
    iphp->selected_peer = -1;
    iphp->no_fallback = 0;
    iphp->sticky_conf = ngx_http_conf_upstream_srv_conf( us, ngx_http_sticky_lc_module );
    iphp->request = r;

    /* check weather a cookie is present or not and save it */
    if( NGX_DECLINED !=
            ngx_http_parse_multi_header_lines( &r->headers_in.cookies, &iphp->sticky_conf->cookie_name, &route) ) {

        /* a route cookie has been found. Let's give it a try */
        ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                      "[sticky/init_sticky_peer] got cookie route=%V, let's try to find a matching peer", &route);

        /* hash, hmac or text, just compare digest */
        if( iphp->sticky_conf->hash || iphp->sticky_conf->hmac || iphp->sticky_conf->text ) {

            /* check internal struct has been set */
            if( NULL == iphp->sticky_conf->peers ) {
                /* log a warning, as it will continue without the sticky */
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                              "[sticky/init_sticky_peer] internal peers struct has not been set");
                return NGX_OK; /* return OK, in order to continue */
            }

            /* search the digest found in the cookie in the peer digest list */
            for(i = 0; i < iphp->rrp.peers->number; i++) {

                /* ensure the both len are equal and > 0 */
                if( iphp->sticky_conf->peers[i].digest.len != route.len || route.len <= 0 ) {
                    continue;
                }

                if( 0 == ngx_strncmp(iphp->sticky_conf->peers[i].digest.data, route.data, route.len) ) {
                    /* we found a match */
                    iphp->selected_peer = i;
                    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                                  "[sticky/init_sticky_peer] the route \"%V\" matches peer at index %ui", &route, i);
                    return NGX_OK;
                }
            }

        } else {

            /* switch back to index, convert cookie data to integer and ensure it corresponds to a valid peer */
            n = ngx_atoi( route.data, route.len );

            if( NGX_ERROR == n ) {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                              "[sticky/init_sticky_peer] unable to convert the route \"%V\" to an integer value", &route);
            } else if( n >= 0 && n < (ngx_int_t)iphp->rrp.peers->number ) {
                /* got one valid peer number */
                ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                              "[sticky/init_sticky_peer] the route \"%V\" matches peer at index %i", &route, n);
                iphp->selected_peer = n;
                return NGX_OK;
            }
        }

        /* found cookie, but no corresponding peer was found, continue with rr */
        ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                      "[sticky/init_sticky_peer] route \"%V\" doesn't match any peer. Ignoring it ...", &route);
        return NGX_OK;
    }

    /* related cookies not found */
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                  "[sticky/init_sticky_peer] route cookie not found", &route);
    return NGX_OK; /* return OK, in order to continue */
}

/*
 * function called by the upstream module to choose the next peer to use
 * called at least one time per request
 */
static ngx_int_t
ngx_http_get_sticky_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_sticky_peer_data_t  *iphp = data;
    ngx_http_sticky_srv_conf_t   *conf = iphp->sticky_conf;

    ngx_int_t                     selected_peer = -1;
    time_t                        now = ngx_time();
    uintptr_t                     m = 0;
    ngx_uint_t                    n = 0, i;
    ngx_http_upstream_rr_peer_t  *peer = NULL;

    ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                  "[sticky/get_sticky_peer] get sticky peer, try: %ui, n_peers: %ui, no_fallback: %ui/%ui",
                  pc->tries, iphp->rrp.peers->number, conf->no_fallback, iphp->no_fallback);

    if( iphp->selected_peer >= 0  /* has got a selected peer */
            && iphp->selected_peer < (ngx_int_t)iphp->rrp.peers->number /* legal peer number */
            && !iphp->rrp.peers->single ) { /* has multiple peers */

        ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                      "[sticky/get_sticky_peer] let's try the selected peer (%i)", iphp->selected_peer);

        /* whether selected_peer has been tried */
        n = iphp->selected_peer / (8 * sizeof(uintptr_t));
        m = (uintptr_t) 1 << iphp->selected_peer % (8 * sizeof(uintptr_t)); /* get 0001 0010 0100 1000 from 0 1 2 3 */

        if( 0 == (iphp->rrp.tried[n] & m) ) {
            peer = &iphp->rrp.peers->peer[iphp->selected_peer];

            if( peer->down ) {
                iphp->no_fallback = conf->no_fallback;
                ngx_log_error(NGX_LOG_NOTICE, pc->log, 0,
                              "[sticky/get_sticky_peer] selected peer is down and no_fallback is flagged");
                return NGX_BUSY;
            }

            if( conf->no_fallback ) {
                /* if enabled no_fallback ，server will return 504 when upstream is invalid */
                iphp->no_fallback = 1;

                /* reset fail_timeout after kicking out peer for enough time */
                if( (now - peer->accessed) > peer->fail_timeout ) {
                    peer->fails = 0;
                }

                /* peer failed */
                if( peer->max_fails > 0 && (peer->fails >= peer->max_fails) ) {
                    ngx_log_error(NGX_LOG_NOTICE, pc->log, 0,
                                  "[sticky/get_sticky_peer] selected peer is maked as failed ,no_fallback is flagged");
                    return NGX_BUSY;
                }
            }

            if( 0 == peer->max_fails || (peer->fails < peer->max_fails) ){
                selected_peer = (ngx_int_t)n;
            }
            else if( (now - peer->accessed) > peer->fail_timeout) {
                peer->fails = 0;
                selected_peer = (ngx_int_t)n;
            }
            /* peer is max_fails or time is less than fail_timeout */
            else {
                /* mark as tried in bitmap */
                iphp->rrp.tried[n] |= m;
            }
        }
    }

    /* have a valid peer, tell the upstream module to use it */
    if( peer && selected_peer >= 0 ) {
        ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                      "[sticky/get_sticky_peer] peer found at index %i", selected_peer);

#if defined(nginx_version) && nginx_version >= 1009000
        iphp->rrp.current = peer;
#else
        iphp->rrp.current = iphp->selected_peer;
#endif
        /* save peer to peer connection ,done */
        pc->cached = 0;
        pc->connection = NULL;
        pc->sockaddr = peer->sockaddr;
        pc->socklen = peer->socklen;
        pc->name = &peer->name;

        /* mark as tried */
        iphp->rrp.current->conns ++;
        iphp->rrp.tried[n] |= m;

    }
    /* peer is NULL or selected_peer == -1 means that no previous peer is valid */
    else {
        ngx_int_t ret = NGX_ERROR;

        /* check fallback flag */
        if( iphp->no_fallback ) {
            ngx_log_error(NGX_LOG_NOTICE, pc->log, 0, "[sticky/get_sticky_peer] No fallback in action !");
            return NGX_BUSY;
        }

        if( NGX_LB_ALG_RR == conf->lb_alg ) {

            iphp->lb_alg = NGX_LB_ALG_RR;
            ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[sticky/get_sticky_peer_rr] LB_RR ");

            ret = ngx_http_upstream_get_round_robin_peer( pc, &iphp->rrp );

        } else if( NGX_LB_ALG_LC == conf->lb_alg ) {

            iphp->lb_alg = NGX_LB_ALG_LC;
            ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[sticky/get_sticky_peer_lc] LB_LC ");

            ret = ngx_http_upstream_get_least_conn_peer( pc, &iphp->rrp );

        } else {
            return NGX_BUSY;
        }

        if( NGX_OK != ret ) {
            ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                          "[sticky/get_sticky_peer_rr] ngx_http_upstream_get_round_robin_peer returned %i", ret);
            return ret;
        }

        /* search for the choosen peer in order to set the cookie */
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[sticky/get_sticky_peer_lc] get cookie 0");

        for( i = 0; i < iphp->rrp.peers->number; i++ ) {

            /* check sockaddr and socklen */
            if( iphp->rrp.peers->peer[i].sockaddr == pc->sockaddr
                    && iphp->rrp.peers->peer[i].socklen == pc->socklen ) {

                /* when enabled hash, write digest str to cookie */
                if( conf->hash || conf->hmac || conf->text ) {
                    ngx_http_sticky_misc_set_cookie(iphp->request, &conf->cookie_name, &conf->peers[i].digest,
                                                    &conf->cookie_domain, &conf->cookie_path, conf->cookie_expires,
                                                    conf->cookie_secure, conf->cookie_httponly);
                    ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                                  "[sticky/get_sticky_peer_lc]set cookie \"%V\" value=\"%V\" index=%ui",
                                  &conf->cookie_name, &conf->peers[i].digest, i);
                } else { /* when disabled hash , write i to cookie */
                    ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                            "[sticky/get_sticky_peer_lc] cookie disabled, write %ui to cookie", i);
                    ngx_str_t route;
                    ngx_uint_t tmp = i;
                    route.len = 0;

                    do {
                        route.len++;
                    } while( tmp /= 10 );

                    route.data = ngx_pcalloc( iphp->request->pool, sizeof(u_char) * (route.len + 1) );

                    if( NULL == route.data ) {
                        break;
                    }

                    ngx_snprintf( route.data, route.len, "%d", i );
                    route.len = ngx_strlen(route.data);
                    ngx_http_sticky_misc_set_cookie(iphp->request, &conf->cookie_name, &route, &conf->cookie_domain,
                            &conf->cookie_path, conf->cookie_expires, conf->cookie_secure, conf->cookie_httponly);
                    ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                                  "[sticky/get_sticky_peer_lc] set cookie \"%V\" value=\"%V\" index=%ui",
                                  &conf->cookie_name, &tmp, i);
                }

                break; /* found and hopefully the cookie have been set */
            }
        }
    }

    /* reset the selection in order to bypass the sticky module
     * when the upstream module will try another peers if necessary */
    iphp->selected_peer = -1;

    return NGX_OK;
}
static ngx_int_t
ngx_http_upstream_get_least_conn_peer( ngx_peer_connection_t *pc, void *data )
{
    ngx_http_upstream_rr_peer_data_t *rrp = data;

    time_t                        now = ngx_time();
    uintptr_t                     m = 0;
    ngx_int_t                     total = 0, rc = NGX_ERROR;
    ngx_uint_t                    n = 0, i;
    ngx_uint_t                    p, many;
    ngx_http_upstream_rr_peer_t  *peer = NULL, *best = NULL;
    ngx_http_upstream_rr_peers_t *peers = NULL;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
            "[sticky/get_least_conn_peer] get least conn peer, try: %ui", pc->tries);

    if ( rrp->peers->single ) {
        return ngx_http_upstream_get_round_robin_peer(pc, rrp);
    }

    pc->cached = 0;
    pc->connection = NULL;

    peers = rrp->peers;   /* get all peers from upstream */

    ngx_http_upstream_rr_peers_wlock(peers);

#if( NGX_SUPPRESS_WARN )
    many = 0;
    p = 0;
#endif

    /* traversal all valid peer and get a best peer with weighted least conns */
    for(peer = peers->peer, i = 0;
            peer;
            peer = peer->next, i++) {
        ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0,
            "[sticky/get_least_conn_peer] peer no: %ui peer conns: %ui peer weight: %ui", i,  peer->conns, peer->weight );

        n = i / (8 * sizeof(uintptr_t));
        m = (uintptr_t) 1 << i % (8 * sizeof(uintptr_t));

        if( rrp->tried[n] & m || peer->down ) {
            continue;
        }

        if(peer->max_fails
                && peer->fails >= peer->max_fails
                && now - peer->checked <= peer->fail_timeout)
        {
            continue;
        }

#if defined(nginx_version) && nginx_version >= 1011005
        if( peer->max_conns && peer->conns >= peer->max_conns ) {
            continue;
        }
#endif

        /*
         * select peer with least number of connections; if there are
         * multiple peers with the same number of connections, select
         * based on round-robin
         */
        if( NULL == best || ( peer->conns * best->weight < best->conns * peer->weight) ) {
#if 0
            if( NULL != best ) {
                ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                    "[sticky/get_least_conn_peer] best conns: %ui best weight: %ui", best->conns, best->weight );
            }
            ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                "[sticky/get_least_conn_peer] peer conns: %ui peer weight: %ui", peer->conns, peer->weight );
#endif
            best = peer;
            many = 0;
            p = i;
        } else if( peer->conns * best->weight == best->conns * peer->weight )
        {
            many = 1;
        }
    }

    if( NULL == best ) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                       "[sticky/get_least_conn_peer] no least conn peer found");
        if( peers->next ) {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                            "[sticky/get_least_conn_peer] get least conn peer, backup servers");

            rrp->peers = peers->next;
            n = ( rrp->peers->number+(8*sizeof(uintptr_t)-1) ) / (8 * sizeof(uintptr_t));

            for(i = 0; i < n; i++) {
                rrp->tried[i] = 0;
            }

            ngx_http_upstream_rr_peers_unlock(peers);

            rc = ngx_http_get_sticky_peer(pc, rrp);

            if( NGX_BUSY != rc ) {
                return rc;
            }

            ngx_http_upstream_rr_peers_wlock(peers);
        }

        ngx_http_upstream_rr_peers_unlock(peers);

        pc->name = peers->name;

        return NGX_BUSY;
    }

    if( many ) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[sticky/get_least_conn_peer] got many least conn peers");

        for(peer = best, i = p;
                peer;
                peer = peer->next, i++) {
            n = i / (8 * sizeof(uintptr_t));
            m = (uintptr_t) 1 << i % (8 * sizeof(uintptr_t));

            if( (rrp->tried[n] & m) || peer->down) {
                continue;
            }

            if( (peer->conns * best->weight) != (best->conns * peer->weight) ) {
                continue;
            }

            if(peer->max_fails
                    && peer->fails >= peer->max_fails
                    && now - peer->checked <= peer->fail_timeout) {
                continue;
            }

#if defined(nginx_version) && nginx_version >= 1011005
            if( peer->max_conns && peer->conns >= peer->max_conns ) {
                continue;
            }
#endif

            peer->current_weight += peer->effective_weight;
            total += peer->effective_weight;

            if( peer->effective_weight < peer->weight ) {
                peer->effective_weight ++;
            }

            if( peer->current_weight > best->current_weight ) {
                best = peer;
                p = i;
            }
        }
    }

    best->current_weight -= total;

    if( (now - best->checked) > best->fail_timeout ) {
        best->checked = now;
    }

    pc->sockaddr = best->sockaddr;
    pc->socklen  = best->socklen;
    pc->name     = &best->name;

    best->conns ++;

    rrp->current = best;

    n = p / (8 * sizeof(uintptr_t));
    m = (uintptr_t) 1 << p % (8 * sizeof(uintptr_t));
    rrp->tried[n] |= m;

    ngx_log_debug(NGX_LOG_DEBUG_HTTP, pc->log, 0, "[sticky/get_least_conn_peer] set selected_peer as %ui", n);

    ngx_http_upstream_rr_peers_unlock(peers);

    return NGX_OK;
}

/*
 * Function called when the sticky command is parsed on the conf file
 */
static char *
ngx_http_sticky_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstream_srv_conf_t  *upstream_conf;

    ngx_http_sticky_srv_conf_t    *sticky_conf;
    ngx_uint_t i;
    ngx_str_t tmp;

    ngx_str_t name = ngx_string("route");
    ngx_str_t domain = ngx_string("");
    ngx_str_t path = ngx_string("/");
    ngx_str_t hmac_key = ngx_string("");
    time_t expires = NGX_CONF_UNSET;
    unsigned secure = 0;
    unsigned httponly = 0;
    ngx_uint_t no_fallback = 0;

    ngx_http_sticky_misc_hash_pt hash = NGX_CONF_UNSET_PTR;
    ngx_http_sticky_misc_hmac_pt hmac = NULL;
    ngx_http_sticky_misc_text_pt text = NULL;

    ngx_uint_t lb_alg = NGX_LB_ALG_RR;

    /* parse all elements */
    for( i = 1; i < cf->args->nelts; i++ ) {
        ngx_str_t *value = cf->args->elts;

        /* is "lb_alg=" is starting the argument ? */
        if( (u_char *)ngx_strstr(value[i].data, "lb_alg=") == value[i].data ) {

            if( value[i].len <= (sizeof("lb_alg=") - 1 )) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "[sticky/sticky_set] a value must be provided to \"lb_alg=\"");
                return NGX_CONF_ERROR;
            }

            /* extract value to temp */
            tmp.len =  value[i].len - ngx_strlen("lb_alg=");
            tmp.data = (u_char *)(value[i].data + sizeof("lb_alg=") - 1);

            /* is lb_alg=rr */
            if( 0 == ngx_strncmp(tmp.data, "rr", sizeof("rr") - 1) ) {
                lb_alg = NGX_LB_ALG_RR;
                continue;
            }

            /* is lb_alg=lc */
            if( 0 == ngx_strncmp(tmp.data, "lc", sizeof("lc") - 1) ) {
                lb_alg = NGX_LB_ALG_LC;
                continue;
            }
        }

        /* is "name=" is starting the argument ? */
        if( (u_char *)ngx_strstr(value[i].data, "name=") == value[i].data ) {

            /* do we have at least one char after "name=" ? */
            if( value[i].len <= (sizeof("name=") - 1 )) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "[sticky/sticky_set] a value must be provided to \"name=\"");
                return NGX_CONF_ERROR;
            }

            /* save what's after "name=" */
            name.len = value[i].len - ngx_strlen("name=");
            name.data = (u_char *)(value[i].data + sizeof("name=") - 1);
            continue;
        }

        /* is "domain=" is starting the argument ? */
        if( (u_char *)ngx_strstr(value[i].data, "domain=") == value[i].data ) {

            /* do we have at least one char after "domain=" ? */
            if( value[i].len <= ngx_strlen("domain=") ) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "[sticky/sticky_set] a value must be provided to \"domain=\"");
                return NGX_CONF_ERROR;
            }

            /* save what's after "domain=" */
            domain.len = value[i].len - ngx_strlen("domain=");
            domain.data = (u_char *)(value[i].data + sizeof("domain=") - 1);
            continue;
        }

        /* is "path=" is starting the argument ? */
        if( (u_char *)ngx_strstr(value[i].data, "path=") == value[i].data ) {

            /* do we have at least one char after "path=" ? */
            if( value[i].len <= ngx_strlen("path=") ) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "[sticky/sticky_set] a value must be provided to \"path=\"");
                return NGX_CONF_ERROR;
            }

            /* save what's after "domain=" */
            path.len = value[i].len - ngx_strlen("path=");
            path.data = (u_char *)(value[i].data + sizeof("path=") - 1);
            continue;
        }

        /* is "expires=" is starting the argument ? */
        if( (u_char *)ngx_strstr(value[i].data, "expires=") == value[i].data ) {

            /* do we have at least one char after "expires=" ? */
            if( value[i].len <= (sizeof("expires=") - 1 )) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "[sticky/sticky_set] a value must be provided to \"expires=\"");
                return NGX_CONF_ERROR;
            }

            /* extract value */
            tmp.len =  value[i].len - ngx_strlen("expires=");
            tmp.data = (u_char *)(value[i].data + sizeof("expires=") - 1);

            /* convert to time, save and validate */
            expires = ngx_parse_time(&tmp, 1);

            if( NGX_ERROR == expires || expires < 1 ) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "[sticky/sticky_set] invalid value for \"expires=\"");
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if( 0 == ngx_strncmp(value[i].data, "secure", 6) && 6 == value[i].len ) {
            secure = 1;
            continue;
        }

        if( 0 == ngx_strncmp(value[i].data, "httponly", 8) && 8 == value[i].len ) {
            httponly = 1;
            continue;
        }

        /* is "text=" is starting the argument ? */
        if( (u_char *)ngx_strstr(value[i].data, "text=") == value[i].data ) {

            /* only hash or hmac can be used, not both */
            if( hmac || hash != NGX_CONF_UNSET_PTR ) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "[sticky/sticky_set] please choose between \"hash=\", \"hmac=\" and \"text\"");
                return NGX_CONF_ERROR;
            }

            /* do we have at least one char after "name=" ? */
            if( value[i].len <= (sizeof("text=") - 1 )) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "[sticky/sticky_set] a value must be provided to \"text=\"");
                return NGX_CONF_ERROR;
            }

            /* extract value to temp */
            tmp.len =  value[i].len - ngx_strlen("text=");
            tmp.data = (u_char *)(value[i].data + sizeof("text=") - 1);

            /* is name=raw */
            if( 0 == ngx_strncmp(tmp.data, "raw", sizeof("raw") - 1) ) {
                text = ngx_http_sticky_misc_text_raw;
                continue;
            }

            /* is name=md5 */
            if( 0 == ngx_strncmp(tmp.data, "md5", sizeof("md5") - 1) ) {
                text = ngx_http_sticky_misc_text_md5;
                continue;
            }

            /* is name=sha1 */
            if( 0 == ngx_strncmp(tmp.data, "sha1", sizeof("sha1") - 1) ) {
                text = ngx_http_sticky_misc_text_sha1;
                continue;
            }

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "[sticky/sticky_set] wrong value for \"text=\": raw, md5 or sha1");
            return NGX_CONF_ERROR;
        }

        /* is "hash=" is starting the argument ? */
        if( (u_char *)ngx_strstr(value[i].data, "hash=") == value[i].data ) {

            /* only hash or hmac can be used, not both */
            if( hmac || text ) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "[sticky/sticky_set] please choose between \"hash=\", \"hmac=\" and \"text=\"");
                return NGX_CONF_ERROR;
            }

            /* do we have at least one char after "hash=" ? */
            if( value[i].len <= (sizeof("hash=") - 1 )) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "[sticky/sticky_set] a value must be provided to \"hash=\"");
                return NGX_CONF_ERROR;
            }

            /* extract value to temp */
            tmp.len =  value[i].len - ngx_strlen("hash=");
            tmp.data = (u_char *)(value[i].data + sizeof("hash=") - 1);

            /* is hash=index */
            if( 0 == ngx_strncmp(tmp.data, "index", sizeof("index") - 1) ) {
                hash = NULL;
                continue;
            }

            /* is hash=md5 */
            if( 0 == ngx_strncmp(tmp.data, "md5", sizeof("md5") - 1) ) {
                hash = ngx_http_sticky_misc_md5;
                continue;
            }

            /* is hash=sha1 */
            if( 0 == ngx_strncmp(tmp.data, "sha1", sizeof("sha1") - 1) ) {
                hash = ngx_http_sticky_misc_sha1;
                continue;
            }

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "[sticky/sticky_set] wrong value for \"hash=\": index, md5 or sha1");
            return NGX_CONF_ERROR;
        }

        /* is "hmac=" is starting the argument ? */
        if( (u_char *)ngx_strstr(value[i].data, "hmac=") == value[i].data ) {

            /* only hash or hmac can be used, not both */
            if( NGX_CONF_UNSET_PTR != hash || 0 != text ) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "[sticky/sticky_set] please choose between \"hash=\", \"hmac=\" and \"text\"");
                return NGX_CONF_ERROR;
            }

            /* do we have at least one char after "hmac=" ? */
            if( value[i].len <= (sizeof("hmac=") - 1 )) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "[sticky/sticky_set] a value must be provided to \"hmac=\"");
                return NGX_CONF_ERROR;
            }

            /* extract value */
            tmp.len =  value[i].len - ngx_strlen("hmac=");
            tmp.data = (u_char *)(value[i].data + sizeof("hmac=") - 1);

            /* is hmac=md5 ? */
            if( 0 == ngx_strncmp(tmp.data, "md5", sizeof("md5") - 1) ) {
                hmac = ngx_http_sticky_misc_hmac_md5;
                continue;
            }

            /* is hmac=sha1 ? */
            if( 0 == ngx_strncmp(tmp.data, "sha1", sizeof("sha1") - 1) ) {
                hmac = ngx_http_sticky_misc_hmac_sha1;
                continue;
            }

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "[sticky/sticky_set] wrong value for \"hmac=\": md5 or sha1");
            return NGX_CONF_ERROR;
        }

        /* is "hmac_key=" is starting the argument ? */
        if( (u_char *)ngx_strstr(value[i].data, "hmac_key=") == value[i].data ) {

            /* do we have at least one char after "hmac_key=" ? */
            if( value[i].len <= ngx_strlen("hmac_key=") ) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "[sticky/sticky_set] a value must be provided to \"hmac_key=\"");
                return NGX_CONF_ERROR;
            }

            /* save what's after "hmac_key=" */
            hmac_key.len = value[i].len - ngx_strlen("hmac_key=");
            hmac_key.data = (u_char *)(value[i].data + sizeof("hmac_key=") - 1);
            continue;
        }

        /* is "no_fallback" flag present ? */
        if( 0 == ngx_strncmp(value[i].data, "no_fallback", sizeof("no_fallback") - 1) ) {
            no_fallback = 1;
            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid arguement (%V)", &value[i]);

        return NGX_CONF_ERROR;
    }

    /* if has and hmac and name have not been set, default to md5 */
    if( NGX_CONF_UNSET_PTR == hash && NULL == hmac && NULL == text ) {
        hash = ngx_http_sticky_misc_md5;
    }

    /* don't allow meaning less parameters */
    if( hmac_key.len > 0 && hash != NGX_CONF_UNSET_PTR ) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "[sticky/sticky_set] \"hmac_key=\" is meaningless when \"hmac\" is used. Please remove it.");
        return NGX_CONF_ERROR;
    }

    /* ensure we have an hmac key if hmac's been set */
    if( 0 == hmac_key.len && hmac != NULL ) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "please specify \"hmac_key=\" when using \"hmac\"");
        return NGX_CONF_ERROR;
    }

    /* ensure hash is NULL to avoid conflicts later */
    if( NGX_CONF_UNSET_PTR == hash ) {
        hash = NULL;
    }

    /* save the sticky parameters */
    sticky_conf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_sticky_lc_module);
    sticky_conf->cookie_name = name;
    sticky_conf->cookie_domain = domain;
    sticky_conf->cookie_path = path;
    sticky_conf->cookie_expires = expires;
    sticky_conf->cookie_secure = secure;
    sticky_conf->cookie_httponly = httponly;
    sticky_conf->hash = hash;
    sticky_conf->hmac = hmac;
    sticky_conf->text = text;
    sticky_conf->hmac_key = hmac_key;
    sticky_conf->no_fallback = no_fallback;
    sticky_conf->lb_alg = lb_alg;
    sticky_conf->peers = NULL; /* ensure it's null before running */

    upstream_conf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    /*
     * ensure another upstream module has not been already loaded
     * peer.init_upstream is set to null and the upstream module use RR if not set
     * But this check only works when the other module is declared before sticky
     */
    if( NULL != upstream_conf->peer.init_upstream ) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "[sticky/sticky_set] You can't use sticky with another upstream module");
        return NGX_CONF_ERROR;
    }

    /* configure the upstream to get back to this module */
    upstream_conf->peer.init_upstream = ngx_http_init_upstream_sticky;

    upstream_conf->flags = NGX_HTTP_UPSTREAM_CREATE
#if defined(nginx_version) && nginx_version >= 1011005
                           | NGX_HTTP_UPSTREAM_MAX_CONNS
#endif
                           | NGX_HTTP_UPSTREAM_WEIGHT
                           | NGX_HTTP_UPSTREAM_MAX_FAILS
                           | NGX_HTTP_UPSTREAM_FAIL_TIMEOUT
                           | NGX_HTTP_UPSTREAM_DOWN
                           | NGX_HTTP_UPSTREAM_BACKUP;

    return NGX_CONF_OK;
}

/*
 * alloc stick configuration
 */
static void *
ngx_http_sticky_create_conf(ngx_conf_t *cf)
{
    ngx_http_sticky_srv_conf_t *conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_sticky_srv_conf_t));

    if( NULL == conf ) {
        return NGX_CONF_ERROR;
    }

    return conf;
}
