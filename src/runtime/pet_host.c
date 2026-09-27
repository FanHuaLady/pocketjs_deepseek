#include "runtime/pet_host.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PET_COMMAND_CAPACITY 512
#define PET_REQUEST_CAPACITY 4096

static pthread_t server_thread;
static pthread_mutex_t command_mutex = PTHREAD_MUTEX_INITIALIZER;
static char pending_command[PET_COMMAND_CAPACITY];
static int has_command;
static int listen_fd = -1;
static volatile int server_running;
static PetAnimationConfig animation_config = {
    .idle_seconds = 60,
    .response_multiplier = 3.0,
};

void pet_control_set_animation_config(const PetAnimationConfig *config)
{
    if (config != NULL) animation_config = *config;
}

static int command_has_string_field(const char *body, const char *field)
{
    const char *key = strstr(body, field);
    const char *colon;
    const char *quote;

    if (key == NULL) return 0;
    colon = strchr(key + strlen(field), ':');
    if (colon == NULL) return 0;
    quote = strchr(colon + 1, '"');
    return quote != NULL && strchr(quote + 1, '"') != NULL;
}

static void queue_command(const char *body, size_t length)
{
    if (length >= sizeof(pending_command)) length = sizeof(pending_command) - 1;
    pthread_mutex_lock(&command_mutex);
    memcpy(pending_command, body, length);
    pending_command[length] = '\0';
    has_command = 1;
    pthread_mutex_unlock(&command_mutex);
}

size_t pet_control_poll(char *out, size_t capacity)
{
    size_t length;

    if (out == NULL || capacity == 0) return 0;
    pthread_mutex_lock(&command_mutex);
    if (!has_command) {
        pthread_mutex_unlock(&command_mutex);
        return 0;
    }
    length = strlen(pending_command);
    if (length >= capacity) length = capacity - 1;
    memcpy(out, pending_command, length);
    out[length] = '\0';
    has_command = 0;
    pthread_mutex_unlock(&command_mutex);
    return length;
}

static void send_response(int fd, int status, const char *body)
{
    char response[256];
    int length = (int)strlen(body);
    int written = snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 %d\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
        status,
        length,
        body
    );
    if (written > 0) send(fd, response, (size_t)written, 0);
}

static void handle_client(int fd)
{
    char request[PET_REQUEST_CAPACITY];
    ssize_t received;
    char *body;
    char *content_length_text;
    size_t content_length = 0;

    received = recv(fd, request, sizeof(request) - 1, 0);
    if (received <= 0) return;
    request[received] = '\0';
    body = strstr(request, "\r\n\r\n");
    if (body == NULL) {
        send_response(fd, 400, "{\"error\":\"invalid http request\"}");
        return;
    }
    body += 4;
    content_length_text = strstr(request, "Content-Length:");
    if (content_length_text != NULL) {
        content_length = (size_t)strtoul(content_length_text + 15, NULL, 10);
    }
    if (content_length == 0) content_length = strlen(body);
    if (content_length > (size_t)(request + received - body)) {
        content_length = (size_t)(request + received - body);
    }
    if (!command_has_string_field(body, "\"animation\"") &&
        !command_has_string_field(body, "\"phase\"")) {
        send_response(fd, 400, "{\"error\":\"animation or phase is required\"}");
        return;
    }
    queue_command(body, content_length);
    send_response(fd, 202, "{\"ok\":true}");
}

static void *server_main(void *unused)
{
    (void)unused;
    while (server_running) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (server_running && errno != EINTR) perror("pet accept");
            continue;
        }
        handle_client(client_fd);
        close(client_fd);
    }
    return NULL;
}

int pet_control_start(unsigned port)
{
    struct sockaddr_in address;
    int reuse = 1;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return -1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listen_fd, 4) != 0) {
        close(listen_fd);
        listen_fd = -1;
        return -1;
    }
    server_running = 1;
    if (pthread_create(&server_thread, NULL, server_main, NULL) != 0) {
        server_running = 0;
        close(listen_fd);
        listen_fd = -1;
        return -1;
    }
    return 0;
}

void pet_control_stop(void)
{
    if (!server_running) return;
    server_running = 0;
    shutdown(listen_fd, SHUT_RDWR);
    close(listen_fd);
    listen_fd = -1;
    pthread_join(server_thread, NULL);
}

static JSValue js_pet_poll(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv)
{
    char command[PET_COMMAND_CAPACITY];
    size_t length;

    (void)this_value;
    (void)argc;
    (void)argv;
    length = pet_control_poll(command, sizeof(command));
    if (length == 0) return JS_NewString(ctx, "");
    return JS_NewStringLen(ctx, command, length);
}

int rv1106_pet_install(JSContext *ctx, JSValueConst global)
{
    JSValue pet = JS_NewObject(ctx);
    JSValue config;
    if (JS_IsException(pet)) return 0;
    if (JS_SetPropertyStr(ctx, pet, "poll", JS_NewCFunction(ctx, js_pet_poll, "poll", 0)) < 0) {
        JS_FreeValue(ctx, pet);
        return 0;
    }
    config = JS_NewObject(ctx);
    if (JS_IsException(config) ||
        JS_SetPropertyStr(
            ctx,
            config,
            "idleSeconds",
            JS_NewInt64(ctx, (int64_t)animation_config.idle_seconds)
        ) < 0 ||
        JS_SetPropertyStr(
            ctx,
            config,
            "responseMultiplier",
            JS_NewFloat64(ctx, animation_config.response_multiplier)
        ) < 0 ||
        JS_SetPropertyStr(ctx, pet, "config", config) < 0) {
        JS_FreeValue(ctx, config);
        JS_FreeValue(ctx, pet);
        return 0;
    }
    return JS_SetPropertyStr(ctx, global, "pet", pet) >= 0;
}
