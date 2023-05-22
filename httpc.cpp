#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <curl/curl.h>
#include <string.h>
#include <map>
#include <vector>
#include <functional>
#include <lua-5.3.6/lua.hpp>

#define MAX(a, b) (((a) > (b)) ? (a) : (b))
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

struct event_base *base;
CURLM *curl_handle;
struct event *timeout;

struct curl_context_t {
    struct event *event;
    curl_socket_t sockfd;
};

struct webrequest {
    bool done;
    bool content_realloc_failed;
    bool lose_content;
    char * ip;
    char * url;
    char error[CURL_ERROR_SIZE];
    char * content;
    int port;
    int content_length;
    int response_code;
    size_t content_maxlength;
};

std::map<uintptr_t, webrequest *> requests;

static void curl_perform(int fd, short event, void *arg);

static webrequest * new_request(CURL * curl, bool lose_content) {
    uintptr_t ptr = (uintptr_t)curl;
    webrequest * req = new webrequest();
    memset(req, 0, sizeof(webrequest));
    req->lose_content = lose_content;
    requests[ptr] = req;
    return req;
}

static curl_context_t * 
create_curl_context(curl_socket_t sockfd) {
    curl_context_t * context = (curl_context_t *)malloc(sizeof(*context));
    context->sockfd = sockfd;
    context->event = event_new(base, sockfd, 0, curl_perform, context);
    return context;
}

static void 
destroy_curl_context(curl_context_t *context) {
    event_del(context->event);
    event_free(context->event);
    free(context);
}

static size_t 
write_callback(char * buffer, size_t block_size, size_t count, void * arg) {
    size_t length = block_size * count;

    webrequest * req = (webrequest *)arg;
    if (!req)
        return 0;

    if (req->lose_content){
        return length;
    }

    if (req->content_realloc_failed) {
        return length;
    }

    if (req->content_length + length > req->content_maxlength) {
        req->content_maxlength = MAX(req->content_maxlength, req->content_length + length);
        req->content_maxlength = MAX(req->content_maxlength, 512);
        req->content_maxlength = 2 * req->content_maxlength;
        void * new_content = (char*)realloc(req->content, req->content_maxlength);
        if (!new_content) {
            req->content_realloc_failed = true;
            return length;
        }
        req->content = (char *)new_content;
    }

    memcpy(req->content + req->content_length, buffer, length);
    req->content_length += length;
    return length;
}

static int 
on_respond(CURL * curl) {
    auto iter = requests.find((uintptr_t)curl);
    if (iter == requests.end()) {
        return 1;
    }

    webrequest * req = iter->second;
    req->done = true;
    curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &req->ip);
    curl_easy_getinfo(curl, CURLINFO_LOCAL_PORT, &req->port);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &req->content_length);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &req->response_code);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &req->url);

	printf("\nsuccess url : %s, response_code : %d, error : %s\n", req->url, req->response_code, req->error ? req->error : "NONE");

    if (req->response_code == 200 || req->response_code == 201) {
        requests.erase((uintptr_t)curl);
        delete req;
    }
    return 0;
}

static void 
check_multi_info(void) {
    CURLMsg * message = nullptr;
    int pending = 0;
    while((message = curl_multi_info_read(curl_handle, &pending))) {
        switch(message->msg) {
        case CURLMSG_DONE: {
            CURL * easy_handle = message->easy_handle;
            on_respond(easy_handle);
            curl_multi_remove_handle(curl_handle, easy_handle);
            curl_easy_cleanup(easy_handle);
        }
        break;

        default:
            break;
        }
    }
}

static void 
curl_perform(int fd, short event, void * arg) {
    int running_handles;
    int flags = 0;
    curl_context_t *context;

    if(event & EV_READ)
        flags |= CURL_CSELECT_IN;
    if(event & EV_WRITE)
        flags |= CURL_CSELECT_OUT;

    context = (curl_context_t *) arg;
    curl_multi_socket_action(curl_handle, context->sockfd, flags, &running_handles);
    check_multi_info();
}

static void 
on_timeout(evutil_socket_t fd, short events, void *arg) {
    int running_handles = 0;
    curl_multi_socket_action(curl_handle, CURL_SOCKET_TIMEOUT, 0, &running_handles);
    check_multi_info();
}

static int 
start_timeout(CURLM *multi, long timeout_ms, void *userp) {
    if(timeout_ms < 0)
        evtimer_del(timeout);
    else {
        if(timeout_ms == 0)
            timeout_ms = 1;
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        evtimer_del(timeout);
        evtimer_add(timeout, &tv);
    }
    return 0;
}

static int 
handle_socket(CURL * easy, curl_socket_t s, int action, void * userp, void * socketp) {
    int events = 0;
    curl_context_t * curl_context = nullptr;
    switch(action) {
    case CURL_POLL_IN:
    case CURL_POLL_OUT:
    case CURL_POLL_INOUT:
        curl_context = socketp ? (curl_context_t *) socketp : create_curl_context(s);
        curl_multi_assign(curl_handle, s, (void *)curl_context);
        if(action != CURL_POLL_IN)
            events |= EV_WRITE;
        if(action != CURL_POLL_OUT)
            events |= EV_READ;
        events |= EV_PERSIST;
        event_del(curl_context->event);
        event_assign(curl_context->event, base, curl_context->sockfd, events, curl_perform, curl_context);
        event_add(curl_context->event, NULL);
        break;
    case CURL_POLL_REMOVE:
        if(socketp) {
            event_del(((curl_context_t*)socketp)->event);
            destroy_curl_context((curl_context_t*)socketp);
            curl_multi_assign(curl_handle, s, NULL);
        }
        break;
    }
    return 0;
}

static void 
send_request(const char * url, bool lose_content, curl_httppost * post_form, const char* postdata, size_t postlen) {
    CURL * handle = curl_easy_init();
    webrequest * req = new_request(handle, lose_content);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPIDLE, 1 * 60);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPINTVL, 10);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, req);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, req->error);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 10 * 1000);
    curl_easy_setopt(handle, CURLOPT_URL, url);

    if (post_form) {
      curl_easy_setopt(handle, CURLOPT_HTTPPOST, post_form);
    }
    else if (postdata) {
      curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, postlen);
      curl_easy_setopt(handle, CURLOPT_COPYPOSTFIELDS, postdata);
    }

    curl_multi_add_handle(curl_handle, handle);
}

static int 
start(const char * method) {
    pthread_mutex_lock(&mutex);
    int result = curl_global_init(CURL_GLOBAL_ALL);
    pthread_mutex_unlock(&mutex);
    if(result) {
        return 1;
    }

    event_config * config = event_config_new();
    const char ** pstr = event_get_supported_methods();
    for (int i = 0; nullptr != pstr[i]; i++) {
        if (strcmp(method, pstr[i]) != 0) {
            event_config_avoid_method(config, pstr[i]);
        }
    }
    base =  event_base_new_with_config(config);
    if (!base)
        return 2;

    event_config_free(config);
    timeout = evtimer_new(base, on_timeout, NULL);

    curl_handle = curl_multi_init();
    curl_multi_setopt(curl_handle, CURLMOPT_SOCKETFUNCTION, handle_socket);
    curl_multi_setopt(curl_handle, CURLMOPT_TIMERFUNCTION, start_timeout);

    return 0;
}

typedef void (*push_request)(webrequest *, void *, int);

static void 
failed(push_request f, void * ptr) {
    auto index = 1;
    for (auto iter = requests.begin(); iter != requests.end();) {
        webrequest *& req = iter->second;
        if (req && req->done) {
            f(req, ptr, index++);
            delete req;
            iter = requests.erase(iter);
        } else {
            ++iter;
        }
    }
}

static void 
release() {
    curl_multi_cleanup(curl_handle);
    event_free(timeout);
    event_base_free(base);
    libevent_global_shutdown();
    curl_global_cleanup();
}


#if defined(_LUA_LIB_)

static int 
lstart(lua_State * L) {
    if (base) {
        lua_pushinteger(L, 1);
        return 1;
    }

    const char * method = lua_tostring(L, 1);
    if (!method) {
        lua_pushinteger(L, 1);
        return 1;
    }

    start(method);

    lua_pushinteger(L, 0);
    return 1;
}

static curl_httppost * 
get_postform(lua_State * L, int index) {
    enum { forms_max_index = 7 };
    struct curl_forms forms[forms_max_index + 1];

    int formadd_failed = false;
    curl_httppost * firstitem = nullptr;
    curl_httppost * lastitem = nullptr;

    lua_pushnil(L);
    while (!formadd_failed && lua_next(L, index)) {
        int forms_index = 0;

        lua_pushnil(L);
        while (forms_index < forms_max_index && lua_next(L, -2)) {
            lua_pushvalue(L, -2);

            const char * key = lua_tostring(L, -1);
            size_t value_length = 0;
            const char * value = lua_tolstring(L, -2, &value_length);

            if (strcmp(key, "name") == 0) {
                forms[forms_index].option = CURLFORM_COPYNAME;
                forms[forms_index].value = value;
                forms_index++;
            }
            else if (strcmp(key, "contents") == 0) {
                forms[forms_index].option = CURLFORM_COPYCONTENTS;
                forms[forms_index].value = value;
                forms_index++;

                forms[forms_index].option = CURLFORM_CONTENTSLENGTH;
                forms[forms_index].value = (const char*)value_length;
                forms_index++;
            }
            else if (strcmp(key, "file") == 0) {
                forms[forms_index].option = CURLFORM_FILE;
                forms[forms_index].value = value;
                forms_index++;
            }
            else if (strcmp(key, "content_type") == 0) {
                forms[forms_index].option = CURLFORM_CONTENTTYPE;
                forms[forms_index].value = value;
                forms_index++;
            }
            else if (strcmp(key, "filename") == 0) {
                forms[forms_index].option = CURLFORM_FILENAME;
                forms[forms_index].value = value;
                forms_index++;
            }
            lua_pop(L, 2);
        }
        lua_pop(L, 1);

        forms[forms_index].option = CURLFORM_END;
        CURLFORMcode result = curl_formadd(&firstitem, &lastitem, CURLFORM_ARRAY, forms, CURLFORM_END);
        if (result != CURL_FORMADD_OK) {
            formadd_failed = true;
            break;
        }
    }

    if (formadd_failed) {
        curl_formfree(firstitem);
        return nullptr;
    }
    return firstitem;
}

static int 
lrequest(lua_State * L) {
    if (!base) {
        return luaL_error(L, "Operation failed. The program has not been initialized");
    }

    const char * url = lua_tostring(L, 1);
    if (!url) {
        lua_pushinteger(L, 1);
        return 1;
    }
    bool lose_content = lua_toboolean(L, 2);
	curl_httppost * post_form = nullptr;
	size_t postlen = 0;
	const char * postdata = nullptr;
	int top = lua_gettop(L);
	if (top > 2 && lua_istable(L, 3)) {
		post_form = get_postform(L, 3);
		if (!post_form) {
			return luaL_argerror(L, 3, "post_form invalid!");
		}
	} else if (top > 2 && lua_isstring(L, 3)) {
		postdata = lua_tolstring(L, 3, &postlen);
	}
    send_request(url, lose_content, post_form, postdata, postlen);
    lua_pushinteger(L, 0);
    return 1;
}

static int 
lflush(lua_State * L) {
    if (!base) {
        return luaL_error(L, "Operation failed. The program has not been initialized");
    }
    event_base_loop(base, EVLOOP_NONBLOCK | EVLOOP_ONCE);
    return 0;
}

#define PUSH_FIELD(L, key, value, type) {\
    lua_pushstring(L, (key)); \
    lua_push##type(L, (value)); \
    lua_settable(L, -3); \
}

static int 
lfailed(lua_State * L) {
    lua_newtable(L);
    auto f = [](webrequest * req, void * ptr, int index)->void {
        lua_State * vm = (lua_State *)ptr;
        lua_pushinteger(vm, index);

        lua_newtable(vm);
        PUSH_FIELD(vm, "error", req->error, string);
        PUSH_FIELD(vm, "ip", req->ip, string);
        PUSH_FIELD(vm, "response_code", req->response_code, integer);
        PUSH_FIELD(vm, "url", req->url, string);
        PUSH_FIELD(vm, "content", req->content ? req->content : "NONE", string);
        lua_rawset(vm, -3);
    };
    failed(f, (void *)L);
    return 1;
}

static int 
lrelease(lua_State * L) {
    release();
    return 0;
}

static int 
lsleep(lua_State * L) {
    auto time = lua_tointeger(L, 1);
    if (time > 0)
        usleep(time * 1000);
    return 0;
}

static const struct luaL_Reg methods[] = {
    { "start", lstart }, 
    { "request", lrequest }, 
    { "flush", lflush }, 
    { "failed", lfailed }, 
    { "release", lrelease }, 
    { "sleep", lsleep }, 
    { NULL, NULL }, 
};

extern "C"
int luaopen_httpc(lua_State * L) {
    luaL_checkversion(L);
    luaL_newlib(L, methods);
    return 1;
}

#else
static void 
stdio_callback(struct bufferevent * bev, void * arg) {
    struct evbuffer * evbuf = bufferevent_get_input(bev);
    char * msg = evbuffer_readln(evbuf, NULL, EVBUFFER_EOL_LF);
    if(!msg)
        return;

    if (strcmp(msg, "quit") == 0) {
        release();
        exit(0);
    } else if (strlen(msg) == 0) {
        ;
    } else {
        send_request(msg, true, nullptr, nullptr, 0);
    }
    free(msg);
}

int main(int argc, char **argv) {
    if (start("epoll"))
        return 1;

    struct bufferevent * iobev = bufferevent_socket_new(base, STDIN_FILENO, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(iobev, stdio_callback, NULL, NULL, base);
    bufferevent_enable(iobev, EV_READ | EV_PERSIST);
    // event_base_dispatch(base);
    while (true) {
        event_base_loop(base, EVLOOP_NONBLOCK | EVLOOP_ONCE);
        auto f = [](webrequest * req, void * ptr, int index)->void {
            if (req->done)
                printf("\nfailed response_code : %d, error : %s\n", req->response_code, req->error ? req->error : "NONE");
        };
        failed(f, nullptr);
    }
    release();
    return 0;
}
#endif

