#include "mov-reader.h"
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <memory.h>
#include <assert.h>
#include "file-reader.h"
#include "mov-internal.h"

#define MOV_0000 MOV_TAG(0, 0, 0, 0)
#define MOV_ROOT MOV_TAG('r', 'o', 'o', 't')

#define MP4_VERSION_1 "mov1" //  ISO/IEC 14496-1:2001
#define MP4_VERSION_2 "mov2" //  ISO/IEC 14496-14:2003

struct ftyp_box_t
{
	uint32_t major_brand;
	uint32_t minor_version;
};

struct mov_parse_t
{
	uint32_t type;
	uint32_t parent;
	int(*parse)(struct mov_reader_t* mov, const struct mov_box_t* box);
};

static int mov_reader_box(struct mov_reader_t* mov, const struct mov_box_t* parent);

// 4.3 File Type Box (p17)
static int mov_read_ftyp(struct mov_reader_t* mov, const struct mov_box_t* box)
{
	mov->major_brand = file_reader_rb32(mov->fp);
	mov->minor_version = file_reader_rb32(mov->fp);
	file_reader_seek(mov->fp, box->size - 8); // skip compatible_brands
	return 0;
}

// 8.1.1 Media Data Box (p28)
static int mov_read_mdat(struct mov_reader_t* mov, const struct mov_box_t* box)
{
	return file_reader_seek(mov->fp, box->size);
}

// 8.1.2 Free Space Box (p28)
static int mov_read_free(struct mov_reader_t* mov, const struct mov_box_t* box)
{
	// Container: File or other box
	return file_reader_seek(mov->fp, box->size);
}

// 8.2.1.1 Movie Box (p29)
static int mov_read_moov(struct mov_reader_t* mov, const struct mov_box_t* box)
{
	return mov_reader_box(mov, box);
}

static int mov_trak_build(struct mov_reader_t* mov, struct mov_track_t* track)
{
	size_t i, j, k;
	uint64_t n = 0, chunk_offset;

	track->samples = malloc(sizeof(struct mov_sample_t) *  (track->stsz_count+1/*store last sample PTS/DTS*/));
	if (NULL == track->samples)
		return ENOMEM;

	// sample size
	for (n = 0; n < track->stsz_count; n++)
		track->samples[n].bytes = track->stsz[n];

	// sample offset
	track->stsc[track->stsc_count].first_chunk = track->stco_count + 1; // fill stco count
	for (i = 0, n = 0; i < track->stsc_count && track->stsc[i].first_chunk <= track->stco_count; i++)
	{
		for (j = track->stsc[i].first_chunk; j < track->stsc[i + 1].first_chunk; j++)
		{
			chunk_offset = track->stco[j-1];
			for (k = 0; k < track->stsc[i].samples_per_chunk; k++, n++)
			{
				track->samples[n].samples_description_index = track->stsc[i].sample_description_index;
				track->samples[n].offset = chunk_offset;
				chunk_offset += track->samples[n].bytes;
				assert(0 == n || track->samples[n-1].offset + track->samples[n-1].bytes <= track->samples[n].offset);
			}
		}
	}
	assert(n == track->stsz_count);

	// sample dts
	track->samples[0].dts = 0;
	track->samples[0].pts = 0;
	for (i = 0, n = 1; i < track->stts_count; i++)
	{
		for (j = 0; j < track->stts[i].sample_count; j++, n++)
		{
			track->samples[n].dts = track->samples[n - 1].dts + track->stts[i].sample_delta * 1000 / track->mdhd.timescale;
			track->samples[n].pts = track->samples[n].dts;
		}
	}
	assert(n - 1 == track->stsz_count);

	// sample cts/pts
	for (i = 0, n = 0; i < track->ctts_count; i++)
	{
		for (j = 0; j < track->ctts[i].sample_count; j++, n++)
			track->samples[n].pts += (int64_t)((int32_t)track->ctts[i].sample_delta) * 1000 / track->mdhd.timescale;
	}
	assert(0 == track->ctts_count || n == track->stsz_count);
}

// 8.3.1 Track Box (p31)
// Box Type : ��trak�� 
// Container : Movie Box(��moov��) 
// Mandatory : Yes 
// Quantity : One or more
static int mov_read_trak(struct mov_reader_t* mov, const struct mov_box_t* box)
{
	int r;
	void* p;
	size_t i;
	p = realloc(mov->tracks, sizeof(struct mov_track_t) * (mov->track_count + 1));
	if (NULL == p) return ENOMEM;
	mov->tracks = p;
	mov->track_count += 1;
	mov->track = &mov->tracks[mov->track_count - 1];
	memset(mov->track, 0, sizeof(struct mov_track_t));

	r = mov_reader_box(mov, box);
	if (0 == r)
	{
		mov_trak_build(mov, mov->track);
	}

	return r;
}

// 8.4.3 Handler Reference Box (p36)
// Box Type: ��hdlr�� 
// Container: Media Box (��mdia��) or Meta Box (��meta��) 
// Mandatory: Yes 
// Quantity: Exactly one
static int mov_read_hdlr(struct mov_reader_t* mov, const struct mov_box_t* box)
{
	file_reader_r8(mov->fp); /* version */
	file_reader_rb24(mov->fp); /* flags */
	//uint32_t pre_defined = file_reader_rb32(mov->fp);
	file_reader_seek(mov->fp, 4);
	mov->handler_type = file_reader_rb32(mov->fp);
	// const unsigned int(32)[3] reserved = 0;
	file_reader_seek(mov->fp, 12);
	// string name;
	file_reader_seek(mov->fp, box->size - 24); // String name
	return 0;
}

static int mov_read_vmhd(struct mov_reader_t* mov, const struct mov_box_t* box)
{
	file_reader_r8(mov->fp); /* version */
	file_reader_rb24(mov->fp); /* flags */
	uint16_t graphicsmode = file_reader_rb16(mov->fp);
	// template unsigned int(16)[3] opcolor = {0, 0, 0};
	file_reader_seek(mov->fp, 6);
	return 0;
}

static int mov_read_smhd(struct mov_reader_t* mov, const struct mov_box_t* box)
{
	file_reader_r8(mov->fp); /* version */
	file_reader_rb24(mov->fp); /* flags */
	uint16_t balance = file_reader_rb16(mov->fp);
	//const unsigned int(16) reserved = 0;
	file_reader_seek(mov->fp, 2);
	return 0;
}

static int mov_read_dref(struct mov_reader_t* mov, const struct mov_box_t* box)
{
	uint32_t i, entry_count;
	file_reader_r8(mov->fp); /* version */
	file_reader_rb24(mov->fp); /* flags */
	entry_count = file_reader_rb32(mov->fp);

	for (i = 0; i < entry_count; i++)
	{
		uint32_t size = file_reader_rb32(mov->fp);
		uint32_t type = file_reader_rb32(mov->fp);
		uint32_t vern = file_reader_rb32(mov->fp); /* version + flags */
		file_reader_seek(mov->fp, size-12);
	}
	return 0;
}

// 8.6.2 Sync Sample Box (p50)
static int mov_read_stss(struct mov_reader_t* mov, const struct mov_box_t* box)
{
	uint32_t i, entry_count;
	struct mov_track_t* stream;
	file_reader_r8(mov->fp); /* version */
	file_reader_rb24(mov->fp); /* flags */
	entry_count = file_reader_rb32(mov->fp);

	assert(mov->track);
	stream = mov->track;
	if (stream->stss)
	{
		assert(0);
		free(stream->stss); // duplicated STSS atom
	}
	stream->stss_count = 0;
	stream->stss = malloc(sizeof(stream->stss[0]) * entry_count);
	if (NULL == stream->stss)
		return ENOMEM;

	for (i = 0; i < entry_count; i++)
		stream->stss[i] = file_reader_rb32(mov->fp); // uint32_t sample_number

	stream->stss_count = i;
	return 0;
}

static int mov_read_default(struct mov_reader_t* mov, const struct mov_box_t* box)
{
	return mov_reader_box(mov, box);
}

static struct mov_parse_t s_mov_parse_table[] = {
	{ MOV_TAG('f', 't', 'y', 'p'), MOV_ROOT, mov_read_ftyp },
	{ MOV_TAG('m', 'd', 'a', 't'), MOV_ROOT, mov_read_mdat },
	{ MOV_TAG('f', 'r', 'e', 'e'), MOV_0000, mov_read_free },
	{ MOV_TAG('s', 'k', 'i', 'p'), MOV_0000, mov_read_free },
	{ MOV_TAG('m', 'o', 'o', 'v'), MOV_ROOT, mov_read_moov },
	{ MOV_TAG('m', 'v', 'h', 'd'), MOV_MOOV, mov_read_mvhd },
	{ MOV_TAG('t', 'r', 'a', 'k'), MOV_MOOV, mov_read_trak },
	{ MOV_TAG('t', 'k', 'h', 'd'), MOV_TRAK, mov_read_tkhd },
	{ MOV_TAG('e', 'd', 't', 's'), MOV_TRAK, mov_read_default },
	{ MOV_TAG('e', 'l', 's', 't'), MOV_EDTS, mov_read_elst },
	{ MOV_TAG('m', 'd', 'i', 'a'), MOV_TRAK, mov_read_default },
	{ MOV_TAG('m', 'd', 'h', 'd'), MOV_MDIA, mov_read_mdhd },
	{ MOV_TAG('h', 'd', 'l', 'r'), MOV_MDIA, mov_read_hdlr },
	{ MOV_TAG('m', 'i', 'n', 'f'), MOV_MDIA, mov_read_default },
	{ MOV_TAG('v', 'm', 'h', 'd'), MOV_MINF, mov_read_vmhd },
	{ MOV_TAG('s', 'm', 'h', 'd'), MOV_MINF, mov_read_smhd },
	{ MOV_TAG('d', 'i', 'n', 'f'), MOV_MINF, mov_read_default },
	{ MOV_TAG('d', 'r', 'e', 'f'), MOV_DINF, mov_read_dref },
	{ MOV_TAG('s', 't', 'b', 'l'), MOV_MINF, mov_read_default },
	{ MOV_TAG('s', 't', 's', 'd'), MOV_STBL, mov_read_stsd },
	{ MOV_TAG('s', 't', 't', 's'), MOV_STBL, mov_read_stts },
	{ MOV_TAG('c', 't', 't', 's'), MOV_STBL, mov_read_ctts },
	{ MOV_TAG('s', 't', 's', 'c'), MOV_STBL, mov_read_stsc },
	{ MOV_TAG('s', 't', 's', 'z'), MOV_STBL, mov_read_stsz },
	{ MOV_TAG('s', 't', 'z', '2'), MOV_STBL, mov_read_stz2 },
	{ MOV_TAG('s', 't', 's', 's'), MOV_STBL, mov_read_stss },
	{ MOV_TAG('s', 't', 'c', 'o'), MOV_STBL, mov_read_stco },
	{ MOV_TAG('c', 'o', '6', '4'), MOV_STBL, mov_read_stco },
	{ MOV_TAG('e', 's', 'd', 's'), MOV_STBL, mov_read_esds },
	{ 0, NULL } // last
};

void* mov_reader_create(const char* file)
{
	struct mov_reader_t* mov;
	mov = (struct mov_reader_t*)malloc(sizeof(*mov));
	if (NULL == mov)
		return NULL;

	memset(mov, 0, sizeof(*mov));
	mov->fp = file_reader_create(file);
	if (NULL == mov->fp)
	{
		mov_reader_destroy(mov);
		return NULL;
	}

	// ISO/IEC 14496-12:2012(E) 4.3.1 Definition
	// Files with no file-type box should be read as if they contained an FTYP box 
	// with Major_brand='mov1', minor_version=0, and the single compatible brand 'mov1'.
	mov->major_brand = MOV_TAG('m', 'o', 'v', '1');
	mov->minor_version = 0;

	mov->header = 0;
	return mov;
}

void mov_reader_destroy(void* p)
{
	struct mov_reader_t* mov;
	mov = (struct mov_reader_t*)p;

	if (mov->fp)
	{
		file_reader_destroy(mov->fp);
		mov->fp = NULL;
	}

	free(mov);
}

static int mov_reader_box(struct mov_reader_t* mov, const struct mov_box_t* parent)
{
	int i;
	uint64_t bytes = 0;
	struct mov_box_t box;
	int(*parse)(struct mov_reader_t* mov, const struct mov_box_t* box);

	while (bytes + 8 < parent->size 
		&& 0 == file_reader_error(mov->fp))
	{
		uint64_t n = 8;
		box.size = file_reader_rb32(mov->fp);
		box.type = file_reader_rb32(mov->fp);

		if (1 == box.size)
		{
			// unsigned int(64) largesize
			box.size = file_reader_rb64(mov->fp);
			n += 8;
		}
		else if (0 == box.size)
		{
			if (0 == box.type)
				break; // all done
			box.size = UINT64_MAX;
		}

		if (UINT64_MAX == box.size)
		{
			bytes = parent->size;
		}
		else
		{
			bytes += box.size;
			box.size -= n;
		}

		if (bytes  > parent->size)
			return -1;

		for (i = 0, parse = NULL; s_mov_parse_table[i].type && !parse; i++)
		{
			if (s_mov_parse_table[i].type == box.type)
			{
				assert(MOV_0000 == s_mov_parse_table[i].parent
					|| s_mov_parse_table[i].parent == parent->type);
				parse = s_mov_parse_table[i].parse;
			}
		}

		if (NULL == parse)
		{
			file_reader_seek(mov->fp, box.size);
		}
		else
		{
			uint64_t pos = file_reader_tell(mov->fp);
			parse(mov, &box);
			uint64_t pos2 = file_reader_tell(mov->fp);
			assert(pos2 - pos == box.size);
			file_reader_seek(mov->fp, box.size - (pos2 - pos));
		}
	}

	return 0;
}

int mov_reader_read(void* p, void* buffer, size_t bytes)
{
	size_t i;
	struct mov_box_t box;
	struct mov_reader_t* mov;
	mov = (struct mov_reader_t*)p;

	box.type = MOV_ROOT;
	box.size = UINT64_MAX;
	mov_reader_box(mov, &box);
	return 0;
}
