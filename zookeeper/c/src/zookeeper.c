/*
 * Copyright 2008, Yahoo! Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#ifndef DLL_EXPORT
#  define USE_STATIC_LIB
#endif

#if defined(__CYGWIN__)
#define USE_IPV6
#endif

#include <zookeeper.h>
#include <zookeeper.jute.h>
#include <proto.h>
#include "zk_adaptor.h"
#include "zk_log.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <stdarg.h>

const int ZOOKEEPER_WRITE = 1 << 0;
const int ZOOKEEPER_READ = 1 << 1;

const int EPHEMERAL = 1 << 0;
const int SEQUENCE = 1 << 1;

const int EXPIRED_SESSION_STATE = -112;
const int AUTH_FAILED_STATE = -113;
const int CONNECTING_STATE = 1;
const int ASSOCIATING_STATE = 2;
const int CONNECTED_STATE = 3;
static __attribute__ ((unused)) const char* state2String(int state){
    switch(state){
    case 0:
        return "CLOSED_STATE";
    case 1 /*CONNECTING_STATE*/:
        return "CONNECTING_STATE";
    case 2 /*ASSOCIATING_STATE*/:
        return "ASSOCIATING_STATE";
    case 3 /*CONNECTED_STATE*/:
        return "CONNECTED_STATE";
    case -112 /*EXPIRED_SESSION_STATE*/:
        return "EXPIRED_SESSION_STATE";
    case -113 /*AUTH_FAILED_STATE*/:
        return "AUTH_FAILED_STATE";
    }
    return "INVALID_STATE";
}

const int CREATED_EVENT = 1;
const int DELETED_EVENT = 2;
const int CHANGED_EVENT = 3;
const int CHILD_EVENT = 4;
const int SESSION_EVENT = -1;
const int NOTWATCHING_EVENT = -2;
static __attribute__ ((unused)) const char* watcherEvent2String(int ev){
    switch(ev){
    case 0:
        return "ERROR_EVENT";
    case 1 /*CREATED_EVENT*/:
        return "CREATED_EVENT";
    case 2 /*DELETED_EVENT*/:
        return "DELETED_EVENT";
    case 3 /*CHANGED_EVENT*/:
        return "CHANGED_EVENT";
    case 4 /*CHILD_EVENT*/:
        return "CHILD_EVENT";
    case -1 /*SESSION_EVENT*/:
        return "SESSION_EVENT";
    case -2 /*NOTWATCHING_EVENT*/:
        return "NOTWATCHING_EVENT";
    }
    return "INVALID_EVENT";
}

const int PERM_READ = 1 << 0;
const int PERM_WRITE = 1 << 1;
const int PERM_CREATE = 1 << 2;
const int PERM_DELETE = 1 << 3;
const int PERM_ADMIN = 1 << 4;
const int PERM_ALL = 0x1f;
struct Id ANYONE_ID_UNSAFE = {"world", "anyone"};
struct Id AUTH_IDS = {"auth", ""};
static struct ACL _OPEN_ACL_UNSAFE_ACL[] = {{0x1f, {"world", "anyone"}}};
static struct ACL _READ_ACL_UNSAFE_ACL[] = {{0x01, {"world", "anyone"}}};
static struct ACL _CREATOR_ALL_ACL_ACL[] = {{0x1f, {"auth", ""}}};
struct ACL_vector OPEN_ACL_UNSAFE = { 1, _OPEN_ACL_UNSAFE_ACL};
struct ACL_vector READ_ACL_UNSAFE = { 1, _READ_ACL_UNSAFE_ACL};
struct ACL_vector CREATOR_ALL_ACL = { 1, _CREATOR_ALL_ACL_ACL};

#define COMPLETION_VOID 0
#define COMPLETION_STAT 1
#define COMPLETION_DATA 2
#define COMPLETION_STRINGLIST 3
#define COMPLETION_ACLLIST 4
#define COMPLETION_STRING 5

/* predefined xid's values recognized as special by the server */
#define PING_XID -2
#define AUTH_XID -4

const char*err2string(int err);
static const char* format_endpoint_info(const struct sockaddr* ep);
static const char* format_current_endpoint_info(zhandle_t* zh);
static int add_completion(zhandle_t *zh, int xid, int completion_type, 
        const void *dc, const void *data, int add_to_front);
static int handle_socket_error_msg(zhandle_t *zh, int line, int rc,
    const char* format,...);
static void cleanup_bufs(zhandle_t *zh,int callCompletion,int rc);

static int disable_conn_permute=0; // permute enabled by default

static void *SYNCHRONOUS_MARKER = (void*)&SYNCHRONOUS_MARKER;
    
typedef struct _completion_list {
    int xid;
    int completion_type; /* one of the COMPLETION_* values */
    union {
       void_completion_t void_result;
       stat_completion_t stat_result;
       data_completion_t data_result;
       strings_completion_t strings_result;
       acl_completion_t acl_result;
       string_completion_t string_result;
    } c;
    const void *data;
    buffer_list_t *buffer;
    struct _completion_list *next;
} completion_list_t;

const void *zoo_get_context(zhandle_t *zh) 
{
    return zh->context;
}

void zoo_set_context(zhandle_t *zh, void *context)
{
    if (zh != NULL) {
        zh->context = context;
    }
}

int zoo_recv_timeout(zhandle_t *zh)
{
    return zh->recv_timeout;
}

static void init_auth_info(auth_info *auth)
{
    auth->scheme=NULL;
    auth->auth.buff=NULL;
    auth->auth.len=0;
    auth->state=0;
    auth->completion=0;
    auth->data=0;
}

static void free_auth_info(auth_info *auth)
{
    if(auth->scheme!=NULL)
        free(auth->scheme);
    deallocate_Buffer(&auth->auth);
    init_auth_info(auth);
}

int is_unrecoverable(zhandle_t *zh)
{
    return (zh->state<0)? ZINVALIDSTATE: ZOK;
}
/**
 * Frees and closes everything associated with a handle,
 * including the handle itself.
 */
static void destroy(zhandle_t *zh)
{
    if (zh == NULL) {
        return;
    }
    /* call any outstanding completions with a special error code */
    cleanup_bufs(zh,1,ZCLOSING);
    if (zh->hostname != 0) {
        free(zh->hostname);
        zh->hostname = NULL;
    }
    if (zh->fd != -1) {
        close(zh->fd);
        zh->fd = -1;
        zh->state = 0;
    }
    if (zh->addrs != 0) {
        free(zh->addrs);
        zh->addrs = NULL;
    }
    free_auth_info(&zh->auth);
}

static void setup_random()
{
    int seed;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd == -1) {
        seed = getpid();
    } else {
        read(fd, &seed, sizeof(seed));
        close(fd);
    }
    srandom(seed);
}

/**
 * fill in the addrs array of the zookeeper servers in the zhandle. after filling
 * them in, we will permute them for load balancing.
 */
int getaddrs(zhandle_t *zh)
{
    struct hostent *he;
    struct sockaddr *addr;
    struct sockaddr_in *addr4;
    struct sockaddr_in6 *addr6;
    char **ptr;
    char *hosts = strdup(zh->hostname);
    char *host;
    int i;
    int rc;
    int alen = 0; /* the allocated length of the addrs array */
    
    zh->addrs_count = 0;
    if (zh->addrs) {
        free(zh->addrs);
        zh->addrs = 0;
    }
    if (!hosts) {
        LOG_ERROR(("out of memory"));
        errno=ENOMEM;
        return ZSYSTEMERROR;
    }
    zh->addrs = 0;
    host=strtok(hosts, ",");
    while(host) {
        char *port_spec = strchr(host, ':');
        char *end_port_spec;
        int port;
        if (!port_spec) {
            LOG_ERROR(("no port in %s", host));
            errno=EINVAL;
            rc=ZBADARGUMENTS;
            goto fail;
        }
        *port_spec = '\0';
        port_spec++;
        port = strtol(port_spec, &end_port_spec, 0);
        if (!*port_spec || *end_port_spec) {
            LOG_ERROR(("invalid port in %s", host));
            errno=EINVAL;
            rc=ZBADARGUMENTS;
            goto fail;
        }
        he = gethostbyname(host);
        if (!he) {
            LOG_ERROR(("could not resolve %s", host));
            errno=EINVAL;
            rc=ZBADARGUMENTS;
            goto fail;
        }
        
        /* Setup the address array */
        for(ptr = he->h_addr_list;*ptr != 0; ptr++) {
            if (zh->addrs_count == alen) {
                void *tmpaddr;
                alen += 16;
                tmpaddr = realloc(zh->addrs, sizeof(*zh->addrs)*alen);
                if (tmpaddr == 0) {
                    LOG_ERROR(("out of memory"));
                    errno=ENOMEM;
                    rc=ZSYSTEMERROR;
                    goto fail;
                }
                zh->addrs=tmpaddr;
            }
            addr = &zh->addrs[zh->addrs_count];
            addr4 = (struct sockaddr_in*)addr;
            addr6 = (struct sockaddr_in6*)addr;
            addr->sa_family = he->h_addrtype;
            if (addr->sa_family == AF_INET) {
                addr4->sin_port = htons(port);
                memset(&addr4->sin_zero, 0, sizeof(addr4->sin_zero));
                memcpy(&addr4->sin_addr, *ptr, he->h_length);
                zh->addrs_count++;
#if defined(AF_INET6)
            } else if (addr->sa_family == AF_INET6) {
                addr6->sin6_port = htons(port);
                addr6->sin6_scope_id = 0;
                addr6->sin6_flowinfo = 0;
                memcpy(&addr6->sin6_addr, *ptr, he->h_length);
                zh->addrs_count++;
#endif
            } else {
                LOG_WARN(("skipping unknown address family %x for %s", 
                        addr->sa_family, zh->hostname)); 
            }
        }
        host = strtok(0, ",");
    }
    free(hosts);
    if(!disable_conn_permute){
        setup_random();
        /* Permute */
        for(i = 0; i < zh->addrs_count; i++) {
            struct sockaddr *s1 = zh->addrs + random()%zh->addrs_count;
            struct sockaddr *s2 = zh->addrs + random()%zh->addrs_count;
            if (s1 != s2) {
                struct sockaddr t = *s1;
                *s1 = *s2;
                *s2 = t;
            }
        }
    }
    return ZOK;
fail:
    if (zh->addrs) {
        free(zh->addrs);
        zh->addrs=0;
    }
    if (hosts) {
        free(hosts);
    }
    return rc;    
}

const clientid_t *zoo_client_id(zhandle_t *zh)
{
    return &zh->client_id;
}

static void null_watcher_fn(zhandle_t* p1, int p2, int p3,const char* p4){}

watcher_fn zoo_set_watcher(zhandle_t *zh,watcher_fn newFn)
{
    watcher_fn oldWatcher=zh->watcher;
    if (newFn) {
       zh->watcher = newFn;
    } else {
       zh->watcher = null_watcher_fn;
    }
    return oldWatcher;
}

/**
 * Create a zookeeper handle associated with the given host and port.
 */
zhandle_t *zookeeper_init(const char *host, watcher_fn watcher,
  int recv_timeout, const clientid_t *clientid, void *context, int flags)
{
    int errnosave;
    zhandle_t *zh = calloc(1, sizeof(*zh));
    if (!zh) {
        return 0;
    }
    zh->fd = -1;
    zh->state = 0;
    zh->context = context;
    zh->recv_timeout = recv_timeout;
    if (watcher) {
       zh->watcher = watcher;
    } else {
       zh->watcher = null_watcher_fn;
    }
    zh->hostname = strdup(host);
    if (zh->hostname == 0) {
        goto abort;
    }
    if(getaddrs(zh)!=0) {
        goto abort;
    }
    zh->connect_index = 0;
    if (clientid) {
        memcpy(&zh->client_id, clientid, sizeof(zh->client_id));
    } else {
        memset(&zh->client_id, 0, sizeof(zh->client_id));
    }
    zh->primer_buffer.buffer = zh->primer_storage_buffer;
    zh->primer_buffer.curr_offset = 0;
    zh->primer_buffer.len = sizeof(zh->primer_storage_buffer);
    zh->primer_buffer.next = 0;
    zh->last_zxid = 0;
    if (adaptor_init(zh) == -1) {
        goto abort;
    }
    return zh;
abort:
    errnosave=errno;
    destroy(zh);
    free(zh);
    errno=errnosave;
    return 0;
}

static buffer_list_t *allocate_buffer(char *buff, int len)
{
    buffer_list_t *buffer = calloc(1, sizeof(*buffer));
    if (buffer == 0) 
        return 0;

    buffer->len = len==0?sizeof(*buffer):len;
    buffer->curr_offset = 0;
    buffer->buffer = buff;
    buffer->next = 0;
    return buffer;
}

static void free_buffer(buffer_list_t *b)
{
    if (!b) {
        return;
    }
    if (b->buffer) {
        free(b->buffer);
    }
    free(b);
}

static buffer_list_t *dequeue_buffer(buffer_head_t *list)
{
    buffer_list_t *b;
    lock_buffer_list(list);
    b = list->head;
    if (b) {
        list->head = b->next;
        if (!list->head) {
            assert(b == list->last);
            list->last = 0;
        }
    }
    unlock_buffer_list(list);
    return b;
}

static int remove_buffer(buffer_head_t *list)
{
    buffer_list_t *b = dequeue_buffer(list);
    if (!b) {
        return 0;
    }
    free_buffer(b);
    return 1;
}

static void queue_buffer(buffer_head_t *list, buffer_list_t *b, int add_to_front)
{
    b->next = 0;
    lock_buffer_list(list);
    if (list->head) {
        assert(list->last);
        // The list is not empty
        if (add_to_front) {
            b->next = list->head;
            list->head = b;
        } else {
            list->last->next = b;
            list->last = b;
        }
    }else{
        // The list is empty
        assert(!list->head);
        list->head = b;
        list->last = b;
    }
    unlock_buffer_list(list);
}

static int queue_buffer_bytes(buffer_head_t *list, char *buff, int len)
{
    buffer_list_t *b  = allocate_buffer(buff,len);
    if (!b)
        return ZSYSTEMERROR;
    queue_buffer(list, b, 0);
    return ZOK;
}

static int queue_front_buffer_bytes(buffer_head_t *list, char *buff, int len)
{
    buffer_list_t *b  = allocate_buffer(buff,len);
    if (!b)
        return ZSYSTEMERROR;
    queue_buffer(list, b, 1);
    return ZOK;
}

static __attribute__ ((unused)) int get_queue_len(buffer_head_t *list)
{
    int i;
    buffer_list_t *ptr;
    lock_buffer_list(list);
    ptr = list->head;
    for (i=0; ptr!=0; ptr=ptr->next, i++)
        ;
    unlock_buffer_list(list);
    return i;
}
/* returns:
 * -1 if send failed, 
 * 0 if send would block while sending the buffer (or a send was incomplete),
 * 1 if success
 */
static int send_buffer(int fd, buffer_list_t *buff)
{
    int len = buff->len;
    int off = buff->curr_offset;
    int rc = -1;
    if (off < 4) {
        /* we need to send the length at the beginning */
        int nlen = htonl(len);
        char *b = (char*)&nlen;
        rc = send(fd, b + off, sizeof(nlen) - off, 0);
        if (rc == -1) {
            if (errno != EAGAIN) {
                return -1;
            } else {
                return 0;
            }
        } else {
            buff->curr_offset  += rc;
        }
        off = buff->curr_offset;
    }
    if (off >= 4) {
        /* want off to now represent the offset into the buffer */
        off -= sizeof(buff->len);
        rc = send(fd, buff->buffer + off, len - off, 0);
        if (rc == -1) {
            if (errno != EAGAIN) {
                return -1;
            }
        } else {
            buff->curr_offset += rc;
        }
    }
    return buff->curr_offset == len + sizeof(buff->len);
}

/* returns:
 * -1 if recv call failed, 
 * 0 if recv would block,
 * 1 if success
 */
static int recv_buffer(int fd, buffer_list_t *buff)
{
    int off = buff->curr_offset;
    int rc = 0;
    //fprintf(LOGSTREAM, "rc = %d, off = %d, line %d\n", rc, off, __LINE__);
                
    /* if buffer is less than 4, we are reading in the length */
    if (off < 4) {
        char *buffer = (char*)&(buff->len);
        rc = recv(fd, buffer+off, sizeof(int)-off, 0);
        //fprintf(LOGSTREAM, "rc = %d, off = %d, line %d\n", rc, off, __LINE__);
        switch(rc) {
        case 0:
            errno = EHOSTDOWN;
        case -1:
            if (errno == EAGAIN) {
                return 0;
            }
            return -1;
        default:
            buff->curr_offset += rc;
        }
        off = buff->curr_offset;
        if (buff->curr_offset == sizeof(buff->len)) {
            buff->len = ntohl(buff->len);
            buff->buffer = calloc(1, buff->len);
        }
    }
    if (buff->buffer) {
        /* want off to now represent the offset into the buffer */
        off -= sizeof(buff->len);
        
        rc = recv(fd, buff->buffer+off, buff->len-off, 0);
        switch(rc) {
        case 0:
            errno = EHOSTDOWN;
        case -1:
            if (errno == EAGAIN) {
                break;
            }
            return -1;
        default:
            buff->curr_offset += rc;
        }
    }
    return buff->curr_offset == buff->len + sizeof(buff->len);
}

void free_buffers(buffer_head_t *list)
{
    while (remove_buffer(list))
        ;
}

void free_completions(zhandle_t *zh,int callCompletion,int rc) 
{
    completion_head_t tmp_list;
    lock_completion_list(&zh->sent_requests);
    tmp_list = zh->sent_requests;
    zh->sent_requests.head = 0;
    zh->sent_requests.last = 0;
    unlock_completion_list(&zh->sent_requests);
    while (tmp_list.head) {
        completion_list_t *cptr = tmp_list.head;
        
        tmp_list.head = cptr->next;
        if (cptr->c.data_result == SYNCHRONOUS_MARKER) {
            struct sync_completion
                        *sc = (struct sync_completion*)cptr->data;
            sc->rc = rc;
            notify_sync_completion(sc);
            zh->outstanding_sync--;
        } else if (callCompletion) {
            switch (cptr->completion_type) {
            case COMPLETION_DATA:
                LOG_DEBUG(("Calling COMPLETION_DATA rc=%d",rc));
                cptr->c.data_result(rc, 0, 0, 0, cptr->data);
                break;
            case COMPLETION_STAT:
                LOG_DEBUG(("Calling COMPLETION_STAT rc=%d",rc));
                cptr->c.stat_result(rc, 0, cptr->data);
                break;
            case COMPLETION_STRINGLIST:
                LOG_DEBUG(("Calling COMPLETION_STRINGLIST rc=%d",rc));
                cptr->c.strings_result(rc, 0, cptr->data);
                break;
            case COMPLETION_STRING:
                LOG_DEBUG(("Calling COMPLETION_STRING rc=%d",rc));
                cptr->c.string_result(rc, 0, cptr->data);
                break;
            case COMPLETION_ACLLIST:
                LOG_DEBUG(("Calling COMPLETION_ACLLIST rc=%d",rc));
                cptr->c.acl_result(rc, 0, 0, cptr->data);
                break;
            case COMPLETION_VOID:
                LOG_DEBUG(("Calling COMPLETION_VOID rc=%d",rc));
                // We want to skip the ping
                if (cptr->xid != PING_XID)
                    cptr->c.void_result(rc, cptr->data);
                break;
            }
        }
        free(cptr);
    }
}

static void cleanup_bufs(zhandle_t *zh,int callCompletion,int rc)
{
    enter_critical(zh);
    free_buffers(&zh->to_send);
    free_buffers(&zh->to_process);
    free_completions(zh,callCompletion,rc);
    leave_critical(zh);
    if (zh->input_buffer && zh->input_buffer != &zh->primer_buffer) {
        free_buffer(zh->input_buffer);
        zh->input_buffer = 0;
    }
}

static void handle_error(zhandle_t *zh,int rc)
{
    close(zh->fd);
    if (is_unrecoverable(zh)) {
        LOG_DEBUG(("Calling a watcher for a SESSION_EVENT and the state=%s",
                state2String(zh->state)));
        zh->watcher(zh, SESSION_EVENT, zh->state, 0);
    } else if (zh->state == CONNECTED_STATE) {
        LOG_DEBUG(("Calling a watcher for a SESSION_EVENT and the state=CONNECTING_STATE"));
        zh->watcher(zh, SESSION_EVENT, CONNECTING_STATE, 0);
    }
    cleanup_bufs(zh,1,rc);
    zh->fd = -1;
    zh->connect_index++;
    if (!is_unrecoverable(zh)) {
        zh->state = 0;
    }
}

static int handle_socket_error_msg(zhandle_t *zh, int line, int rc,
        const char* format, ...)
{
    if(logLevel>=LOG_LEVEL_ERROR){
        va_list va;
        char buf[1024];
        va_start(va,format);
        vsnprintf(buf, sizeof(buf)-1,format,va);
        log_message(LOG_LEVEL_ERROR,line,__func__,
            format_log_message("Socket [%s] zk retcode=%d, errno=%d(%s): %s",
            format_current_endpoint_info(zh),rc,errno,strerror(errno),buf));
        va_end(va);
    }
    handle_error(zh,rc);
    return rc;
}

static void auth_completion_func(int rc, zhandle_t* zh)
{
    if(zh==NULL)
        return;
    
    if(rc!=0){
        LOG_ERROR(("Authentication scheme %s failed. Connection closed.",
                zh->auth.scheme));
        zh->state=AUTH_FAILED_STATE;
    }else{
        zh->auth.state=1;  // active
        LOG_INFO(("Authentication scheme %s succeeded", zh->auth.scheme));
    }
    // chain call user's completion function
    if(zh->auth.completion!=0){
        zh->auth.completion(rc,zh->auth.data);
        zh->auth.completion=0;
    }
}

static int send_auth_info(zhandle_t *zh)
{    
    struct oarchive *oa;
    struct RequestHeader h = { .xid = AUTH_XID, .type = SETAUTH_OP};
    struct AuthPacket req;
    int rc;

    if(zh->auth.scheme==NULL)
      return ZOK; // there is nothing to send

    oa = create_buffer_oarchive();
    req.type=0;   // ignored by the server
    req.scheme = zh->auth.scheme;
    req.auth = zh->auth.auth;
    rc = serialize_RequestHeader(oa, "header", &h);
    rc = rc < 0 ? rc : serialize_AuthPacket(oa, "req", &req);
    /* add this buffer to the head of the send queue */
    rc = rc < 0 ? rc : queue_front_buffer_bytes(&zh->to_send, get_buffer(oa),
            get_buffer_len(oa));
    /* We queued the buffer, so don't free it */   
    close_buffer_oarchive(&oa, 0);
    
    LOG_DEBUG(("Sending auth info request to %s",format_current_endpoint_info(zh)));
    return (rc < 0)?ZMARSHALLINGERROR:ZOK;
}

static int serialize_prime_connect(struct connect_req *req, char* buffer){
    //this should be the order of serialization
    int offset = 0;
    req->protocolVersion = htonl(req->protocolVersion);
    memcpy(buffer + offset, &req->protocolVersion, sizeof(req->protocolVersion));
    offset = offset +  sizeof(req->protocolVersion);
    
    req->lastZxidSeen = htonll(req->lastZxidSeen);
    memcpy(buffer + offset, &req->lastZxidSeen, sizeof(req->lastZxidSeen));
    offset = offset +  sizeof(req->lastZxidSeen);
    
    req->timeOut = htonl(req->timeOut);
    memcpy(buffer + offset, &req->timeOut, sizeof(req->timeOut));
    offset = offset +  sizeof(req->timeOut);
    
    req->sessionId = htonll(req->sessionId);
    memcpy(buffer + offset, &req->sessionId, sizeof(req->sessionId));
    offset = offset +  sizeof(req->sessionId);
    
    req->passwd_len = htonl(req->passwd_len);
    memcpy(buffer + offset, &req->passwd_len, sizeof(req->passwd_len));
    offset = offset +  sizeof(req->passwd_len);
    
    memcpy(buffer + offset, req->passwd, sizeof(req->passwd));
    
    return 0;
}

 static int deserialize_prime_response(struct prime_struct *req, char* buffer){
     //this should be the order of deserialization 
     int offset = 0;
     memcpy(&req->len, buffer + offset, sizeof(req->len));
     offset = offset +  sizeof(req->len);
     
     req->len = ntohl(req->len);
     memcpy(&req->protocolVersion, buffer + offset, sizeof(req->protocolVersion));
     offset = offset +  sizeof(req->protocolVersion);
     
     req->protocolVersion = ntohl(req->protocolVersion);
     memcpy(&req->timeOut, buffer + offset, sizeof(req->timeOut));
     offset = offset +  sizeof(req->timeOut);
     
     req->timeOut = ntohl(req->timeOut);
     memcpy(&req->sessionId, buffer + offset, sizeof(req->sessionId));
     offset = offset +  sizeof(req->sessionId);
     
     req->sessionId = htonll(req->sessionId);
     memcpy(&req->passwd_len, buffer + offset, sizeof(req->passwd_len));
     offset = offset +  sizeof(req->passwd_len);
     
     req->passwd_len = ntohl(req->passwd_len);
     memcpy(req->passwd, buffer + offset, sizeof(req->passwd));
     return 0;
 }

static int prime_connection(zhandle_t *zh)
{
    int rc;
    /*this is the size of buffer to serialize req into*/
    char buffer_req[HANDSHAKE_REQ_SIZE]; 
    int len = sizeof(buffer_req);
    int hlen = 0;
    struct connect_req req;
    req.protocolVersion = 0;
    req.sessionId = zh->client_id.client_id;
    req.passwd_len = sizeof(req.passwd);
    memcpy(req.passwd, zh->client_id.passwd, sizeof(zh->client_id.passwd));
    req.timeOut = zh->recv_timeout;
    req.lastZxidSeen = zh->last_zxid;
    hlen = htonl(len);
    /* We are running fast and loose here, but this string should fit in the initial buffer! */
    rc=send(zh->fd, &hlen, sizeof(len), 0);
    serialize_prime_connect(&req, buffer_req);
    rc=rc<0 ? rc : send(zh->fd, buffer_req, len, 0);
    if (rc<0) {
        return handle_socket_error_msg(zh, __LINE__, ZCONNECTIONLOSS,
                "failed to send a handshake packet: %s", strerror(errno));
    }
    zh->state = ASSOCIATING_STATE;

    zh->input_buffer = &zh->primer_buffer;
    /* This seems a bit weird to to set the offset to 4, but we already have a
     * length, so we skip reading the length (and allocating the buffer) by
     * saying that we are already at offset 4 */
    zh->input_buffer->curr_offset = 4;

    return ZOK;
}

static inline int calculate_interval(const struct timeval *start, 
        const struct timeval *end)
{
    int interval;
    struct timeval i = *end;
    i.tv_sec -= start->tv_sec;
    i.tv_usec -= start->tv_usec;
    interval = i.tv_sec * 1000 + (i.tv_usec/1000);
    return interval;
}

static struct timeval get_timeval(int interval)
{
    struct timeval tv;
    if (interval < 0) {
        interval = 0;
    }
    tv.tv_sec = interval/1000;
    tv.tv_usec = (interval%1000)*1000;
    return tv;
}

 static int add_void_completion(zhandle_t *zh, int xid, void_completion_t dc,
     const void *data);
 static int add_string_completion(zhandle_t *zh, int xid,
     string_completion_t dc, const void *data);

 int zookeeper_interest(zhandle_t *zh, int *fd, int *interest,
     struct timeval *tv)
{
    if(zh==0 || fd==0 ||interest==0 || tv==0)
        return ZBADARGUMENTS;
    if (is_unrecoverable(zh))
        return ZINVALIDSTATE;
    api_prolog(zh);
    *fd = zh->fd;
    *interest = 0;
    tv->tv_sec = 0;
    tv->tv_usec = 0;
    if (*fd == -1) {
        if (zh->connect_index == zh->addrs_count) {
            /* Wait a bit before trying again so that we don't spin */
            zh->connect_index = 0;
        }else {
            int rc;
            int on = 1;
            
            zh->fd = socket(PF_INET, SOCK_STREAM, 0);
            setsockopt(zh->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(int));
            fcntl(zh->fd, F_SETFL, O_NONBLOCK|fcntl(zh->fd, F_GETFL, 0));
            rc = connect(zh->fd, &zh->addrs[zh->connect_index],
                    sizeof(struct sockaddr));
            if (rc == -1) {
                if (errno == EWOULDBLOCK || errno == EINPROGRESS)
                    zh->state = CONNECTING_STATE;
                else
                    return api_epilog(zh,handle_socket_error_msg(zh,__LINE__,
                            ZCONNECTIONLOSS,"connect() call failed"));
            } else {
                if((rc=prime_connection(zh))!=0)
                    return api_epilog(zh,rc);

                LOG_INFO(("Initiated connection to server [%s]", 
                        format_endpoint_info(&zh->addrs[zh->connect_index])));
            }
        }
        *fd = zh->fd;
        *tv = get_timeval(zh->recv_timeout/3);
        gettimeofday(&zh->last_recv,0);
    }
    if (zh->fd != -1) {
        struct timeval now;
        int idle_interval;
        int to;
        gettimeofday(&now, 0);
        idle_interval = calculate_interval(&zh->last_recv, &now);
        // We only allow 1/3 of our timeout time to expire before sending
        // a PING
        if (!zh->sent_requests.head && zh->state==CONNECTED_STATE) {
            to = zh->recv_timeout/3 - idle_interval;
            if (to <= 0) {
                int rc;
                struct oarchive *oa = create_buffer_oarchive();
                struct RequestHeader h = { .xid = PING_XID, .type = PING_OP};

                rc = serialize_RequestHeader(oa, "header", &h);
                rc = rc < 0 ? rc : add_void_completion(zh, h.xid, 0, 0);
                rc = rc < 0 ? rc : queue_buffer_bytes(&zh->to_send,
                        get_buffer(oa), get_buffer_len(oa));
                close_buffer_oarchive(&oa, 0);
                if (rc < 0){
                    LOG_ERROR(("failed to marchall request (zk retcode=%d)",rc));
                    return api_epilog(zh,ZMARSHALLINGERROR);
                }
//                LOG_DEBUG(("Sending PING to %s (exceeded idle by %dms)",
//                    format_current_endpoint_info(zh),-to));
                to = zh->recv_timeout/3;
            }
        } else {
            to = (zh->recv_timeout*2)/3 - idle_interval;
            if (to <= 0) {
                // We gotta cut our losses and connect to someone else
                errno = ETIMEDOUT;               
                *fd=-1;
                *interest=0;
                *tv = get_timeval(to);
                return api_epilog(zh,handle_socket_error_msg(zh,
                        __LINE__,ZOPERATIONTIMEOUT,
                        "connection timed out (exceeded timeout by %dms)",-to));
            }
        }
        *tv = get_timeval(to);
        *interest = ZOOKEEPER_READ;
        if (zh->to_send.head || zh->state == CONNECTING_STATE) {
            *interest |= ZOOKEEPER_WRITE;
        }
    }
    return api_epilog(zh,ZOK);
}

static int check_events(zhandle_t *zh, int events)
{
    if (zh->fd == -1)
        return ZINVALIDSTATE;
    if ((events&ZOOKEEPER_WRITE)&&(zh->state == CONNECTING_STATE)) {
        int rc, error;
        socklen_t len = sizeof(error);
        rc = getsockopt(zh->fd, SOL_SOCKET, SO_ERROR, &error, &len);
        if (rc < 0 || error) {
            if (rc == 0)
                errno = error;
            return handle_socket_error_msg(zh, __LINE__,ZCONNECTIONLOSS,
                "server refused to accept the client");
        }
        if((rc=prime_connection(zh))!=0)
            return rc;
        LOG_INFO(("initiated connection to server [%s]", 
                format_endpoint_info(&zh->addrs[zh->connect_index])));
        return ZOK;
    }
    if (zh->to_send.head && (events&ZOOKEEPER_WRITE)) {
        /* make the flush call non-blocking by specifying a 0 timeout */
        int rc=flush_send_queue(zh,0);
        if (rc < 0)
            return handle_socket_error_msg(zh,__LINE__,ZCONNECTIONLOSS,
                "failed while flushing send queue");
    }
    if (events&ZOOKEEPER_READ) {
        int rc;
        if (zh->input_buffer == 0) {
            zh->input_buffer = allocate_buffer(0,0);
        }

        rc = recv_buffer(zh->fd, zh->input_buffer);
        if (rc < 0) { 
            return handle_socket_error_msg(zh, __LINE__,ZCONNECTIONLOSS,
                "failed while receiving a server response");
        }
        if (rc > 0) {
            gettimeofday(&zh->last_recv, 0);
            if (zh->input_buffer != &zh->primer_buffer) {
                queue_buffer(&zh->to_process, zh->input_buffer, 0);
            } else  {
                int64_t oldid,newid;
                //deserialize
                deserialize_prime_response(&zh->primer_storage, zh->primer_buffer.buffer);
                /* We are processing the primer_buffer, so we need to finish
                 * the connection handshake */
                oldid = zh->client_id.client_id;
                newid = zh->primer_storage.sessionId;
                if (oldid != 0 && oldid != newid) {
                    zh->state = EXPIRED_SESSION_STATE;
                    errno = ESTALE;
                    return handle_socket_error_msg(zh,__LINE__,ZSESSIONEXPIRED,
                            "session %llx has expired.",oldid);
                } else {
                    zh->recv_timeout = zh->primer_storage.timeOut;
                    zh->client_id.client_id = newid;
                 
                    memcpy(zh->client_id.passwd, &zh->primer_storage.passwd, sizeof(zh->client_id.passwd));
                    zh->state = CONNECTED_STATE;
                    LOG_INFO(("connected to server [%s] with session id=%llx",
                            format_endpoint_info(&zh->addrs[zh->connect_index]),newid));
                    /* send the authentication packet now */
                    send_auth_info(zh);
                    LOG_DEBUG(("Calling a watcher for a SESSION_EVENT and the state=CONNECTED_STATE"));
                    zh->input_buffer = 0; // just in case the watcher calls zookeeper_process() again
                    zh->watcher(zh, SESSION_EVENT, CONNECTED_STATE, 0);
                }
            }
            zh->input_buffer = 0;
        }             
    }
    return ZOK;
}

void api_prolog(zhandle_t* zh)
{
    inc_ref_counter(zh,1); 
}

int api_epilog(zhandle_t *zh,int rc)
{
    if(inc_ref_counter(zh,-1)==0 && zh->close_requested!=0)
        zookeeper_close(zh);
    return rc;
}

static __attribute__((unused)) void print_completion_queue(zhandle_t *zh)
{
    completion_list_t* cptr;
    
    if(logLevel<LOG_LEVEL_DEBUG) return;
        
    fprintf(LOGSTREAM,"Completion queue: ");
    if (zh->sent_requests.head==0) {
        fprintf(LOGSTREAM,"empty\n");
        return;
    }
    
    cptr=zh->sent_requests.head;
    while(cptr){
        fprintf(LOGSTREAM,"%d,",cptr->xid);
        cptr=cptr->next;
    }
    fprintf(LOGSTREAM,"end\n");    
}

static completion_list_t* create_completion_entry(int xid, int completion_type,
        const void *dc, const void *data);
static void queue_completion(completion_head_t *list, completion_list_t *c,
        int add_to_front);

completion_list_t *dequeue_completion(completion_head_t *list)
{
    completion_list_t *cptr;
    lock_completion_list(list);
    cptr = list->head;
    if (cptr) {
        list->head = cptr->next;
        if (!list->head) {
            assert(list->last == cptr);
            list->last = 0;
        }
    }
    unlock_completion_list(list);
    return cptr;
}

void process_completions(zhandle_t *zh)
{
    completion_list_t *cptr;
    while ((cptr = dequeue_completion(&zh->completions_to_process)) != 0) {
        struct ReplyHeader hdr;
        buffer_list_t *bptr = cptr->buffer;
        struct iarchive *ia = create_buffer_iarchive(bptr->buffer,
                bptr->curr_offset);
        deserialize_ReplyHeader(ia, "hdr", &hdr);
        zh->last_zxid = hdr.zxid;

        if (hdr.xid == -1) {
            int type, state;
            struct WatcherEvent evt;
            deserialize_WatcherEvent(ia, "event", &evt);
            /* We are doing a notification, so there is no pending request */
            type = evt.type;
            state = evt.state;
            /* This is a notification so there aren't any pending requests */
            LOG_DEBUG(("Calling a watcher for node [%s], event=%s",
                 (evt.path==NULL?"NULL":evt.path),watcherEvent2String(type)));
            zh->watcher(zh, type, state, evt.path);
            deallocate_WatcherEvent(&evt);
        } else {
            int rc = hdr.err;
            switch (cptr->completion_type) {
            case COMPLETION_DATA:
                LOG_DEBUG(("Calling COMPLETION_DATA rc=%d",rc));
                if (rc) {
                    cptr->c.data_result(rc, 0, 0, 0, cptr->data);
                } else {
                    struct GetDataResponse res;
                    deserialize_GetDataResponse(ia, "reply", &res);
                    cptr->c.data_result(rc, res.data.buff, res.data.len,
                            &res.stat, cptr->data);
                    deallocate_GetDataResponse(&res);
                }
                break;
            case COMPLETION_STAT:
                LOG_DEBUG(("Calling COMPLETION_STAT rc=%d",rc));
                if (rc) {
                    cptr->c.stat_result(rc, 0, cptr->data);
                } else {
                    struct SetDataResponse res;
                    deserialize_SetDataResponse(ia, "reply", &res);
                    cptr->c.stat_result(rc, &res.stat, cptr->data);
                    deallocate_SetDataResponse(&res);
                }
                break;
            case COMPLETION_STRINGLIST:
                LOG_DEBUG(("Calling COMPLETION_STRINGLIST rc=%d",rc));
                if (rc) {
                    cptr->c.strings_result(rc, 0, cptr->data);
                } else {
                    struct GetChildrenResponse res;
                    deserialize_GetChildrenResponse(ia, "reply", &res);
                    cptr->c.strings_result(rc, &res.children, cptr->data);
                    deallocate_GetChildrenResponse(&res);
                }
                break;
            case COMPLETION_STRING:
                LOG_DEBUG(("Calling COMPLETION_STRING rc=%d",rc));
                if (rc) {
                    cptr->c.string_result(rc, 0, cptr->data);
                } else {
                    struct CreateResponse res;
                    deserialize_CreateResponse(ia, "reply", &res);
                    cptr->c.string_result(rc, res.path, cptr->data);
                    deallocate_CreateResponse(&res);
                }
                break;
            case COMPLETION_ACLLIST:
                LOG_DEBUG(("Calling COMPLETION_ACLLIST rc=%d",rc));
                if (rc) {
                    cptr->c.acl_result(rc, 0, 0, cptr->data);
                } else {
                    struct GetACLResponse res;
                    deserialize_GetACLResponse(ia, "reply", &res);
                    cptr->c.acl_result(rc, &res.acl, &res.stat, cptr->data);
                    deallocate_GetACLResponse(&res);
                }
                break;
            case COMPLETION_VOID:
                LOG_DEBUG(("Calling COMPLETION_VOID rc=%d",rc));
                if (hdr.xid == PING_XID) {
                    // We want to skip the ping
                } else {
                    cptr->c.void_result(rc, cptr->data);
                }
                break;
            }
            free_buffer(cptr->buffer);
            free(cptr);
        }
        close_buffer_iarchive(&ia);
    }
}

int zookeeper_process(zhandle_t *zh, int events) 
{
    buffer_list_t *bptr;
    int rc;
    if (zh==NULL)
        return ZBADARGUMENTS;
    if (is_unrecoverable(zh))
        return ZINVALIDSTATE;
    api_prolog(zh);
    rc = check_events(zh, events);
    if (rc!=ZOK)
        return api_epilog(zh, rc);

    while (rc >= 0&& (bptr=dequeue_buffer(&zh->to_process))) {
        struct ReplyHeader hdr;
        struct iarchive *ia = create_buffer_iarchive(
                                    bptr->buffer, bptr->curr_offset);
        deserialize_ReplyHeader(ia, "hdr", &hdr);
        zh->last_zxid = hdr.zxid;
        
        if (hdr.xid == -1) {
            completion_list_t *c = create_completion_entry(-1, -1, 0, 0);
            c->buffer = bptr;
            queue_completion(&zh->completions_to_process, c, 0);
        } else if(hdr.xid == AUTH_XID){
            /* special handling for the AUTH response as it may come back 
             * out-of-band */
            auth_completion_func(hdr.err,zh);
            free_buffer(bptr);
            /* authentication completion may change the connection state to 
             * unrecoverable */
            if(is_unrecoverable(zh)){
                handle_error(zh, ZAUTHFAILED);
                close_buffer_iarchive(&ia);
                return api_epilog(zh, ZAUTHFAILED);
            }
        } else { 
            int rc = hdr.err;
            /* Find the request corresponding to the response */
            completion_list_t *cptr = dequeue_completion(&zh->sent_requests);
            assert(cptr);
            /* The requests are going to come back in order */
            if (cptr->xid != hdr.xid) {
                // received unexpected (or out-of-order) response
                close_buffer_iarchive(&ia);
                free_buffer(bptr);
                // put the completion back on the queue (so it gets properly 
                // signaled and deallocated) and disconnect from the server
                queue_completion(&zh->sent_requests,cptr,1);
                return handle_socket_error_msg(zh, __LINE__,ZRUNTIMEINCONSISTENCY,
                        "unexpected server response: expected %x, but received %x",
                        hdr.xid,cptr->xid);
            }
            if (cptr->c.void_result != SYNCHRONOUS_MARKER) {
                if(hdr.xid == PING_XID){
                    // Nothing to do with a ping response
                    free_buffer(bptr);
                    free(cptr);
                } else { 
                    cptr->buffer = bptr;
                    queue_completion(&zh->completions_to_process, cptr, 0);
                }
            } else {
                struct sync_completion
                        *sc = (struct sync_completion*)cptr->data;
                sc->rc = rc;
                switch(cptr->completion_type) {
                case COMPLETION_DATA:
                    if (rc==0) {
                        struct GetDataResponse res;
                        int len;
                        LOG_DEBUG(("Calling COMPLETION_DATA rc=%d",rc));
                        deserialize_GetDataResponse(ia, "reply", &res);
                        if (res.data.len <= sc->u.data.buff_len) {
                            len = res.data.len;
                        } else {
                            len = sc->u.data.buff_len;
                        }
                        sc->u.data.buff_len = len;
                        memcpy(sc->u.data.buffer, res.data.buff, len);
                        sc->u.data.stat = res.stat;
                        deallocate_GetDataResponse(&res);
                    }
                    break;
                case COMPLETION_STAT:
                    if (rc == 0) {
                        struct SetDataResponse res;
                        LOG_DEBUG(("Calling COMPLETION_STAT rc=%d",rc));
                        deserialize_SetDataResponse(ia, "reply", &res);
                        sc->u.stat = res.stat;
                        deallocate_SetDataResponse(&res);
                    }
                    break;
                case COMPLETION_STRINGLIST:
                    if (rc == 0) {
                        struct GetChildrenResponse res;
                        LOG_DEBUG(("Calling COMPLETION_STRINGLIST rc=%d",rc));
                        deserialize_GetChildrenResponse(ia, "reply", &res);
                        sc->u.strs = res.children;
                        /* We don't deallocate since we are passing it back */
                        // deallocate_GetChildrenResponse(&res);
                    }
                    break;
                case COMPLETION_STRING:
                    if (rc == 0) {
                        struct CreateResponse res;
                        int len;
                        LOG_DEBUG(("Calling COMPLETION_STRING rc=%d",rc));
                        deserialize_CreateResponse(ia, "reply", &res);
                        if (sc->u.str.str_len > strlen(res.path)) {
                            len = strlen(res.path);
                        } else {
                            len = sc->u.str.str_len;
                        }
                        memcpy(sc->u.str.str, res.path, len);
                        sc->u.str.str[len] = '\0';
                        deallocate_CreateResponse(&res);
                    }
                    break;
                case COMPLETION_ACLLIST:
                    if (rc == 0) {
                        struct GetACLResponse res;
                        LOG_DEBUG(("Calling COMPLETION_ACLLIST rc=%d",rc));
                        deserialize_GetACLResponse(ia, "reply", &res);
                        cptr->c.acl_result(rc, &res.acl, &res.stat, cptr->data);
                        sc->u.acl.acl = res.acl;
                        sc->u.acl.stat = res.stat;
                        /* We don't deallocate since we are passing it back */
                        //deallocate_GetACLResponse(&res);
                    }
                    break;
                case COMPLETION_VOID:
                    LOG_DEBUG(("Calling COMPLETION_VOID rc=%d",rc));
                    break;
                }
                notify_sync_completion(sc);
                free_buffer(bptr);
                zh->outstanding_sync--;
                free(cptr);
            }
        }

        close_buffer_iarchive(&ia);

    }
    if (process_async(zh->outstanding_sync)) {
        process_completions(zh);
    }
    return api_epilog(zh,ZOK);
}

int zoo_state(zhandle_t *zh)
{
    if(zh!=0)
        return zh->state;
    return 0;
}

static completion_list_t* create_completion_entry(int xid, int completion_type, 
        const void *dc, const void *data)
{
    completion_list_t *c = calloc(1,sizeof(completion_list_t));
    if (!c) {
        LOG_ERROR(("out of memory"));
        return 0;
    }
    c->completion_type = completion_type;
    c->data = data;
    switch(c->completion_type) {
    case COMPLETION_VOID:
        c->c.void_result = (void_completion_t)dc;
        break;
    case COMPLETION_STRING:
        c->c.string_result = (string_completion_t)dc;
        break;
    case COMPLETION_DATA:
        c->c.data_result = (data_completion_t)dc;
        break;
    case COMPLETION_STAT:
        c->c.stat_result = (stat_completion_t)dc;
        break;
    case COMPLETION_STRINGLIST:
        c->c.strings_result = (strings_completion_t)dc;
        break;
    case COMPLETION_ACLLIST:
        c->c.acl_result = (acl_completion_t)dc;
        break;
    }
    c->xid = xid;
    c->next = 0;

    return c;
}

static void queue_completion(completion_head_t *list, completion_list_t *c,
        int add_to_front)
{
     c->next = 0;
    /* appending a new entry to the back of the list */
    lock_completion_list(list);
    if (list->last) {
        assert(list->head);
        // List is not empty
        if (!add_to_front) {
            list->last->next = c;
            list->last = c;
        } else {
            c->next = list->head;
            list->head = c;
        }
    } else {
        // List is empty
        assert(!list->head);
        list->head = c;
        list->last = c;
    }
    unlock_completion_list(list);
}

static int add_completion(zhandle_t *zh, int xid, int completion_type,
        const void *dc, const void *data, int add_to_front)
{
    completion_list_t *c =create_completion_entry(xid, completion_type, dc,
            data);
    if (!c) 
        return ZSYSTEMERROR;
    queue_completion(&zh->sent_requests, c, add_to_front);
    if (dc == SYNCHRONOUS_MARKER) {
        zh->outstanding_sync++;
    }
    return ZOK;
}

static int add_data_completion(zhandle_t *zh, int xid, data_completion_t dc,
        const void *data)
{
    return add_completion(zh, xid, COMPLETION_DATA, dc, data, 0);
}

static int add_stat_completion(zhandle_t *zh, int xid, stat_completion_t dc,
        const void *data)
{
    return add_completion(zh, xid, COMPLETION_STAT, dc, data, 0);
}

static int add_strings_completion(zhandle_t *zh, int xid,
        strings_completion_t dc, const void *data)
{
    return add_completion(zh, xid, COMPLETION_STRINGLIST, dc, data, 0);
}

static int add_acl_completion(zhandle_t *zh, int xid, acl_completion_t dc,
        const void *data)
{
    return add_completion(zh, xid, COMPLETION_ACLLIST, dc, data, 0);
}

static int add_void_completion(zhandle_t *zh, int xid, void_completion_t dc,
        const void *data)
{
    return add_completion(zh, xid, COMPLETION_VOID, dc, data, 0);
}

static int add_string_completion(zhandle_t *zh, int xid,
        string_completion_t dc, const void *data)
{
    return add_completion(zh, xid, COMPLETION_STRING, dc, data, 0);
}

int zookeeper_close(zhandle_t *zh)
{
    int rc=ZOK;
    if (zh==0)
        return ZBADARGUMENTS; 
    
    zh->close_requested=1;
    if (inc_ref_counter(zh,0)!=0) {
        adaptor_finish(zh);
        return ZOK;
    }
    if(zh->state==CONNECTED_STATE){
        struct oarchive *oa;
        struct RequestHeader h = { .xid = get_xid(), .type = CLOSE_OP};
        LOG_INFO(("Closing zookeeper session %llx to [%s]\n",
                zh->client_id.client_id,format_current_endpoint_info(zh)));
        oa = create_buffer_oarchive();
        rc = serialize_RequestHeader(oa, "header", &h);
        rc = rc < 0 ? rc : queue_buffer_bytes(&zh->to_send, get_buffer(oa),
                get_buffer_len(oa));
        /* We queued the buffer, so don't free it */
        close_buffer_oarchive(&oa, 0);
        if (rc < 0) {
            rc = ZMARSHALLINGERROR;
            goto finish;
        }

        /* make sure the close request is sent; we set timeout to an arbitrary 
         * (but reasonable) number of milliseconds since we want the call to block*/
        rc=adaptor_send_queue(zh, 3000);
    }else{
        LOG_INFO(("Freeing zookeeper resources for session %llx\n",
                zh->client_id.client_id));
        rc = ZOK;
    } 

finish:
    destroy(zh);
    adaptor_destroy(zh);
    free(zh);
    return rc;
}

int zoo_aget(zhandle_t *zh, const char *path, int watch, data_completion_t dc,
        const void *data)
{
    struct oarchive *oa; 
    struct RequestHeader h = { .xid = get_xid(), .type = GETDATA_OP};
    struct GetDataRequest req = { (char*)path, watch };
    int rc;
    
    if (zh==0)
        return ZBADARGUMENTS;
    if (is_unrecoverable(zh))
        return ZINVALIDSTATE;
    oa=create_buffer_oarchive();
    rc = serialize_RequestHeader(oa, "header", &h);
    rc = rc < 0 ? rc : serialize_GetDataRequest(oa, "req", &req);
    enter_critical(zh);
    rc = rc < 0 ? rc : add_data_completion(zh, h.xid, dc, data);
    rc = rc < 0 ? rc : queue_buffer_bytes(&zh->to_send, get_buffer(oa),
            get_buffer_len(oa));
    leave_critical(zh);

    /* We queued the buffer, so don't free it */
    close_buffer_oarchive(&oa, 0);
    
    LOG_DEBUG(("Sending zoo_aget() request to %s",format_current_endpoint_info(zh)));
    /* make a best (non-blocking) effort to send the requests asap */
    adaptor_send_queue(zh, 0);
    return (rc < 0)?ZMARSHALLINGERROR:ZOK;
}

int zoo_aset(zhandle_t *zh, const char *path, const char *buffer, int buflen,
        int version, stat_completion_t dc, const void *data)
{
    struct oarchive *oa;
    struct RequestHeader h = { .xid = get_xid(), .type = SETDATA_OP};
    struct SetDataRequest req;
    int rc;
    
    if (zh==0)
        return ZBADARGUMENTS;
    if (is_unrecoverable(zh))
        return ZINVALIDSTATE;
    oa = create_buffer_oarchive();
    req.path = (char*)path;
    req.data.buff = (char*)buffer;
    req.data.len = buflen;
    req.version = version;
    rc = serialize_RequestHeader(oa, "header", &h);
    rc = rc < 0 ? rc : serialize_SetDataRequest(oa, "req", &req);
    enter_critical(zh);
    rc = rc < 0 ? rc : add_stat_completion(zh, h.xid, dc, data);
    rc = rc < 0 ? rc : queue_buffer_bytes(&zh->to_send, get_buffer(oa),
            get_buffer_len(oa));
    leave_critical(zh);
    /* We queued the buffer, so don't free it */
    close_buffer_oarchive(&oa, 0);

    LOG_DEBUG(("Sending zoo_aset() request to %s",format_current_endpoint_info(zh)));
    /* make a best (non-blocking) effort to send the requests asap */
    adaptor_send_queue(zh, 0);
    return (rc < 0)?ZMARSHALLINGERROR:ZOK;
}

int zoo_acreate(zhandle_t *zh, const char *path, const char *value,
        int valuelen, const struct ACL_vector *acl_entries, int ephemeral,
        string_completion_t completion, const void *data)
{
    struct oarchive *oa;
    struct RequestHeader h = { .xid = get_xid(), .type = CREATE_OP };
    struct CreateRequest req;
    int rc;
    
    if (zh==0)
        return ZBADARGUMENTS;
    if (is_unrecoverable(zh))
        return ZINVALIDSTATE;
    oa = create_buffer_oarchive();
    req.path = (char*)path;
    req.flags = ephemeral;
    req.data.buff = (char*)value;
    req.data.len = valuelen;
    if (acl_entries == 0) {
        req.acl.count = 0;
        req.acl.data = 0;
    } else {
        req.acl = *acl_entries;
    }
    rc = serialize_RequestHeader(oa, "header", &h);
    rc = rc < 0 ? rc : serialize_CreateRequest(oa, "req", &req);
    enter_critical(zh);
    rc = rc < 0 ? rc : add_string_completion(zh, h.xid, completion, data);
    rc = rc < 0 ? rc : queue_buffer_bytes(&zh->to_send, get_buffer(oa),
            get_buffer_len(oa));
    leave_critical(zh);
    /* We queued the buffer, so don't free it */
    close_buffer_oarchive(&oa, 0);
    
    LOG_DEBUG(("Sending zoo_acreate() request to %s",format_current_endpoint_info(zh)));
    /* make a best (non-blocking) effort to send the requests asap */
    adaptor_send_queue(zh, 0);
    return (rc < 0)?ZMARSHALLINGERROR:ZOK;
}

int zoo_adelete(zhandle_t *zh, const char *path, int version,
        void_completion_t completion, const void *data)
{
    struct oarchive *oa;
    struct RequestHeader h = { .xid = get_xid(), .type = DELETE_OP};
    struct DeleteRequest req;
    int rc;
    
    if (zh==0)
        return ZBADARGUMENTS;
    if (is_unrecoverable(zh))
        return ZINVALIDSTATE;
    oa = create_buffer_oarchive();
    req.path = (char*)path;
    req.version = version;
    rc = serialize_RequestHeader(oa, "header", &h);
    rc = rc < 0 ? rc : serialize_DeleteRequest(oa, "req", &req);
    enter_critical(zh);
    rc = rc < 0 ? rc : add_void_completion(zh, h.xid, completion, data);
    rc = rc < 0 ? rc : queue_buffer_bytes(&zh->to_send, get_buffer(oa),
            get_buffer_len(oa));
    leave_critical(zh);
    /* We queued the buffer, so don't free it */
    close_buffer_oarchive(&oa, 0);

    LOG_DEBUG(("Sending zoo_adelete() request to %s",format_current_endpoint_info(zh)));
    /* make a best (non-blocking) effort to send the requests asap */
    adaptor_send_queue(zh, 0);
    return (rc < 0)?ZMARSHALLINGERROR:ZOK;
}

int zoo_aexists(zhandle_t *zh, const char *path, int watch,
        stat_completion_t completion, const void *data)
{
    struct oarchive *oa;
    struct RequestHeader h = { .xid = get_xid(), .type = EXISTS_OP };
    struct ExistsRequest req;
    int rc;
    
    if (zh==0)
        return ZBADARGUMENTS;
    if (is_unrecoverable(zh))
        return ZINVALIDSTATE;
    oa = create_buffer_oarchive();
    req.path = (char*)path;
    req.watch = watch;
    rc = serialize_RequestHeader(oa, "header", &h);
    rc = rc < 0 ? rc : serialize_ExistsRequest(oa, "req", &req);
    enter_critical(zh);
    rc = rc < 0 ? rc : add_stat_completion(zh, h.xid, completion, data);
    rc = rc < 0 ? rc : queue_buffer_bytes(&zh->to_send, get_buffer(oa),
            get_buffer_len(oa));
    leave_critical(zh);
    /* We queued the buffer, so don't free it */
    close_buffer_oarchive(&oa, 0);

    LOG_DEBUG(("Sending zoo_aexists() request to %s",format_current_endpoint_info(zh)));
    /* make a best (non-blocking) effort to send the requests asap */
    adaptor_send_queue(zh, 0);
    return (rc < 0)?ZMARSHALLINGERROR:ZOK;
}

int zoo_aget_children(zhandle_t *zh, const char *path, int watch,
        strings_completion_t completion, const void *data)
{
    struct oarchive *oa;
    struct RequestHeader h = { .xid = get_xid(), .type = GETCHILDREN_OP};
    struct GetChildrenRequest req;
    int rc;
    
    if (zh==0)
        return ZBADARGUMENTS;
    if (is_unrecoverable(zh))
        return ZINVALIDSTATE;
    oa = create_buffer_oarchive();
    req.path = (char*)path;
    req.watch = watch;
    rc = serialize_RequestHeader(oa, "header", &h);
    rc = rc < 0 ? rc : serialize_GetChildrenRequest(oa, "req", &req);
    enter_critical(zh);
    rc = rc < 0 ? rc : add_strings_completion(zh, h.xid, completion, data);
    rc = rc < 0 ? rc : queue_buffer_bytes(&zh->to_send, get_buffer(oa),
            get_buffer_len(oa));
    leave_critical(zh);
    /* We queued the buffer, so don't free it */
    close_buffer_oarchive(&oa, 0);

    LOG_DEBUG(("Sending zoo_aget_children() request to %s",format_current_endpoint_info(zh)));
    /* make a best (non-blocking) effort to send the requests asap */
    adaptor_send_queue(zh, 0);
    return (rc < 0)?ZMARSHALLINGERROR:ZOK;
}

int zoo_async(zhandle_t *zh, const char *path,
        string_completion_t completion, const void *data)
{
    struct oarchive *oa;
    struct RequestHeader h = { .xid = get_xid(), .type = SYNC_OP};
    struct SyncRequest req;
    int rc;

    if (zh==0)
        return ZBADARGUMENTS;
    if (is_unrecoverable(zh))
        return ZINVALIDSTATE;
    oa = create_buffer_oarchive();
    req.path = (char*)path;
    rc = serialize_RequestHeader(oa, "header", &h);
    rc = rc < 0 ? rc : serialize_SyncRequest(oa, "req", &req);
    enter_critical(zh);
    rc = rc < 0 ? rc : add_string_completion(zh, h.xid, completion, data);
    rc = rc < 0 ? rc : queue_buffer_bytes(&zh->to_send, get_buffer(oa),
            get_buffer_len(oa));
    leave_critical(zh);
    /* We queued the buffer, so don't free it */
    close_buffer_oarchive(&oa, 0);

    LOG_DEBUG(("Sending zoo_sync() request to %s",format_current_endpoint_info(zh)));
    /* make a best (non-blocking) effort to send the requests asap */
    adaptor_send_queue(zh, 0);
    return (rc < 0)?ZMARSHALLINGERROR:ZOK;
}


int zoo_aget_acl(zhandle_t *zh, const char *path, acl_completion_t completion,
        const void *data)
{
    struct oarchive *oa;
    struct RequestHeader h = { .xid = get_xid(), .type = GETACL_OP};
    struct GetACLRequest req;
    int rc;
    
    if (zh==0)
        return ZBADARGUMENTS;
    if (is_unrecoverable(zh))
        return ZINVALIDSTATE;
    oa = create_buffer_oarchive();
    req.path = (char*)path;
    rc = serialize_RequestHeader(oa, "header", &h);
    rc = rc < 0 ? rc : serialize_GetACLRequest(oa, "req", &req);
    enter_critical(zh);
    rc = rc < 0 ? rc : add_acl_completion(zh, h.xid, completion, data);
    rc = rc < 0 ? rc : queue_buffer_bytes(&zh->to_send, get_buffer(oa),
            get_buffer_len(oa));
    leave_critical(zh);
    /* We queued the buffer, so don't free it */
    close_buffer_oarchive(&oa, 0);

    LOG_DEBUG(("Sending zoo_aget_acl() request to %s",format_current_endpoint_info(zh)));
    /* make a best (non-blocking) effort to send the requests asap */
    adaptor_send_queue(zh, 0);
    return (rc < 0)?ZMARSHALLINGERROR:ZOK;
}

int zoo_aset_acl(zhandle_t *zh, const char *path, int version,
        struct ACL_vector *acl, void_completion_t completion, const void *data)
{
    struct oarchive *oa;
    struct RequestHeader h = { .xid = get_xid(), .type = SETACL_OP};
    struct SetACLRequest req;
    int rc;
    
    if (zh==0)
        return ZBADARGUMENTS;
    if (is_unrecoverable(zh))
        return ZINVALIDSTATE;
    oa = create_buffer_oarchive();
    req.path = (char*)path;
    req.acl = *acl;
    req.version = version;
    rc = serialize_RequestHeader(oa, "header", &h);
    rc = rc < 0 ? rc : serialize_SetACLRequest(oa, "req", &req);
    enter_critical(zh);
    rc = rc < 0 ? rc : add_void_completion(zh, h.xid, completion, data);
    rc = rc < 0 ? rc : queue_buffer_bytes(&zh->to_send, get_buffer(oa),
            get_buffer_len(oa));
    leave_critical(zh);
    /* We queued the buffer, so don't free it */
    close_buffer_oarchive(&oa, 0);

    LOG_DEBUG(("Sending zoo_aset_acl() request to %s",format_current_endpoint_info(zh)));
    /* make a best (non-blocking) effort to send the requests asap */
    adaptor_send_queue(zh, 0);
    return (rc < 0)?ZMARSHALLINGERROR:ZOK;
}

/* specify timeout of 0 to make the function non-blocking */
/* timeout is in milliseconds */
int flush_send_queue(zhandle_t*zh, int timeout)
{
    int rc= ZOK;
    struct timeval started;
    gettimeofday(&started,0);
    // we can't use dequeue_buffer() here because if (non-blocking) send_buffer() 
    // returns EWOULDBLOCK we'd have to put the buffer back on the queue.
    // we use a recursive lock instead and only dequeue the buffer if a send was
    // successful
    lock_buffer_list(&zh->to_send);
    while (zh->to_send.head != 0&& zh->state == CONNECTED_STATE) {
        if(timeout!=0){
            int elapsed;
            struct pollfd fds;
            struct timeval now;
            gettimeofday(&now,0);
            elapsed=calculate_interval(&started,&now);
            if (elapsed>timeout) {
                rc = ZOPERATIONTIMEOUT;
                break;
            }
            fds.fd = zh->fd;
            fds.events = POLLOUT;
            fds.revents = 0;
            rc = poll(&fds, 1, timeout-elapsed);
            if (rc<=0) {
                /* timed out or an error or POLLERR */
                rc = rc==0 ? ZOPERATIONTIMEOUT : ZSYSTEMERROR; 
                break;
            }
        }

        rc = send_buffer(zh->fd, zh->to_send.head);
        if(rc==0 && timeout==0){
            /* send_buffer would block while sending this buffer */
            rc = ZOK;
            break;
        }
        if (rc < 0) {
            rc = ZCONNECTIONLOSS;
            break;
        }
        // if the buffer has been sent succesfully, remove it from the queue
        if (rc > 0)
            remove_buffer(&zh->to_send);
        rc = ZOK;
    }
    unlock_buffer_list(&zh->to_send);
    return rc;
}

const char* zerror(int c)
{
    switch (c){
    case ZOK:
      return "ok";
    case ZSYSTEMERROR:
      return "system error";
    case ZRUNTIMEINCONSISTENCY:
      return "run time inconsistency";
    case ZDATAINCONSISTENCY:
      return "data inconsistency";
    case ZCONNECTIONLOSS:
      return "connection loss";
    case ZMARSHALLINGERROR:
      return "marshalling error";
    case ZUNIMPLEMENTED:
      return "unimplemented";
    case ZOPERATIONTIMEOUT:
      return "operation timeout";
    case ZBADARGUMENTS:
      return "bad arguments";
    case ZINVALIDSTATE:
      return "invalid zhandle state";
    case ZAPIERROR:
      return "api error";
    case ZNONODE:
      return "no node";
    case ZNOAUTH:
      return "not authenticated";
    case ZBADVERSION:
      return "bad version";
    case  ZNOCHILDRENFOREPHEMERALS:
      return "no children for ephemerals";
    case ZNODEEXISTS:
      return "node exists";
    case ZNOTEMPTY:
      return "not empty";
    case ZSESSIONEXPIRED:
      return "session expired";
    case ZINVALIDCALLBACK:
      return "invalid callback";
    case ZINVALIDACL:
      return "invalid acl";
    case ZAUTHFAILED:
      return "authentication failed";
    }
    if (c > 0) {
      return strerror(c);
    }
    return "unknown error";
}

int zoo_add_auth(zhandle_t *zh,const char* scheme,const char* cert, 
        int certLen,void_completion_t completion, const void *data)
{
    if(scheme==NULL || zh==NULL)
        return ZBADARGUMENTS;
    
    if (is_unrecoverable(zh))
        return ZINVALIDSTATE;
    
    free_auth_info(&zh->auth);
    zh->auth.scheme=strdup(scheme);
    if(cert!=NULL && certLen!=0){
        zh->auth.auth.buff=calloc(1,certLen);
        if(zh->auth.auth.buff==0)
            return ZSYSTEMERROR;
        memcpy(zh->auth.auth.buff,cert,certLen);
        zh->auth.auth.len=certLen;
    }
    
    zh->auth.completion=completion;
    zh->auth.data=data;
    if(zh->state == CONNECTED_STATE || zh->state == ASSOCIATING_STATE)
        return send_auth_info(zh);
    
    return ZOK;
}

static const char* format_endpoint_info(const struct sockaddr* ep)
{
    static char buf[128];
    char addrstr[128];
    void *inaddr;
    int port;
    if(ep==0)
        return "null";

    inaddr=&((struct sockaddr_in*)ep)->sin_addr;
    port=((struct sockaddr_in*)ep)->sin_port;
#if defined(AF_INET6)
    if(ep->sa_family==AF_INET6){
        inaddr=&((struct sockaddr_in6*)ep)->sin6_addr;
        port=((struct sockaddr_in6*)ep)->sin6_port;
    }
#endif
    
    inet_ntop(ep->sa_family,inaddr,addrstr,sizeof(addrstr)-1);
    sprintf(buf,"%s:%d",addrstr,ntohs(port));
    return buf;
}

static const char* format_current_endpoint_info(zhandle_t* zh)
{
    return format_endpoint_info(&zh->addrs[zh->connect_index]);
}

void zoo_deterministic_conn_order(int yesOrNo)
{
    disable_conn_permute=yesOrNo;
}

/* ****************************************************************************
 * sync API
 */
int zoo_create(zhandle_t *zh, const char *path, const char *value,
        int valuelen, const struct ACL_vector *acl, int flags, char *realpath,
        int max_realpath_len)
{
    struct sync_completion *sc = alloc_sync_completion();
    int rc;
    if (!sc) {
        return ZSYSTEMERROR;
    }
    sc->u.str.str = realpath;
    sc->u.str.str_len = max_realpath_len;
    rc=zoo_acreate(zh, path, value, valuelen, acl, flags, SYNCHRONOUS_MARKER, sc);
    if(rc==ZOK){
        wait_sync_completion(sc);
        rc = sc->rc;
    }
    free_sync_completion(sc);
    return rc;
}

int zoo_delete(zhandle_t *zh, const char *path, int version)
{
    struct sync_completion *sc = alloc_sync_completion();
    int rc;
    if (!sc) {
        return ZSYSTEMERROR;
    }
    rc=zoo_adelete(zh, path, version, SYNCHRONOUS_MARKER, sc);
    if(rc==ZOK){
        wait_sync_completion(sc);
        rc = sc->rc;
    }
    free_sync_completion(sc);
    return rc;
}

int zoo_exists(zhandle_t *zh, const char *path, int watch, struct Stat *stat)
{
    struct sync_completion *sc = alloc_sync_completion();
    int rc;
    if (!sc) {
        return ZSYSTEMERROR;
    }
    rc=zoo_aexists(zh, path, watch, SYNCHRONOUS_MARKER, sc);
    if(rc==ZOK){
        wait_sync_completion(sc);
        rc = sc->rc;
        if (rc == 0&& stat) {
            *stat = sc->u.stat;
        }
    }
    free_sync_completion(sc);
    return rc;
}

int zoo_get(zhandle_t *zh, const char *path, int watch, char *buffer,
        int* buffer_len, struct Stat *stat)
{
    struct sync_completion *sc;
    int rc=0;

    if(buffer_len==NULL)
        return ZBADARGUMENTS;
    if((sc=alloc_sync_completion())==NULL)
        return ZSYSTEMERROR;

    sc->u.data.buffer = buffer;
    sc->u.data.buff_len = *buffer_len;
    rc=zoo_aget(zh, path, watch, SYNCHRONOUS_MARKER, sc);
    if(rc==ZOK){
        wait_sync_completion(sc);
        rc = sc->rc;
        if (rc == 0) {
            if(stat)
                *stat = sc->u.data.stat;
            *buffer_len = sc->u.data.buff_len;
        }
    }
    free_sync_completion(sc);
    return rc;
}

int zoo_set(zhandle_t *zh, const char *path, const char *buffer, int buflen,
        int version)
{
    struct sync_completion *sc = alloc_sync_completion();
    int rc;
    if (!sc) {
        return ZSYSTEMERROR;
    }
    rc=zoo_aset(zh, path, buffer, buflen, version, SYNCHRONOUS_MARKER, sc);
    if(rc==ZOK){
        wait_sync_completion(sc);
        rc = sc->rc;
    }
    free_sync_completion(sc);
    return rc;
}

int zoo_get_children(zhandle_t *zh, const char *path, int watch,
        struct String_vector *strings)
{
    struct sync_completion *sc = alloc_sync_completion();
    int rc;
    if (!sc) {
        return ZSYSTEMERROR;
    }
    rc=zoo_aget_children(zh, path, watch, SYNCHRONOUS_MARKER, sc);
    if(rc==ZOK){
        wait_sync_completion(sc);
        rc = sc->rc;
        if (rc == 0) {
            if (strings) {
                *strings = sc->u.strs;
            } else {
                deallocate_String_vector(&sc->u.strs);
            }
        }
    }
    free_sync_completion(sc);
    return rc;
}

int zoo_get_acl(zhandle_t *zh, const char *path, struct ACL_vector *acl,
        struct Stat *stat)
{
    struct sync_completion *sc = alloc_sync_completion();
    int rc;
    if (!sc) {
        return ZSYSTEMERROR;
    }
    rc=zoo_aget_acl(zh, path, SYNCHRONOUS_MARKER, sc);
    if(rc==ZOK){
        wait_sync_completion(sc);
        rc = sc->rc;
        if (rc == 0&& stat) {
            *stat = sc->u.acl.stat;
        }
        if (rc == 0) {
            if (acl) {
                *acl = sc->u.acl.acl;
            } else {
                deallocate_ACL_vector(&sc->u.acl.acl);
            }
        }
    }
    free_sync_completion(sc);
    return rc;
}

int zoo_set_acl(zhandle_t *zh, const char *path, int version,
        const struct ACL_vector *acl)
{
    struct sync_completion *sc = alloc_sync_completion();
    int rc;
    if (!sc) {
        return ZSYSTEMERROR;
    }
    rc=zoo_aset_acl(zh, path, version, (struct ACL_vector*)acl,
            SYNCHRONOUS_MARKER, sc);
    if(rc==ZOK){
        wait_sync_completion(sc);
        rc = sc->rc;
    }
    free_sync_completion(sc);
    return rc;
}
