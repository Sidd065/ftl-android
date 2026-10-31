// Fixes for bugs in the game itself (the desktop builds have them too).
#include "fixes.h"

#include <stdint.h>
#include <string.h>

#include "../loader/macho.h"
#include "../patches.h"
#include "game.h"
#include "port_log.h"

#define AT(T, p, off) (*(T *)((uint8_t *)(p) + (off)))

static mo_image *g_img;

// libc++ std::string (Apple's alternate layout)
static const char *cxx_str(const void *s, size_t *len) {
    const uint8_t *b = s;
    if (b[23] & 0x80) {
        *len = *(const size_t *)(b + 8);
        return *(const char *const *)b;
    }
    *len = b[23];
    return (const char *)b;
}

// Crew members keep pointers to doors: the one they're walking through (closed behind
// them at their next step), the one in their way and the one they're attacking. The dead
// keep theirs, and a boarding drone outlives its death: whenever a new enemy ship shows
// up, the game adds every boarding drone the player owns to its crew, dead ones too
// (ShipManager::SetTarget). So a drone that died in a doorway still points at that door
// after its ship is deleted on the next jump, and when the next enemy ship is destroyed,
// ShipManager::KillEveryone > CrewMember::Kill closes the deleted door, which can crash.
// When a door is deleted, everyone forgets it.
static const struct { int off; const char *what; } CREW_DOOR_FIELDS[] = {
    {CREW_LAST_DOOR, "the door they were walking through"},
    {CREW_BLOCKING_DOOR, "the door in their way"},
    {CREW_TARGET, "what they were attacking"},
};

static void (*g_door_destroy)(void *door);
static void door_destroy_hook(void *door) {
    uint8_t *factory = mo_ptr(g_img, G_CREW_FACTORY);
    void **crew = AT(void **, factory, FACTORY_CREW), **end = AT(void **, factory, FACTORY_CREW + 8);
    for (; crew && crew < end; crew++) {
        uint8_t *c = *crew;
        if (!c) continue;
        for (size_t i = 0; i < sizeof CREW_DOOR_FIELDS / sizeof *CREW_DOOR_FIELDS; i++) {
            if (AT(void *, c, CREW_DOOR_FIELDS[i].off) != door) continue;
            size_t n;
            const char *species = cxx_str(c + CREW_SPECIES, &n);
            int ship = AT(int, c, CREW_SHIP);
            LOGI("fixes: one of the %s crew (%.*s, on %s) still had a deleted door as %s; cleared",
                 AT(int, c, CREW_OWNER) == 0 ? "player's" : "enemy's", (int)n, species,
                 ship == 0 ? "the player's ship" : ship == 1 ? "the enemy ship" : "no ship", CREW_DOOR_FIELDS[i].what);
            AT(void *, c, CREW_DOOR_FIELDS[i].off) = NULL;
        }
    }
    g_door_destroy(door);
}

int fixes_install(mo_image *img) {
    g_img = img;
    g_door_destroy = patch_hook(img, G_DOOR_DESTROY, (void *)door_destroy_hook, 1);
    if (!g_door_destroy) {
        LOGW("fixes: couldn't hook door deletion; crew can still use doors of deleted ships");
        return -1;
    }
    return 0;
}
