/**
 * @file backend_registry.c
 * @brief Static backend registry implementation.
 */

#include "backend_registry.h"

/* Forward declaration — defined in backends/er_backend.c */
extern const game_backend_t er_backend;
/* Forward declaration — defined in backends/ds3_backend.c */
extern const game_backend_t ds3_backend;

static const game_backend_t *const g_backends[] = {
    &er_backend,
    &ds3_backend
};

#define BACKEND_COUNT (sizeof(g_backends) / sizeof(g_backends[0]))

const game_backend_t *backend_registry_get_by_id(game_id_t id) {
    for (size_t i = 0; i < BACKEND_COUNT; i++) {
        if (g_backends[i]->id == id) return g_backends[i];
    }

    return NULL;
}

const game_backend_t *backend_registry_get_default(void) {
    return g_backends[0];
}

size_t backend_registry_count(void) {
    return BACKEND_COUNT;
}

const game_backend_t *backend_registry_get_at(size_t index) {
    if (index >= BACKEND_COUNT) return NULL;
    return g_backends[index];
}
