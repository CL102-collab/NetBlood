/* Test-only SDL input and device audit, loaded into ordinary game processes.
 * It sends real key events; it does not call game/network functions or change
 * simulation state. The parent supplies scancode/state pairs through a FIFO.
 */
#define _GNU_SOURCE
#include <SDL.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned windows, audio_devices, injected_events;
static int headless(void) { return getenv("NETWORK_TEST_HEADLESS") != NULL; }

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
    static SDL_Window *(*original)(const char *, int, int, int, int, Uint32);
    if (!original) *(void **)(&original) = dlsym(RTLD_NEXT, "SDL_CreateWindow");
    ++windows;
    if (headless()) { fprintf(stderr, "NETWORK_TEST_FORBIDDEN_WINDOW\n"); _exit(89); }
    return original ? original(title, x, y, w, h, flags) : NULL;
}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int capture, const SDL_AudioSpec *desired,
                                    SDL_AudioSpec *obtained, int changes)
{
    static SDL_AudioDeviceID (*original)(const char *, int, const SDL_AudioSpec *, SDL_AudioSpec *, int);
    if (!original) *(void **)(&original) = dlsym(RTLD_NEXT, "SDL_OpenAudioDevice");
    ++audio_devices;
    if (headless()) { fprintf(stderr, "NETWORK_TEST_FORBIDDEN_AUDIO\n"); _exit(90); }
    return original ? original(device, capture, desired, obtained, changes) : 0;
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
    static int (*original)(SDL_AudioSpec *, SDL_AudioSpec *);
    if (!original) *(void **)(&original) = dlsym(RTLD_NEXT, "SDL_OpenAudio");
    ++audio_devices;
    if (headless()) { fprintf(stderr, "NETWORK_TEST_FORBIDDEN_AUDIO\n"); _exit(90); }
    return original ? original(desired, obtained) : -1;
}

int SDL_PollEvent(SDL_Event *event)
{
    static int (*original)(SDL_Event *);
    static int descriptor = -1;
    static char line[64];
    static size_t length;
    static int barrier_serial, barrier_polls;
    if (!original) *(void **)(&original) = dlsym(RTLD_NEXT, "SDL_PollEvent");
    if (!original) return 0;
    int found = original(event);
    if (found || !event || headless()) return found;
    if (barrier_polls) {
        if (!--barrier_polls)
            fprintf(stderr, "NETWORK_TEST_INPUT_BARRIER_%d\n", barrier_serial);
        return 0;
    }
    const char *path = getenv("NETWORK_TEST_EVENTS");
    if (!path) return 0;
    if (descriptor < 0) descriptor = open(path, O_RDONLY | O_NONBLOCK);
    if (descriptor < 0) return 0;
    char byte;
    for (unsigned count = 0; count < 128 && read(descriptor, &byte, 1) == 1; ++count) {
        if (byte != '\n') {
            if (length+1 < sizeof(line)) line[length++] = byte;
            continue;
        }
        line[length] = 0;
        int scan = -1, down = -1;
        int valid = sscanf(line, "%d %d", &scan, &down) == 2;
        length = 0;
        if (valid && scan == -1 && down > 0) {
            /* End this drain and allow two ordinary event-loop iterations
             * before acknowledging. Deferred menu/bind actions can run before
             * the test queues text intended for the newly opened input mode. */
            barrier_serial = down;
            barrier_polls = 2;
            return 0;
        }
        if (!valid || scan < 0 || scan >= SDL_NUM_SCANCODES || (down != 0 && down != 1)) continue;
        memset(event, 0, sizeof(*event));
        if (!scan) event->type = SDL_QUIT;
        else {
            event->type = down ? SDL_KEYDOWN : SDL_KEYUP;
            event->key.type = event->type;
            event->key.timestamp = SDL_GetTicks();
            event->key.windowID = 1;
            event->key.state = down ? SDL_PRESSED : SDL_RELEASED;
            event->key.keysym.scancode = (SDL_Scancode)scan;
            event->key.keysym.sym = SDL_GetKeyFromScancode((SDL_Scancode)scan);
        }
        ++injected_events;
        fprintf(stderr, "NETWORK_TEST_KEY scan=%d down=%d\n", scan, down);
        return 1;
    }
    return 0;
}

__attribute__((destructor)) static void report(void)
{
    fprintf(stderr, "NETWORK_TEST_DEVICES windows=%u audio=%u events=%u\n", windows, audio_devices, injected_events);
}
