/* The player in the game, from the camera update (upstream's sub_000A5070, on the game thread, once a frame):
 * INFINITE CANS (decision log D72) and the light bar in the character's colour, with a flash on a boost (D73).
 *
 * The fields, from upstream's notes and the JSRF decompilation (codeberg.org/xen486/JSRF-Decompilation):
 * the player object (vtable 0x1CCFF8) holds its spray cans at +0xFA0 and the character's most at +0xFA4 (the game
 * clamps pickups to it), the boost timer at +0xE60 (set while a boost runs); player 1's character is the game
 * manager's variable 13 (eGLOBAL_CHARID_P1; g_lpActMan at 0x22FCE0, its variables at +0x7868). */
#include <stdint.h>
#include <stdio.h>

extern int jsrf_ps5_infinite_cans; /* jsrf_ps5_menu.c */

enum {
	PLAYER_VTABLE = 0x1CCFF8,
	CANS = 0xFA0,
	CANS_MOST = 0xFA4,
	BOOST_TIMER = 0xE60,
	ACTMAN = 0x22FCE0,
	ACTMAN_VARS = 0x7868,
	CHARID_P1 = 13,
	CHARACTERS = 24,
	FLASH_FRAMES = 14,
};
#define GUEST_LIMIT (64u << 20)

typedef struct {
	uint8_t r, g, b, reserved;
} pad_color;
int scePadSetLightBar(int handle, const pad_color *color);
int scePadGetHandle(int32_t user, int type, int index);
int sceUserServiceGetInitialUser(int32_t *);
int sceUserServiceGetForegroundUser(int32_t *);

/* By ePLAYERCHARID; picked by eye from each character's look. */
static const uint8_t kColors[CHARACTERS][3] = {
	{255, 120, 0},   /* Beat */
	{255, 30, 0},    /* Corn */
	{0, 255, 90},    /* Gum */
	{255, 210, 0},   /* Combo */
	{150, 0, 255},   /* Rhyth */
	{0, 80, 255},    /* Soda */
	{60, 255, 0},    /* Yoyo */
	{0, 255, 255},   /* NT3000 */
	{255, 0, 70},    /* Garam */
	{255, 90, 190},  /* Boogie */
	{130, 190, 255}, /* Cube */
	{255, 0, 140},   /* Love Shockers */
	{0, 190, 0},     /* Poison Jam */
	{190, 255, 0},   /* Noise Tanks */
	{190, 0, 0},     /* Immortals */
	{255, 60, 0},    /* Doom Riders */
	{30, 0, 255},    /* Rapid 99 */
	{200, 130, 60},  /* Potts */
	{100, 0, 190},   /* Gouji */
	{160, 160, 255}, /* Roboy */
	{255, 0, 0},     /* Clutch */
	{255, 0, 255},   /* Jazz */
	{130, 0, 0},     /* Akumu */
	{0, 200, 160},   /* Zero Beat */
};

static uint32_t guest32(const uint8_t *m, uint32_t va)
{
	return *(const uint32_t *)(m + va);
}

static void light_bar(int character, int flash)
{
	static int pad = -1;
	static unsigned tries;
	static uint32_t shown = ~0u;
	if (pad < 0) {
		int32_t user = -1;
		if (tries++ % 120 != 0)
			return;
		if (sceUserServiceGetInitialUser(&user) != 0 && sceUserServiceGetForegroundUser(&user) != 0)
			return;
		pad = scePadGetHandle(user, 0, 0);
		if (pad < 0)
			return;
	}
	/* the flash: white, fading back to the character's colour */
	const uint8_t *c = kColors[character];
	pad_color col;
	col.r = (uint8_t)(c[0] + (255 - c[0]) * flash / FLASH_FRAMES);
	col.g = (uint8_t)(c[1] + (255 - c[1]) * flash / FLASH_FRAMES);
	col.b = (uint8_t)(c[2] + (255 - c[2]) * flash / FLASH_FRAMES);
	col.reserved = 0;
	uint32_t key = (uint32_t)col.r << 16 | (uint32_t)col.g << 8 | col.b;
	if (key == shown)
		return;
	if (scePadSetLightBar(pad, &col) == 0)
		shown = key;
}

void jsrf_ps5_player_step(uint8_t *m, uint32_t player)
{
	static int last_character = -1, flash, other_seen;
	static uint32_t last_timer;
	if (player < 0x10000u || player + 0x1000u >= GUEST_LIMIT)
		return;
	/* the camera may follow another object in a scene: only the player's fields are touched */
	if (guest32(m, player) != PLAYER_VTABLE) {
		if (!other_seen) {
			other_seen = 1;
			fprintf(stderr, "jsrf: game: the camera follows %08X (vtable %08X), not the player\n", player,
			        guest32(m, player));
		}
		return;
	}
	uint32_t *pl = (uint32_t *)(m + player);
	if (jsrf_ps5_infinite_cans) {
		int32_t most = (int32_t)pl[CANS_MOST / 4];
		if (most > 0 && most <= 1000 && (int32_t)pl[CANS / 4] < most)
			pl[CANS / 4] = (uint32_t)most;
	}

	uint32_t timer = pl[BOOST_TIMER / 4];
	if (timer && !last_timer)
		flash = FLASH_FRAMES;
	last_timer = timer;

	uint32_t am = guest32(m, ACTMAN);
	if (am < 0x10000u || am + ACTMAN_VARS + 0x100u >= GUEST_LIMIT)
		return;
	int character = (int32_t)guest32(m, am + ACTMAN_VARS + CHARID_P1 * 4);
	if (character < 0 || character >= CHARACTERS)
		return;
	if (character != last_character) {
		fprintf(stderr, "jsrf: game: player 1 is character %d, the light bar %02X%02X%02X\n", character,
		        kColors[character][0], kColors[character][1], kColors[character][2]);
		last_character = character;
	}
	light_bar(character, flash);
	if (flash > 0)
		flash--;
}
