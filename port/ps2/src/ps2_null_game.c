/*
PS2_NULL_GAME.C

What the PS2 build leaves out of the port's game code, as the Xbox's own
game plays without it (docs/ps2_port_plan.md, "Relocating the tag data"):

- Halo Custom Edition maps, which are made for PC memory budgets: no map is
  one, and none loads;
- the tag validator and the tag relocation, with the tag schema they read
  (megabytes of tables and 262 KB of working memory): the PS2 reads the
  converter's maps (tools/ps2/convert_map.py), whose runtime values are
  reset and whose tags are already where the PS2's tag cache is.

The returns are what the code they stand for returns for an Xbox map on the
other builds.
*/

#include "platform.h"

#include <stddef.h>

/* the types are the game's (the callers pass them on); only their sizes
matter here */
typedef unsigned char boolean_t;

boolean_t custom_edition_level_name(char const *level_name) { return 0; }
boolean_t custom_edition_map_file_present(char const *map_name) { return 0; }
unsigned long custom_edition_map_checksum(char const *level_name) { return 0; }
boolean_t custom_edition_install_present(void) { return 0; }
boolean_t custom_edition_cache_playable(char const *level_name) { return 0; }
boolean_t custom_edition_cache_present(char const *level_name, unsigned long checksum, char *message,
	long message_size)
{
	if (message_size > 0)
		message[0] = '\0';
	return 0;
}
boolean_t custom_edition_cache_campaign(char const *file_name) { return 0; }
boolean_t custom_edition_cache_multiplayer(char const *file_name) { return 0; }
boolean_t custom_edition_cache_refuse(void const *header, char const *build, char const *path) { return 0; }
void *custom_edition_cache_tags_load(char const *map_name, void *header) { return NULL; }
boolean_t custom_edition_cache_tags_loaded(void) { return 0; }
void custom_edition_cache_tags_unload(void) { }
boolean_t custom_edition_cache_relies_on(short behaviour) { return 0; }
void custom_edition_cache_read(long tag_index, long offset, long size, void *buffer) { }
boolean_t custom_edition_structure_bsp_reference_valid(void const *reference) { return 0; }
boolean_t custom_edition_structure_bsp_load(void *structure_bsp) { return 0; }
void custom_edition_structure_bsp_unload(void) { }
short custom_edition_part_palette(void const *vertex_buffer, unsigned char const **nodes) { return 0; }
unsigned char *custom_edition_sounds_encode(unsigned char const *data, long data_bytes, short compression,
	boolean_t big_endian, long channels, long rate, long encoded_rate, unsigned long *encoded_bytes)
{
	return NULL;
}
boolean_t custom_edition_vehicles_by_placement(void) { return 0; }
/* (a vehicle's placement is placed: the stock game's) */
boolean_t custom_edition_vehicle_placement_allowed(void const *placement) { return 1; }

boolean_t tag_relocate_tags(void *tag_header, long tag_data_size, unsigned long linked_address) { return 1; }
boolean_t tag_relocate_structure_bsp(void *base, long size, unsigned long linked_address, void const *tag_header,
	long tag_data_size, unsigned long tags_linked_address)
{
	return 1;
}
boolean_t tag_validate_tags(void *tag_header, long tag_data_size, long file_length, char const *map_name) { return 1; }
boolean_t tag_validate_structure_bsp(long tag_index, void *base, long size) { return 1; }
