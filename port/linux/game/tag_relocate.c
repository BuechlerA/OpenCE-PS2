/*
TAG_RELOCATE.C

Moving a map's tags, and its structure BSPs, to where they are read, when
that is not where the map linked them (tag_relocate.h).

An Xbox map's tags are a memory image linked to 0x803A6000, the Xbox's tag
cache, and each structure BSP to its own address in that 22 MB cache (the
top of it). The PS2 cannot give a program those addresses (they are the
kernel's), nor 22 MB, so its build reads the tags and each BSP elsewhere and
moves every pointer they hold: each word that points into the linked tags
(or BSP) gets the difference between where they are and where they were
linked. The other builds read the tags where the map linked them, and this
does nothing.

The pointers are found as tools/ps2/relocation_check.py finds them, which
checks on every map of the retail disc that nothing else in the tags points
into them (docs/ps2_port_plan.md, "Relocating the tag data"):
- the tag header: the tag index, and the vertex and index buffer headers;
- each tag in the index: its name and its root;
- each buffer header's Data (the buffer's address, which registering it
  makes physical: cache_files_windows.c);
- every block, data and tag reference the tag schema lists, in each tag's
  root and blocks (port/linux/game/tag_schema.h);
- the pointers the schema does not list (it lists what the validator
  checks), in extra_pointers below;
- the name of every tag reference in a tag's root or block element: a word
  after a group tag that points into the tag names, before a name length
  under 256 (the schema lists the references it checks).
A structure BSP has a header of its own (its root, and its vertex and
lightmap vertex buffer headers), and its pointers point into itself and
into the tags.

What the relocation reads is untrusted: an address that is not in the tags
(or BSP) is left, a block that does not fit is not walked, and nothing
outside the bytes read is read or written. tag_validate_tags checks the
moved tags after.
*/

/* ---------- headers */

#include "cseries.h"
#include "tag_relocate.h"
#include "tag_schema.h"

#include <string.h>

/* ---------- constants */

enum
{
	/* struct cache_file_tag_header (cache_files.c) */
	TAG_HEADER_INSTANCES = 0x00,
	TAG_HEADER_COUNT = 0x0C,
	TAG_HEADER_VERTEX_BUFFER_COUNT = 0x10,
	TAG_HEADER_VERTEX_BUFFERS = 0x14,
	TAG_HEADER_INDEX_BUFFER_COUNT = 0x18,
	TAG_HEADER_INDEX_BUFFERS = 0x1C,
	TAG_HEADER_SIZE = 0x28,
	/* struct cache_file_tag_instance */
	TAG_INSTANCE_GROUP = 0x00,
	TAG_INSTANCE_NAME = 0x10,
	TAG_INSTANCE_ADDRESS = 0x14,
	TAG_INSTANCE_SIZE = 0x20,
	/* struct cache_file_structure_bsp_header */
	BSP_HEADER_ROOT = 0x00,
	BSP_HEADER_VERTEX_BUFFER_COUNT = 0x04,
	BSP_HEADER_VERTEX_BUFFERS = 0x08,
	BSP_HEADER_LIGHTMAP_VERTEX_BUFFER_COUNT = 0x0C,
	BSP_HEADER_LIGHTMAP_VERTEX_BUFFERS = 0x10,
	BSP_HEADER_SIZE = 0x18,
	/* D3DVertexBuffer, D3DIndexBuffer: Common, Data, Lock */
	BUFFER_HEADER_DATA = 0x04,
	BUFFER_HEADER_SIZE = 0x0C,
	/* struct tag_block: count, address; struct tag_data: size, flags,
	file offset, address; struct tag_reference: group tag, name, name
	length, index */
	BLOCK_ADDRESS = 0x04,
	DATA_ADDRESS = 0x0C,
	REFERENCE_NAME = 0x04,
	MAXIMUM_NAME_LENGTH = 256,
	MAXIMUM_DEPTH = 24,
};

enum extra_kind
{
	_extra_pointer,
	_extra_block,
	_extra_data,
};

/* ---------- structures */

struct extra
{
	short kind;
	short offset;
	/* a block's elements' size, and their own extras */
	short element_size;
	struct extra const *elements;
};

struct extra_definition
{
	char const *name;
	struct extra const *extras;
};

struct extra_group
{
	unsigned long group_tag;
	/* the size of the group's root (the schema has no definition of it) */
	long root_size;
	struct extra const *extras;
};

/* a place the relocated bytes may point into, as linked */
struct range
{
	unsigned long linked;
	unsigned long size;
	unsigned long delta;
};

struct relocation
{
	byte *buffer;
	unsigned long linked;
	unsigned long size;
	/* the buffer's own, and for a BSP the tags' */
	struct range ranges[2];
	short range_count;
	/* the tag names, as linked */
	unsigned long names_low;
	unsigned long names_high;
};

/* ---------- globals */

/* The pointers the tag schema does not list, by definition: what
tools/ps2/relocation_check.py's EXTRA_POINTERS lists, the same. */

static struct extra const end_of_extras = { NONE, 0, 0, NULL };

static struct extra const model_geometry_part_extras[] =
{
	/* the triangle buffer's base_address and hardware_format (its index
	data and D3D header), the vertex buffer's hardware_format */
	{ _extra_pointer, 0x4C, 0, NULL },
	{ _extra_pointer, 0x50, 0, NULL },
	{ _extra_pointer, 0x64, 0, NULL },
	{ NONE, 0, 0, NULL }
};

static struct extra const structure_material_extras[] =
{
	/* the vertices' and the lightmap vertices' vertex buffers' hardware_format */
	{ _extra_pointer, 0xC0, 0, NULL },
	{ _extra_pointer, 0xD4, 0, NULL },
	{ NONE, 0, 0, NULL }
};

/* (Halo 1's layouts: a child scenario is a tag reference and 16 bytes; a
comment, a point, 16 bytes and its text; a structure bsp reference, its file
offset, size, address, 4 bytes and a tag reference, whose name is at 0x14 -
its address is where the loader puts it) */
static struct extra const child_scenario_extras[] =
{
	{ _extra_pointer, 0x04, 0, NULL },
	{ NONE, 0, 0, NULL }
};

static struct extra const comment_extras[] =
{
	{ _extra_data, 0x28, 0, NULL },
	{ NONE, 0, 0, NULL }
};

static struct extra const structure_bsp_reference_extras[] =
{
	{ _extra_pointer, 0x14, 0, NULL },
	{ NONE, 0, 0, NULL }
};

static struct extra const scenario_extras[] =
{
	{ _extra_block, 0x44, 0x20, child_scenario_extras },
	{ _extra_data, 0x110, 0, NULL },
	{ _extra_block, 0x11C, 0x30, comment_extras },
	/* decals */
	{ _extra_block, 0x3AC, 0, NULL },
	{ _extra_block, 0x5A8, 0x20, structure_bsp_reference_extras },
	{ NONE, 0, 0, NULL }
};

static struct extra const structure_bsp_extras[] =
{
	/* markers */
	{ _extra_block, 0x244, 0, NULL },
	{ NONE, 0, 0, NULL }
};

static struct extra const particle_system_extras[] =
{
	{ _extra_block, 0x54, 0, NULL },
	{ NONE, 0, 0, NULL }
};

static struct extra const game_globals_extras[] =
{
	{ _extra_block, 0x1A4, 0, NULL },
	{ NONE, 0, 0, NULL }
};

static struct extra_definition const extra_definitions[] =
{
	{ "model_geometry_part", model_geometry_part_extras },
	{ "structure_material", structure_material_extras },
	{ "scenario", scenario_extras },
	{ "structure_bsp", structure_bsp_extras },
	{ "particle_system", particle_system_extras },
	{ "game_globals", game_globals_extras },
	{ NULL, NULL }
};

/* a meter's (which the schema has no definition of): its encoded stencil data */
static struct extra const meter_extras[] =
{
	{ _extra_data, 0xA4, 0, NULL },
	{ NONE, 0, 0, NULL }
};

static struct extra_group const extra_groups[] =
{
	{ 'metr', 0xAC, meter_extras },
	{ 0, 0, NULL }
};

/* ---------- private code */

static unsigned long read_word(
	byte const *address)
{
	unsigned long value;

	csmemcpy(&value, address, sizeof(value));
	return value;
}

static void write_word(
	byte *address,
	unsigned long value)
{
	csmemcpy(address, &value, sizeof(value));
}

/* the offset in the buffer of size bytes at a linked address, or NONE */
static long buffer_offset(
	struct relocation const *relocation,
	unsigned long address,
	unsigned long size)
{
	unsigned long offset = address - relocation->linked;

	if (offset >= relocation->size || size > relocation->size - offset)
		return NONE;
	return (long)offset;
}

/* moves the word at offset, if it points into one of the ranges: TRUE if it did */
static boolean relocate_word(
	struct relocation *relocation,
	long offset)
{
	unsigned long value;
	short index;

	if (offset < 0 || (unsigned long)offset > relocation->size - sizeof(unsigned long))
		return FALSE;
	value = read_word(relocation->buffer + offset);
	for (index = 0; index < relocation->range_count; index++)
	{
		struct range const *range = &relocation->ranges[index];

		if (value - range->linked < range->size)
		{
			write_word(relocation->buffer + offset, value + range->delta);
			return TRUE;
		}
	}
	return FALSE;
}

/* the buffer headers' Data: count headers at the linked address the word at
offset holds (read before that word is moved) */
static void relocate_buffer_headers(
	struct relocation *relocation,
	long count_offset,
	long address_offset)
{
	unsigned long address = read_word(relocation->buffer + address_offset);
	long count = (long)read_word(relocation->buffer + count_offset);
	long offset;
	long index;

	if (count <= 0 || count > (long)(relocation->size / BUFFER_HEADER_SIZE))
		return;
	offset = buffer_offset(relocation, address, (unsigned long)count * BUFFER_HEADER_SIZE);
	if (offset == NONE)
		return;
	for (index = 0; index < count; index++)
		relocate_word(relocation, offset + index * BUFFER_HEADER_SIZE + BUFFER_HEADER_DATA);
}

static boolean is_group_tag(
	unsigned long word)
{
	short index;

	for (index = 0; index < 4; index++)
	{
		unsigned long character = (word >> (index * 8)) & 0xFF;

		if (character < 32 || character >= 127)
			return FALSE;
	}
	return TRUE;
}

/* the names of the tag references in size bytes at offset (a root or a
block element) */
static void relocate_reference_names(
	struct relocation *relocation,
	long offset,
	unsigned long size)
{
	long word;
	long end = offset + (long)size;

	for (word = offset + ((-offset) & 3); word + 4 <= end; word += 4)
	{
		unsigned long value;

		/* (the group tag before it and the name length after it may lie
		past the element: they are only read) */
		if (word < 4 || (unsigned long)word + 8 > relocation->size)
			continue;
		value = read_word(relocation->buffer + word);
		if (value - relocation->names_low < relocation->names_high - relocation->names_low &&
			is_group_tag(read_word(relocation->buffer + word - 4)) &&
			read_word(relocation->buffer + word + 4) < MAXIMUM_NAME_LENGTH)
		{
			relocate_word(relocation, word);
		}
	}
}

static void relocate_extras(
	struct relocation *relocation,
	long offset,
	struct extra const *extras,
	short depth)
{
	struct extra const *extra;

	if (depth > MAXIMUM_DEPTH)
		return;
	for (extra = extras; extra && extra->kind != NONE; extra++)
	{
		long field = offset + extra->offset;
		unsigned long address;

		if (field < 0 || (unsigned long)field > relocation->size - sizeof(unsigned long))
			continue;
		address = read_word(relocation->buffer + field);
		if (!relocate_word(relocation, field))
			continue;
		if (extra->kind == _extra_block && extra->element_size && extra->elements && field >= 4)
		{
			long count = (long)read_word(relocation->buffer + field - 4);
			long elements;
			long index;

			if (count <= 0 || count > (long)(relocation->size / extra->element_size))
				continue;
			elements = buffer_offset(relocation, address, (unsigned long)count * extra->element_size);
			if (elements == NONE)
				continue;
			for (index = 0; index < count; index++)
				relocate_extras(relocation, elements + index * extra->element_size, extra->elements, depth + 1);
		}
	}
}

static struct extra const *definition_extras(
	struct tag_schema_definition const *definition)
{
	struct extra_definition const *extra;

	for (extra = extra_definitions; extra->name; extra++)
	{
		if (!strcmp(extra->name, definition->name))
			return extra->extras;
	}
	return NULL;
}

static void relocate_element(
	struct relocation *relocation,
	struct tag_schema_definition const *definition,
	long offset,
	short depth);

/* the fields of a definition at offset (an element, or a structure in one) */
static void relocate_fields(
	struct relocation *relocation,
	struct tag_schema_definition const *definition,
	long offset,
	short depth)
{
	struct tag_schema_field const *field;

	if (depth > MAXIMUM_DEPTH)
		return;
	relocate_extras(relocation, offset, definition_extras(definition), depth);
	for (field = definition->fields; field->type != _tag_schema_terminator; field++)
	{
		short index;

		for (index = 0; index < (field->count > 1 ? field->count : 1); index++)
		{
			long at = offset + field->offset + index * field->size;

			if (at < 0 || (unsigned long)at + 16 > relocation->size)
				continue;
			switch (field->type)
			{
			case _tag_schema_block:
			{
				struct tag_schema_definition const *element = field->definition;
				long count = (long)read_word(relocation->buffer + at);
				unsigned long address = read_word(relocation->buffer + at + BLOCK_ADDRESS);
				long elements;
				long element_index;

				if (count <= 0 || !element || element->size <= 0 ||
					count > (long)(relocation->size / element->size) ||
					!relocate_word(relocation, at + BLOCK_ADDRESS))
				{
					break;
				}
				elements = buffer_offset(relocation, address, (unsigned long)count * element->size);
				if (elements == NONE)
					break;
				for (element_index = 0; element_index < count; element_index++)
					relocate_element(relocation, element, elements + element_index * element->size, depth + 1);
				break;
			}
			case _tag_schema_data:
				relocate_word(relocation, at + DATA_ADDRESS);
				break;
			case _tag_schema_reference:
				relocate_word(relocation, at + REFERENCE_NAME);
				break;
			case _tag_schema_struct:
				if (field->definition)
					relocate_fields(relocation, field->definition, at, depth + 1);
				break;
			default:
				break;
			}
		}
	}
}

/* a root or block element: its fields, then its tag references' names (which
the fields' own pointers, moved first, are never taken for) */
static void relocate_element(
	struct relocation *relocation,
	struct tag_schema_definition const *definition,
	long offset,
	short depth)
{
	relocate_fields(relocation, definition, offset, depth);
	relocate_reference_names(relocation, offset, (unsigned long)definition->size);
}

static struct tag_schema_group const *schema_group(
	unsigned long group_tag)
{
	struct tag_schema_group const *const *list;

	for (list = tag_schema_group_lists; *list; list++)
	{
		struct tag_schema_group const *group;

		for (group = *list; group->group_tag; group++)
		{
			if (group->group_tag == group_tag)
				return group;
		}
	}
	return NULL;
}

/* the definition of a group's root: its own, or its parent's */
static struct tag_schema_definition const *group_definition(
	unsigned long group_tag)
{
	short depth;

	for (depth = 0; depth < 4; depth++)
	{
		struct tag_schema_group const *group = schema_group(group_tag);

		if (!group)
			return NULL;
		if (group->definition)
			return group->definition;
		if (!group->parent_group_tags[0] || group->parent_group_tags[0] == (unsigned long)NONE)
			return NULL;
		group_tag = group->parent_group_tags[0];
	}
	return NULL;
}

/* a tag's root, linked at address */
static void relocate_tag(
	struct relocation *relocation,
	unsigned long group_tag,
	unsigned long address)
{
	struct tag_schema_definition const *definition = group_definition(group_tag);
	struct extra_group const *extra;
	long offset;

	for (extra = extra_groups; extra->group_tag; extra++)
	{
		if (extra->group_tag == group_tag)
		{
			offset = buffer_offset(relocation, address, (unsigned long)extra->root_size);
			if (offset != NONE)
			{
				relocate_extras(relocation, offset, extra->extras, 0);
				relocate_reference_names(relocation, offset, (unsigned long)extra->root_size);
			}
		}
	}
	if (!definition || definition->size <= 0)
		return;
	offset = buffer_offset(relocation, address, (unsigned long)definition->size);
	if (offset != NONE)
		relocate_element(relocation, definition, offset, 0);
}

/* the tag names' extent, as linked, from the tag index at instances (count
instances) */
static void find_names(
	struct relocation *relocation,
	byte const *instances,
	long count,
	unsigned long delta)
{
	long index;

	relocation->names_low = 0xFFFFFFFFUL;
	relocation->names_high = 0;
	for (index = 0; index < count; index++)
	{
		unsigned long name = read_word(instances + index * TAG_INSTANCE_SIZE + TAG_INSTANCE_NAME);

		if (!name)
			continue;
		name -= delta;
		if (name < relocation->names_low)
			relocation->names_low = name;
		if (name > relocation->names_high)
			relocation->names_high = name;
	}
	if (relocation->names_low > relocation->names_high)
		relocation->names_low = relocation->names_high = 0;
	else
		relocation->names_high += MAXIMUM_NAME_LENGTH;
}

/* whether two places of size bytes overlap */
static boolean overlapping(
	unsigned long a,
	unsigned long b,
	unsigned long size)
{
	return a - b < size || b - a < size;
}

/* ---------- public code */

boolean tag_relocate_tags(
	void *tag_header,
	long tag_data_size,
	unsigned long linked_address)
{
	struct relocation relocation;
	unsigned long address = (unsigned long)tag_header;
	unsigned long instances;
	long count;
	long instances_offset;
	long index;

	if (address == linked_address)
		return TRUE;
	if (tag_data_size < TAG_HEADER_SIZE || overlapping(address, linked_address, (unsigned long)tag_data_size))
		return FALSE;

	csmemset(&relocation, 0, sizeof(relocation));
	relocation.buffer = tag_header;
	relocation.linked = linked_address;
	relocation.size = (unsigned long)tag_data_size;
	relocation.ranges[0].linked = linked_address;
	relocation.ranges[0].size = (unsigned long)tag_data_size;
	relocation.ranges[0].delta = address - linked_address;
	relocation.range_count = 1;

	instances = read_word(relocation.buffer + TAG_HEADER_INSTANCES);
	count = (long)read_word(relocation.buffer + TAG_HEADER_COUNT);
	instances_offset = count > 0 && count <= (long)(relocation.size / TAG_INSTANCE_SIZE) ?
		buffer_offset(&relocation, instances, (unsigned long)count * TAG_INSTANCE_SIZE) : NONE;
	if (instances_offset == NONE)
		count = 0;
	else
		find_names(&relocation, relocation.buffer + instances_offset, count, 0);

	/* the header */
	relocate_buffer_headers(&relocation, TAG_HEADER_VERTEX_BUFFER_COUNT, TAG_HEADER_VERTEX_BUFFERS);
	relocate_buffer_headers(&relocation, TAG_HEADER_INDEX_BUFFER_COUNT, TAG_HEADER_INDEX_BUFFERS);
	relocate_word(&relocation, TAG_HEADER_INSTANCES);
	relocate_word(&relocation, TAG_HEADER_VERTEX_BUFFERS);
	relocate_word(&relocation, TAG_HEADER_INDEX_BUFFERS);

	/* each tag: its root (from its address as linked), its name and address */
	for (index = 0; index < count; index++)
	{
		long instance = instances_offset + index * TAG_INSTANCE_SIZE;

		relocate_tag(
			&relocation,
			read_word(relocation.buffer + instance + TAG_INSTANCE_GROUP),
			read_word(relocation.buffer + instance + TAG_INSTANCE_ADDRESS));
	}
	for (index = 0; index < count; index++)
	{
		long instance = instances_offset + index * TAG_INSTANCE_SIZE;

		relocate_word(&relocation, instance + TAG_INSTANCE_NAME);
		relocate_word(&relocation, instance + TAG_INSTANCE_ADDRESS);
	}

	return TRUE;
}

boolean tag_relocate_structure_bsp(
	void *base,
	long size,
	unsigned long linked_address,
	void const *tag_header,
	long tag_data_size,
	unsigned long tags_linked_address)
{
	struct relocation relocation;
	unsigned long address = (unsigned long)base;
	unsigned long tags_delta = (unsigned long)tag_header - tags_linked_address;
	long count;
	long instances_offset;
	unsigned long root;

	if (address == linked_address && !tags_delta)
		return TRUE;
	if (size < BSP_HEADER_SIZE || tag_data_size < TAG_HEADER_SIZE ||
		(address != linked_address && overlapping(address, linked_address, (unsigned long)size)))
	{
		return FALSE;
	}

	csmemset(&relocation, 0, sizeof(relocation));
	relocation.buffer = base;
	relocation.linked = linked_address;
	relocation.size = (unsigned long)size;
	relocation.ranges[0].linked = linked_address;
	relocation.ranges[0].size = (unsigned long)size;
	relocation.ranges[0].delta = address - linked_address;
	relocation.ranges[1].linked = tags_linked_address;
	relocation.ranges[1].size = (unsigned long)tag_data_size;
	relocation.ranges[1].delta = tags_delta;
	relocation.range_count = 2;

	/* the names of the tags (moved already), as linked */
	count = (long)read_word((byte const *)tag_header + TAG_HEADER_COUNT);
	instances_offset = (long)(read_word((byte const *)tag_header + TAG_HEADER_INSTANCES) - (unsigned long)tag_header);
	if (count > 0 && count <= tag_data_size / TAG_INSTANCE_SIZE && instances_offset >= 0 &&
		instances_offset <= tag_data_size - count * TAG_INSTANCE_SIZE)
	{
		find_names(&relocation, (byte const *)tag_header + instances_offset, count, tags_delta);
	}

	root = read_word(relocation.buffer + BSP_HEADER_ROOT);
	relocate_buffer_headers(&relocation, BSP_HEADER_VERTEX_BUFFER_COUNT, BSP_HEADER_VERTEX_BUFFERS);
	relocate_buffer_headers(&relocation, BSP_HEADER_LIGHTMAP_VERTEX_BUFFER_COUNT, BSP_HEADER_LIGHTMAP_VERTEX_BUFFERS);
	relocate_tag(&relocation, 'sbsp', root);
	relocate_word(&relocation, BSP_HEADER_ROOT);
	relocate_word(&relocation, BSP_HEADER_VERTEX_BUFFERS);
	relocate_word(&relocation, BSP_HEADER_LIGHTMAP_VERTEX_BUFFERS);

	return TRUE;
}
