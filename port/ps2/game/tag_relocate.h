/*
TAG_RELOCATE.H

Moving a map's tags, and a structure BSP, to where they are read when that
is not the address the map linked them to (port/ps2/game/tag_relocate.c).
*/

#ifndef __TAG_RELOCATE_H
#define __TAG_RELOCATE_H
#pragma once

/* ---------- constants */

/* where an Xbox map's tags are linked: the Xbox's tag cache; the PS2 reads
the converter's maps (tools/ps2/convert_map.py), which have been moved to its
own, and so does nothing */
#ifdef HALO_PS2
#include "halo_port_capacity.h"
#define TAG_RELOCATE_XBOX_TAG_CACHE_ADDRESS ((unsigned long)HALO_PORT_TAG_CACHE_ADDRESS)
#else
#define TAG_RELOCATE_XBOX_TAG_CACHE_ADDRESS 0x803A6000UL
#endif

/* ---------- prototypes */

/* the tag_data_size bytes of tags read at tag_header, linked to
linked_address: every pointer they hold into themselves is moved to where
they are. TRUE when they are where they were linked (nothing to do) or have
been moved; FALSE when the two places overlap, which the relocation cannot
tell apart. The tags are untrusted: nothing outside them is read or
written; tag_validate_tags checks them after. */
boolean tag_relocate_tags(
	void *tag_header,
	long tag_data_size,
	unsigned long linked_address);

/* the size bytes of a structure BSP read at base, linked to linked_address,
whose pointers point into itself and into the map's tags (moved already by
tag_relocate_tags: at tag_header, tag_data_size bytes, linked to
tags_linked_address). TRUE or FALSE as tag_relocate_tags. */
boolean tag_relocate_structure_bsp(
	void *base,
	long size,
	unsigned long linked_address,
	void const *tag_header,
	long tag_data_size,
	unsigned long tags_linked_address);

#endif /* __TAG_RELOCATE_H */
