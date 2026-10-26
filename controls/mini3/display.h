/* Four seven-segment digits plus DEGREE, DH, DL and DM on Mini 3. */
#include <stdio.h>
#include <time.h>

enum { DISPLAY_LEDS = 32, DISPLAY_VOLUME = 1, DISPLAY_SPECTRUM = 2, DISPLAY_ANIMATION = 3 };
struct display {
    int fd[DISPLAY_LEDS], active, brightness, mode, direction;
    unsigned char saved[DISPLAY_LEDS], last[DISPLAY_LEDS];
    int64_t saved_minute, until, check_at, next_frame, yield_until;
};
#define DISPLAY_DIRECTION(d, up) ((d)->direction = (up) ? 1 : -1)

static const unsigned char display_digits[10] = {63, 6, 91, 79, 102, 109, 125, 7, 127, 111};
static int display_read(int fd) {
    char text[8];
    if (lseek(fd, 0, SEEK_SET) < 0)
        return -1;
    ssize_t n = read(fd, text, sizeof text);
    if (n <= 0)
        return -1;
    int value = 0, digits = 0;
    for (ssize_t i = 0; i < n && text[i] >= '0' && text[i] <= '9'; i++) {
        value = value * 10 + text[i] - '0';
        digits++;
        if (value > 255)
            return -1;
    }
    return digits ? value : -1;
}
static int display_write(int fd, int value) {
    char text[5];
    int n = snprintf(text, sizeof text, "%d\n", value);
    return n > 0 && n < (int)sizeof text && lseek(fd, 0, SEEK_SET) >= 0 &&
           write(fd, text, (size_t)n) == n;
}
static int display_digit(const unsigned char frame[DISPLAY_LEDS], int position) {
    unsigned mask = 0;
    for (int segment = 0; segment < 7; segment++)
        if (frame[position * 7 + segment]) mask |= 1u << segment;
    if (position == 0 && !mask)
        return 0;
    for (int digit = 0; digit < 10; digit++)
        if (mask == display_digits[digit]) return digit;
    return -1;
}
static int display_clock_minutes(const unsigned char frame[DISPLAY_LEDS]) {
    if (frame[28] || frame[31])
        return -1;
    int digits[4];
    for (int i = 0; i < 4; i++) {
        digits[i] = display_digit(frame, i);
        if (digits[i] < 0) return -1;
    }
    int hour = digits[0] * 10 + digits[1], minute = digits[2] * 10 + digits[3];
    return hour < 24 && minute < 60 ? hour * 60 + minute : -1;
}
static void display_time_frame(unsigned char frame[DISPLAY_LEDS], int minutes, int brightness) {
    int digits[4] = {minutes / 600, minutes / 60 % 10, minutes % 60 / 10, minutes % 10};
    for (int position = 0; position < 4; position++)
        for (int segment = 0; segment < 7; segment++)
            frame[position * 7 + segment] =
                display_digits[digits[position]] & (1u << segment) ? (unsigned char)brightness : 0;
}
static int display_owned(struct display *d) {
    /* The native clock may blink the colon while a volume number is shown. */
    for (int i = 0; i < 28; i++)
        if (display_read(d->fd[i]) != d->last[i]) return 0;
    return 1;
}
static void display_close(struct display *d) {
    if (!d->active) return;
    if (display_owned(d)) {
        unsigned char restored[DISPLAY_LEDS];
        memcpy(restored, d->saved, sizeof restored);
        int64_t delta = (int64_t)time(NULL) / 60 - d->saved_minute;
        int minutes = display_clock_minutes(d->saved);
        if (minutes >= 0 && delta > 0 && delta <= 10)
            display_time_frame(restored, (minutes + (int)delta) % 1440, d->brightness);
        for (int i = 0; i < DISPLAY_LEDS; i++)
            if (display_read(d->fd[i]) == d->last[i] || i >= 28)
                (void)display_write(d->fd[i], restored[i]);
    }
    for (int i = 0; i < DISPLAY_LEDS; i++) close(d->fd[i]);
    d->active = d->mode = 0;
}
static int display_open(struct display *d) {
    if (!control_config.display_prefix) return 0;
    static const char *extra[] = {"DEGREE", "DH", "DL", "DM"};
    int count = 0, brightness = 0;
    for (int i = 0; i < DISPLAY_LEDS; i++) {
        char path[160];
        int n;
        if (i < 28)
            n = snprintf(path, sizeof path, "%s%c%d/brightness", control_config.display_prefix,
                         'A' + i % 7, i / 7);
        else
            n = snprintf(path, sizeof path, "%s%s/brightness", control_config.display_prefix,
                         extra[i - 28]);
        if (n < 0 || n >= (int)sizeof path) goto fail;
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) goto fail;
        d->fd[count++] = fd;
        int value = display_read(fd);
        if (value < 0) goto fail;
        d->saved[i] = d->last[i] = (unsigned char)value;
        if (i < 28 && value > brightness) brightness = value;
    }
    if (display_clock_minutes(d->saved) < 0) goto fail;
    d->brightness = brightness ? brightness : 44;
    d->saved_minute = (int64_t)time(NULL) / 60;
    d->active = 1;
    return 1;
fail:
    for (int i = 0; i < count; i++) close(d->fd[i]);
    return 0;
}
static int display_frame(struct display *d, const unsigned char frame[DISPLAY_LEDS]) {
    for (int i = 0; i < DISPLAY_LEDS; i++)
        if (frame[i] != d->last[i]) {
            if (!display_write(d->fd[i], frame[i])) {
                display_close(d);
                return 0;
            }
            d->last[i] = frame[i];
        }
    return 1;
}
static void display_show(struct display *d, int percent, int64_t now) {
    if (d->active && !display_owned(d)) {
        display_close(d);
        d->yield_until = now + 1500000;
        return;
    }
    if (now < d->yield_until) return;
    if (!d->active && !display_open(d)) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    percent = (percent + 2) / 5 * 5;
    if (percent > 99) percent = 99;
    char label[5];
    if (d->direction < 0) snprintf(label, sizeof label, "%-4d", percent);
    else snprintf(label, sizeof label, "%4d", percent);
    unsigned char frame[DISPLAY_LEDS] = {0};
    for (int position = 0; position < 4; position++) {
        if (label[position] < '0' || label[position] > '9') continue;
        unsigned mask = display_digits[label[position] - '0'];
        for (int segment = 0; segment < 7; segment++)
            if (mask & (1u << segment))
                frame[position * 7 + segment] = (unsigned char)d->brightness;
    }
    if (!display_frame(d, frame)) return;
    d->mode = DISPLAY_VOLUME;
    d->until = now + 2000000;
    d->check_at = now + 50000;
}
static void display_play(struct display *d, const struct display_animation *animation,
                         int64_t now) {
    (void)d; (void)animation; (void)now;
}
static void display_tick(struct display *d, int64_t now) {
    if (!d->active || now < d->check_at) return;
    if (!display_owned(d)) {
        display_close(d);
        d->yield_until = now + 1500000;
    } else if (now >= d->until) {
        display_close(d);
    } else {
        d->check_at = now + 50000;
    }
}
static void display_spectrum(struct display *d, const unsigned char *levels, int enabled,
                             int64_t now) {
    (void)d; (void)levels; (void)enabled; (void)now;
}
