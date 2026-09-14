#include "../../native/input_protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <linux/input.h>

#define CF_PANEL_NAME "NVTCapacitiveTouchScreen"

// Write one protocol-B contact update into the touch panel's own evdev node.
// The events look exactly like firmware contacts: same device, same protocol,
// a dedicated slot and tracking id. Returns 0 on success.
static int emit(const CfTouchCommand *c) {
    static int panel_fd = -1;
    if (panel_fd < 0) {
        for (int node = 0; node < 64 && panel_fd < 0; ++node) {
            char path[32];
            snprintf(path, sizeof(path), "/dev/input/event%d", node);
            int fd = open(path, O_WRONLY | O_CLOEXEC);
            if (fd < 0) continue;
            char name[64];
            memset(name, 0, sizeof(name));
            if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
                strncmp(name, CF_PANEL_NAME, sizeof(CF_PANEL_NAME)) == 0) {
                panel_fd = fd;
            } else {
                close(fd);
            }
        }
        if (panel_fd < 0) return ENODEV;
    }
    struct input_event events[8];
    memset(events, 0, sizeof(events));
    size_t count = 0;
    events[count].type = EV_ABS; events[count].code = ABS_MT_SLOT;
    events[count].value = CF_TOUCH_SLOT; ++count;
    if (c->action == CF_TOUCH_ACTION_DOWN) {
        events[count].type = EV_ABS; events[count].code = ABS_MT_TRACKING_ID;
        events[count].value = CF_TOUCH_TRACKING; ++count;
        events[count].type = EV_ABS; events[count].code = ABS_MT_POSITION_X;
        events[count].value = c->raw_x; ++count;
        events[count].type = EV_ABS; events[count].code = ABS_MT_POSITION_Y;
        events[count].value = c->raw_y; ++count;
        events[count].type = EV_ABS; events[count].code = ABS_MT_TOUCH_MAJOR;
        events[count].value = 12; ++count;
        events[count].type = EV_ABS; events[count].code = ABS_MT_PRESSURE;
        events[count].value = 1; ++count;
        // Android InputReader treats an MT slot with pressure but no
        // BTN_TOUCH as hover. Assert the panel's touch key so this becomes a
        // real ACTION_DOWN/MOVE stream instead of HOVER_ENTER/HOVER_EXIT.
        events[count].type = EV_KEY; events[count].code = BTN_TOUCH;
        events[count].value = 1; ++count;
    } else if (c->action == CF_TOUCH_ACTION_MOVE) {
        events[count].type = EV_ABS; events[count].code = ABS_MT_POSITION_X;
        events[count].value = c->raw_x; ++count;
        events[count].type = EV_ABS; events[count].code = ABS_MT_POSITION_Y;
        events[count].value = c->raw_y; ++count;
    } else { // UP and RESET both release the contact.
        events[count].type = EV_ABS; events[count].code = ABS_MT_TRACKING_ID;
        events[count].value = -1; ++count;
        events[count].type = EV_KEY; events[count].code = BTN_TOUCH;
        events[count].value = 0; ++count;
    }
    events[count].type = EV_SYN; events[count].code = SYN_REPORT;
    events[count].value = 0; ++count;
    const char *cursor = (const char *)events;
    size_t remaining = count * sizeof(events[0]);
    while (remaining > 0) {
        ssize_t written = write(panel_fd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        cursor += written;
        remaining -= (size_t)written;
    }
    return 0;
}

static void handle_client(int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int contact_down = 0;
    for (;;) {
        CfTouchCommand command;
        size_t have = 0;
        while (have < sizeof(command)) {
            ssize_t n = read(fd, (char *)&command + have, sizeof(command) - have);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) goto done;
            have += (size_t)n;
        }
        if (!cf_touch_valid(&command)) goto done;
        int result = emit(&command);
        if (result == 0 && command.action == CF_TOUCH_ACTION_DOWN) contact_down = 1;
        if (command.action == CF_TOUCH_ACTION_UP || command.action == CF_TOUCH_ACTION_RESET)
            contact_down = 0;
        if (send(fd, &result, sizeof(result), MSG_NOSIGNAL) != sizeof(result)) goto done;
    }
done:
    if (contact_down) {
        // Never leave a virtual contact pressed when the app goes away.
        CfTouchCommand release = {CF_TOUCH_MAGIC, CF_TOUCH_ACTION_UP,
                                  CF_TOUCH_SLOT, CF_TOUCH_TRACKING, 0, 0};
        emit(&release);
    }
}

void cf_input_companion(int fd) {
    handle_client(fd);
    close(fd);
}

#ifdef CF_COMPANION_STANDALONE
// Test-only loopback entry. This executable is never included in the module ZIP.
#include <arpa/inet.h>
int main(void) {
    int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) return 1;
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {.sin_family=AF_INET, .sin_port=htons(27045), .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) || listen(server, 1)) return 2;
    int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
    close(server);
    if (client < 0) return 3;
    cf_input_companion(client);
    return 0;
}
#endif
