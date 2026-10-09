/* sc-offline's teleport.spatial service: where you are, and positions converted between the game's
 * zones. The built-in "teleport" plugin publishes it through sco_api 1.1 (provide_service). Copy
 * this header into a plugin to use it:
 *
 *   const sc_spatial_v1* sp = NULL;
 *   if (api->size > offsetof(sco_api, query_service) &&
 *       api->query_service(SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION,
 *                          (const void**)&sp) == SCO_OK) { ... }
 *
 * Frames and units. Positions are metres, as doubles. A zone is one of the game's nested reference
 * frames (star system > planet > city or station > ship > room); "local" is a position in a zone's
 * own frame, the same frame the game, the spawn.entities service and spawn.txt use for an entity
 * standing in that zone. "World" is the game's root frame. Out at a planet's distance from the star
 * (1e11 m) a double holds a world position to about 1.5e-5 m, so prefer zone_to_zone between two
 * zones over going through world coordinates: it stops at their common ancestor.
 *
 * Ids. Zone and entity ids are the game's own 64-bit ids, opaque: never pointers. Zone id 0 names
 * the world frame in local_to_world, world_to_local and zone_to_zone. An id names something only
 * while it's streamed in; once it streams out the functions answer 0 for it.
 *
 * Every function returns 1 when it answered and 0 when it can't (wrong thread, you haven't
 * spawned, an id that isn't streamed in, a null pointer, teleport unavailable on this game build);
 * outputs are written only on 1. Game thread only (a command, a tick or a run_on_game_thread
 * task): every call reads the game afresh, nothing is kept from an earlier tick. The built-in loads
 * before every plugin and unloads after them, so the table stays valid while your plugin is
 * loaded. Check size before calling a function a later minor adds. */
#ifndef SC_SPATIAL_SERVICE_H
#define SC_SPATIAL_SERVICE_H
#include <stdint.h>

#define SC_SPATIAL_SERVICE_NAME    "teleport.spatial"
#define SC_SPATIAL_SERVICE_VERSION 0x00010000u /* 1.0 */

typedef struct sc_spatial_v1 {
    uint32_t size; /* sizeof(sc_spatial_v1) as sc-offline built it */
    /* Your position (pos, metres) and orientation (rot, a unit quaternion in the game's
     * (x, y, z, w) order) in the zone you're in, and that zone's id. */
    int (*player_pose)(double pos[3], double rot_xyzw[4], uint64_t* zone_id);
    /* The id of the zone an entity is in (your ship's, a spawned entity's). */
    int (*zone_of_entity)(uint64_t entity_id, uint64_t* zone_id);
    /* A position in zone zone_id to the world frame, and back. */
    int (*local_to_world)(uint64_t zone_id, const double local[3], double world[3]);
    int (*world_to_local)(uint64_t zone_id, const double world[3], double local[3]);
    /* A position in zone `from` to zone `to` (either may be 0, the world). */
    int (*zone_to_zone)(uint64_t from, uint64_t to, const double in[3], double out[3]);
    /* The zone's name ("OOC_Stanton_2b_Daymar"), NUL-terminated and cut to fit cap bytes. */
    int (*zone_name)(uint64_t zone_id, char* out, uint32_t cap);
} sc_spatial_v1;

#endif
