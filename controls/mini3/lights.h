/* Mini 3's six-pixel BGR ring is a single 18-byte framebuffer. */
struct lights {
    int fd, active, volume, steps;
    unsigned char last[18];
    int64_t started, next_frame, yield_until;
};

static int lights_read(struct lights *l, unsigned char frame[18]) {
    return lseek(l->fd, 0, SEEK_SET) >= 0 && read(l->fd, frame, 18) == 18;
}
static int lights_owned(struct lights *l) {
    unsigned char frame[18];
    return lights_read(l, frame) && memcmp(frame, l->last, 18) == 0;
}
static int lights_write(struct lights *l, const unsigned char frame[18]) {
    if (lseek(l->fd, 0, SEEK_SET) < 0 || write(l->fd, frame, 18) != 18)
        return 0;
    memcpy(l->last, frame, 18);
    return 1;
}
static void lights_close(struct lights *l, int restore) {
    if (!l->active)
        return;
    if (restore && lights_owned(l)) {
        const unsigned char dark[18] = {0};
        (void)lights_write(l, dark);
    }
    close(l->fd);
    l->active = 0;
}
static int lights_open(struct lights *l) {
    if (!control_config.ring_prefix)
        return 0;
    int fd = open(control_config.ring_prefix, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return 0;
    l->fd = fd;
    unsigned char frame[18];
    const unsigned char dark[18] = {0};
    if (!lights_read(l, frame) || memcmp(frame, dark, 18) != 0) {
        close(fd);
        return 0;
    }
    memcpy(l->last, dark, 18);
    l->active = 1;
    return 1;
}
static void lights_begin(struct lights *l, enum action action, int paused, int volume, int steps,
                         int64_t now) {
    (void)paused;
    if (action != LOUDER && action != QUIETER)
        return;
    if (now < l->yield_until)
        return;
    if (l->active && !lights_owned(l)) {
        lights_close(l, 0);
        l->yield_until = now + 1000000;
        return;
    }
    if (!l->active && !lights_open(l))
        return;
    l->volume = volume;
    l->steps = steps;
    l->started = l->next_frame = now;
}
static void lights_tick(struct lights *l, int64_t now) {
    if (!l->active || now < l->next_frame)
        return;
    if (!lights_owned(l)) {
        lights_close(l, 0);
        l->yield_until = now + 1000000;
        return;
    }
    enum { HOLD_MS = 300, FADE_MS = 250 };
    int elapsed = (int)((now - l->started) / 1000);
    if (elapsed >= HOLD_MS + FADE_MS) {
        lights_close(l, 1);
        return;
    }
    /* Captured from the stock Mini 3 renderer, BGR order, 0..99 in 5-point steps. */
    static const unsigned char palette[21][3] = {
        {0x01, 0x12, 0x01}, {0x01, 0x12, 0x01}, {0x02, 0x2b, 0x02},
        {0x02, 0x39, 0x05}, {0x02, 0x5f, 0x08}, {0x01, 0x74, 0x0d},
        {0x01, 0x89, 0x14}, {0x01, 0xa3, 0x1f}, {0x00, 0xbd, 0x31},
        {0x00, 0xda, 0x51}, {0x00, 0xf2, 0x83}, {0x00, 0xce, 0xc7},
        {0x00, 0x99, 0xce}, {0x00, 0x7c, 0xce}, {0x00, 0x5e, 0xce},
        {0x00, 0x43, 0xcb}, {0x00, 0x2f, 0xcb}, {0x00, 0x0e, 0xce},
        {0x00, 0x09, 0xd0}, {0x00, 0x03, 0xd8}, {0x00, 0x00, 0xec},
    };
    int percent = l->steps > 0 ? l->volume * 100 / l->steps : 0;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int index = (percent + 2) / 5;
    if (index > 20) index = 20;
    int fade = elapsed < HOLD_MS ? FADE_MS : HOLD_MS + FADE_MS - elapsed;
    unsigned char frame[18];
    for (int pixel = 0; pixel < 6; pixel++)
        for (int color = 0; color < 3; color++)
            frame[pixel * 3 + color] = (unsigned char)(palette[index][color] * fade / FADE_MS);
    if (memcmp(frame, l->last, 18) != 0 && !lights_write(l, frame)) {
        lights_close(l, 0);
        return;
    }
    l->next_frame = elapsed < HOLD_MS ? l->started + HOLD_MS * 1000 : now + 20000;
}
