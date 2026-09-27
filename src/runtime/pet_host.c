#include "runtime/pet_host.h"

#include "runtime/mic_host.h"

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
#define PET_HEADER_CAPACITY 8192
#define PET_COMMAND_BODY_CAPACITY 4096
#define PET_AUDIO_BODY_CAPACITY (1024 * 1024)

static pthread_t server_thread;
static pthread_mutex_t command_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t voice_mutex = PTHREAD_MUTEX_INITIALIZER;
static char pending_command[PET_COMMAND_CAPACITY];
static int has_command;
static uint8_t *pending_audio;
static size_t pending_audio_length;
static unsigned pending_audio_rate;
static unsigned pending_audio_channels;
static unsigned long long pending_audio_id;
static int has_audio;
static uint8_t *pending_voice;
static size_t pending_voice_length;
static unsigned pending_voice_rate;
static unsigned pending_voice_channels;
static unsigned long long pending_voice_id;
static int has_voice;
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

void pet_control_push_command(const char *command)
{
    if (command == NULL || command[0] == '\0') return;
    queue_command(command, strlen(command));
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

static int queue_audio(uint8_t *body, size_t length, unsigned rate, unsigned channels)
{
    if (body == NULL || length == 0 || rate == 0 || channels == 0) return -1;
    pthread_mutex_lock(&audio_mutex);
    free(pending_audio);
    pending_audio = body;
    pending_audio_length = length;
    pending_audio_rate = rate;
    pending_audio_channels = channels;
    pending_audio_id++;
    if (pending_audio_id == 0) pending_audio_id = 1;
    has_audio = 1;
    pthread_mutex_unlock(&audio_mutex);
    return 0;
}

int pet_control_queue_voice(uint8_t *data, size_t length, unsigned rate, unsigned channels)
{
    if (data == NULL || length == 0 || rate == 0 || channels == 0) return -1;
    pthread_mutex_lock(&voice_mutex);
    free(pending_voice);
    pending_voice = data;
    pending_voice_length = length;
    pending_voice_rate = rate;
    pending_voice_channels = channels;
    pending_voice_id++;
    if (pending_voice_id == 0) pending_voice_id = 1;
    has_voice = 1;
    pthread_mutex_unlock(&voice_mutex);
    return 0;
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

static void send_empty_response(int fd, int status)
{
    char response[128];
    int written = snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 %d\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        status
    );
    if (written > 0) send(fd, response, (size_t)written, 0);
}

static void send_binary_response(
    int fd,
    const char *content_type,
    const uint8_t *body,
    size_t length,
    unsigned sample_rate,
    unsigned channels
)
{
    char response[320];
    int written = snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 200\r\nContent-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "X-Audio-Rate: %u\r\n"
        "X-Audio-Channels: %u\r\n"
        "Connection: close\r\n\r\n",
        content_type,
        (unsigned long)length,
        sample_rate,
        channels
    );

    if (written > 0) send(fd, response, (size_t)written, 0);
    if (body != NULL && length > 0) send(fd, body, length, 0);
}

static const char *find_header_end(const char *request, size_t length)
{
    size_t i;

    if (length < 4) return NULL;
    for (i = 0; i + 3 < length; i++) {
        if (request[i] == '\r' &&
            request[i + 1] == '\n' &&
            request[i + 2] == '\r' &&
            request[i + 3] == '\n') {
            return request + i;
        }
    }
    return NULL;
}

static const char *find_header_name(const char *header, const char *name)
{
    size_t name_length = strlen(name);
    const char *line = header;

    while (line != NULL && line[0] != '\0') {
        const char *next = strstr(line, "\r\n");
        size_t line_length = next == NULL ? strlen(line) : (size_t)(next - line);

        if (line_length >= name_length &&
            strncasecmp(line, name, name_length) == 0) {
            return line + name_length;
        }
        if (next == NULL) break;
        line = next + 2;
    }
    return NULL;
}

static size_t header_content_length(const char *header)
{
    const char *content_length_text = find_header_name(header, "Content-Length:");

    if (content_length_text != NULL) {
        return (size_t)strtoul(content_length_text, NULL, 10);
    }
    return 0;
}

static unsigned header_unsigned(const char *header, const char *name)
{
    const char *value = find_header_name(header, name);

    if (value == NULL) return 0;
    return (unsigned)strtoul(value, NULL, 10);
}

static int read_body(
    int fd,
    const uint8_t *initial,
    size_t initial_length,
    size_t content_length,
    size_t capacity,
    uint8_t **out
)
{
    uint8_t *body;
    size_t copied;

    if (content_length == 0 || content_length > capacity) return -1;
    body = malloc(content_length + 1);
    if (body == NULL) return -1;
    copied = initial_length > content_length ? content_length : initial_length;
    if (copied > 0) memcpy(body, initial, copied);
    while (copied < content_length) {
        ssize_t received = recv(fd, body + copied, content_length - copied, 0);

        if (received <= 0) {
            free(body);
            return -1;
        }
        copied += (size_t)received;
    }
    body[content_length] = '\0';
    *out = body;
    return 0;
}

static void handle_audio_request(
    int fd,
    const char *header,
    const uint8_t *initial_body,
    size_t initial_body_length,
    size_t content_length
)
{
    uint8_t *body = NULL;
    unsigned rate = header_unsigned(header, "X-Audio-Rate:");
    unsigned channels = header_unsigned(header, "X-Audio-Channels:");

    if (read_body(
        fd,
        initial_body,
        initial_body_length,
        content_length,
        PET_AUDIO_BODY_CAPACITY,
        &body
    ) != 0) {
        send_response(fd, 400, "{\"error\":\"invalid audio body\"}");
        return;
    }
    if (queue_audio(body, content_length, rate, channels) != 0) {
        free(body);
        send_response(fd, 400, "{\"error\":\"invalid audio metadata\"}");
        return;
    }
    send_response(fd, 202, "{\"ok\":true}");
}

static void handle_command_request(
    int fd,
    const uint8_t *initial_body,
    size_t initial_body_length,
    size_t content_length
)
{
    uint8_t *body = NULL;

    if (read_body(
        fd,
        initial_body,
        initial_body_length,
        content_length,
        PET_COMMAND_BODY_CAPACITY,
        &body
    ) != 0) {
        send_response(fd, 400, "{\"error\":\"invalid command body\"}");
        return;
    }
    if (!command_has_string_field((const char *)body, "\"animation\"") &&
        !command_has_string_field((const char *)body, "\"phase\"")) {
        free(body);
        send_response(fd, 400, "{\"error\":\"animation or phase is required\"}");
        return;
    }
    queue_command((const char *)body, content_length);
    free(body);
    send_response(fd, 202, "{\"ok\":true}");
}

static void handle_listen_request(int fd, const char *header)
{
    Rv1106MicCapture capture;
    unsigned seconds = header_unsigned(header, "X-Record-Seconds:");
    unsigned sample_rate = header_unsigned(header, "X-Audio-Rate:");
    unsigned channels = header_unsigned(header, "X-Audio-Channels:");

    if (seconds == 0) seconds = 5;
    if (seconds > 15) seconds = 15;
    if (sample_rate == 0) sample_rate = 16000;
    if (channels == 0) channels = 1;

    if (rv1106_mic_capture_pcm(sample_rate, channels, seconds, &capture) != 0) {
        send_response(fd, 500, "{\"error\":\"microphone capture failed\"}");
        return;
    }
    send_binary_response(
        fd,
        "application/octet-stream",
        capture.data,
        capture.length,
        capture.sample_rate,
        capture.channels
    );
    rv1106_mic_capture_free(&capture);
}

static void handle_voice_request(int fd)
{
    uint8_t *body;
    size_t length;
    unsigned rate;
    unsigned channels;

    pthread_mutex_lock(&voice_mutex);
    if (!has_voice || pending_voice == NULL) {
        pthread_mutex_unlock(&voice_mutex);
        send_empty_response(fd, 204);
        return;
    }
    body = pending_voice;
    length = pending_voice_length;
    rate = pending_voice_rate;
    channels = pending_voice_channels;
    pending_voice = NULL;
    pending_voice_length = 0;
    has_voice = 0;
    pthread_mutex_unlock(&voice_mutex);

    send_binary_response(fd, "application/octet-stream", body, length, rate, channels);
    free(body);
}

static void handle_client(int fd)
{
    char request[PET_HEADER_CAPACITY + 1];
    ssize_t received;
    size_t request_length = 0;
    const char *header_end = NULL;
    size_t header_length;
    size_t content_length;
    const uint8_t *initial_body;
    size_t initial_body_length;
    char method[8] = {0};
    char path[64] = {0};

    while (request_length < PET_HEADER_CAPACITY) {
        received = recv(
            fd,
            request + request_length,
            PET_HEADER_CAPACITY - request_length,
            0
        );
        if (received <= 0) return;
        request_length += (size_t)received;
        header_end = find_header_end(request, request_length);
        if (header_end != NULL) break;
    }
    if (header_end == NULL) {
        send_response(fd, 400, "{\"error\":\"invalid http request\"}");
        return;
    }
    header_length = (size_t)(header_end - request) + 4;
    *((char *)header_end) = '\0';
    if (sscanf(request, "%7s %63s", method, path) != 2) {
        send_response(fd, 404, "{\"error\":\"not found\"}");
        return;
    }
    content_length = header_content_length(request);
    initial_body = (const uint8_t *)request + header_length;
    initial_body_length = request_length - header_length;

    if (strcmp(method, "GET") == 0 && strcmp(path, "/voice") == 0) {
        handle_voice_request(fd);
        return;
    }
    if (strcmp(method, "POST") != 0) {
        send_response(fd, 404, "{\"error\":\"not found\"}");
        return;
    }
    if (strcmp(path, "/audio") == 0) {
        handle_audio_request(
            fd,
            request,
            initial_body,
            initial_body_length,
            content_length
        );
        return;
    }
    if (strcmp(path, "/command") == 0) {
        handle_command_request(
            fd,
            initial_body,
            initial_body_length,
            content_length
        );
        return;
    }
    if (strcmp(path, "/listen") == 0) {
        handle_listen_request(fd, request);
        return;
    }
    send_response(fd, 404, "{\"error\":\"not found\"}");
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

static JSValue js_pet_audio_poll(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv)
{
    char metadata[128];

    (void)this_value;
    (void)argc;
    (void)argv;
    pthread_mutex_lock(&audio_mutex);
    if (!has_audio) {
        pthread_mutex_unlock(&audio_mutex);
        return JS_UNDEFINED;
    }
    snprintf(
        metadata,
        sizeof(metadata),
        "{\"id\":%llu,\"sampleRate\":%u,\"channels\":%u,\"bytes\":%lu}",
        pending_audio_id,
        pending_audio_rate,
        pending_audio_channels,
        (unsigned long)pending_audio_length
    );
    pthread_mutex_unlock(&audio_mutex);
    return JS_NewString(ctx, metadata);
}

static JSValue js_pet_audio_read(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv)
{
    int64_t id;
    JSValue buffer;

    (void)this_value;
    if (argc < 1 || JS_ToInt64(ctx, &id, argv[0]) < 0) return JS_UNDEFINED;
    pthread_mutex_lock(&audio_mutex);
    if (!has_audio || id != (int64_t)pending_audio_id || pending_audio == NULL) {
        pthread_mutex_unlock(&audio_mutex);
        return JS_UNDEFINED;
    }
    buffer = JS_NewArrayBufferCopy(ctx, pending_audio, pending_audio_length);
    if (!JS_IsException(buffer)) {
        free(pending_audio);
        pending_audio = NULL;
        pending_audio_length = 0;
        has_audio = 0;
    }
    pthread_mutex_unlock(&audio_mutex);
    return buffer;
}

static int install_pet_audio(JSContext *ctx, JSValueConst global)
{
    JSValue pet_audio = JS_NewObject(ctx);

    if (JS_IsException(pet_audio)) return 0;
    if (JS_SetPropertyStr(
            ctx,
            pet_audio,
            "poll",
            JS_NewCFunction(ctx, js_pet_audio_poll, "poll", 0)
        ) < 0 ||
        JS_SetPropertyStr(
            ctx,
            pet_audio,
            "read",
            JS_NewCFunction(ctx, js_pet_audio_read, "read", 1)
        ) < 0) {
        JS_FreeValue(ctx, pet_audio);
        return 0;
    }
    return JS_SetPropertyStr(ctx, global, "petAudio", pet_audio) >= 0;
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
    return JS_SetPropertyStr(ctx, global, "pet", pet) >= 0 &&
           install_pet_audio(ctx, global);
}
