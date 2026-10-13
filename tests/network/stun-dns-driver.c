/* SPDX-License-Identifier: GPL-2.0-or-later
 * Private DNS fixture for stun-live.py. It never resolves the fixture hostname
 * publicly: first lookup fails, subsequent lookups resolve numeric loopback.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <netdb.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

typedef int (*Resolver)(const char*, const char*, const struct addrinfo*, struct addrinfo**);
static Resolver original_resolver;
static pthread_once_t resolver_once = PTHREAD_ONCE_INIT;
static unsigned fixture_attempts;

static void find_resolver(void)
{
    void* symbol = dlsym(RTLD_NEXT, "getaddrinfo");
    memcpy(&original_resolver, &symbol, sizeof(original_resolver));
}

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                struct addrinfo** result)
{
    pthread_once(&resolver_once, find_resolver);
    if (!original_resolver) return EAI_SYSTEM;
    if (node && strcmp(node, "stun-fixture.invalid") == 0)
    {
        if (__sync_fetch_and_add(&fixture_attempts, 1) == 0)
        {
            static const char marker[] = "STUN DNS FIXTURE: first lookup failed locally\n";
            (void)write(STDERR_FILENO, marker, sizeof(marker) - 1);
            if (result) *result = NULL;
            return EAI_AGAIN;
        }
        else
        {
            static const char marker[] = "STUN DNS FIXTURE: retry resolved to loopback\n";
            (void)write(STDERR_FILENO, marker, sizeof(marker) - 1);
            return original_resolver("127.0.0.1", service, hints, result);
        }
    }
    return original_resolver(node, service, hints, result);
}
