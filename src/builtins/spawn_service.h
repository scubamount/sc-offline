/* sc-offline's spawn.entities service: the C function table the built-in "spawn" plugin
 * publishes through sco_api 1.1 (provide_service). Copy this header into a plugin to use it:
 *
 *   const sc_spawn_service_v1* spawn = NULL;
 *   if (api->size > offsetof(sco_api, query_service) &&
 *       api->query_service(SC_SPAWN_SERVICE_NAME, SC_SPAWN_SERVICE_VERSION,
 *                          (const void**)&spawn) == SCO_OK) { ... }
 *
 * Every function: game thread only (a command, a tick or a run_on_game_thread task); called
 * from another thread they do nothing (spawn_near_player answers "game thread only"). The
 * built-in loads before every plugin and unloads after them, so the table stays valid for as
 * long as your plugin is loaded. Nothing here works when the spawn.ship capability is missing
 * (has("spawn.ship") == 0). Check size before calling a function a later minor adds. */
#ifndef SC_SPAWN_SERVICE_H
#define SC_SPAWN_SERVICE_H
#include <stdint.h>

#define SC_SPAWN_SERVICE_NAME    "spawn.entities"
#define SC_SPAWN_SERVICE_VERSION 0x00010000u /* 1.0 */

typedef struct sc_spawn_service_v1 {
    uint32_t size; /* sizeof(sc_spawn_service_v1) as sc-offline built it */
    /* Spawns an entity class (a ship, a vehicle, an item) at offset metres from you, in your
     * current zone's frame. NULL on success with *out_id = the new entity id; else the reason. */
    const char* (*spawn_near_player)(const char* entity_class, const double offset[3], uint64_t* out_id);
    /* 1 if entity_class is a spawnable class on this game build, else 0. */
    int (*class_exists)(const char* entity_class);
    /* Your entity id, or 0 before you've spawned. */
    uint64_t (*local_player_id)(void);
    /* The ship you're aboard, or 0. */
    uint64_t (*player_ship_id)(void);
} sc_spawn_service_v1;

#endif
