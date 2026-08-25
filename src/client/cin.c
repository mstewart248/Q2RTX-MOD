/*
Copyright (C) 1997-2001 Id Software, Inc.
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include "client.h"
#include "client/sound/sound.h"
#include "client/sound/vorbis.h"
#include "common/files.h"
#include "refresh/images.h"

#include <ogg/ogg.h>
#include <theora/theoradec.h>
#include <vorbis/codec.h>

typedef struct
{
    byte	*data;
    int		count;
} cblock_t;

typedef struct
{
    int     s_khz_original;
    int     s_rate;
    int     s_width;
    int     s_channels;

    int     width;
    int     height;

    // order 1 huffman stuff
    int     *hnodes1;	// [256][256][2];
    int     numhnodes1[256];

    int     h_used[512];
    int     h_count[512];

    byte    palette[768];
    bool    palette_active;

    char    file_name[MAX_QPATH];
    qhandle_t file;

    int     start_time; // cls.realtime for first cinematic frame
    int     frame_index;

    // .cin is always 14 fps; the rerelease .ogv carry their own rate (30/1)
    int     fps_num;
    int     fps_den;
} cinematics_t;

static cinematics_t cin = { 0 };

// the Theora backend lives further down the file; the cinematic lifecycle
// functions above it need these
static bool      OGV_IsActive(void);
static void      OGV_Shutdown(void);
static void      OGV_PumpAudio(void);
static void      OGV_SyncToAudio(void);
static qhandle_t OGV_ReadNextFrame(void);

/*
==================
SCR_StopCinematic
==================
*/
void SCR_StopCinematic(void)
{
    cin.start_time = 0;	// done

    S_UnqueueRawSamples();

    if (cl.image_precache[0])
    {
        R_UnregisterImage(cl.image_precache[0]);
        cl.image_precache[0] = 0;
    }

    OGV_Shutdown();

    if (cin.file)
    {
        FS_CloseFile(cin.file);
        cin.file = 0;
    }
    if (cin.hnodes1)
    {
        Z_Free(cin.hnodes1);
        cin.hnodes1 = NULL;
    }

    // switch the sample rate back to its original value if necessary
    if (cin.s_khz_original != 0)
    {
        Cvar_Set("s_khz", va("%d", cin.s_khz_original));
        cin.s_khz_original = 0;
    }
}

/*
====================
SCR_FinishCinematic

Called when either the cinematic completes, or it is aborted
====================
*/
void SCR_FinishCinematic(void)
{
    SCR_StopCinematic();

    // tell the server to advance to the next map / cinematic
    CL_ClientCommand(va("nextserver %i\n", cl.servercount));
}

//==========================================================================

/*
==================
SmallestNode1
==================
*/
int	SmallestNode1(int numhnodes)
{
    int		i;
    int		best, bestnode;

    best = 99999999;
    bestnode = -1;
    for (i = 0; i < numhnodes; i++)
    {
        if (cin.h_used[i])
            continue;
        if (!cin.h_count[i])
            continue;
        if (cin.h_count[i] < best)
        {
            best = cin.h_count[i];
            bestnode = i;
        }
    }

    if (bestnode == -1)
        return -1;

    cin.h_used[bestnode] = true;
    return bestnode;
}


/*
==================
Huff1TableInit

Reads the 64k counts table and initializes the node trees
==================
*/
void Huff1TableInit(void)
{
    int		prev;
    int		j;
    int		*node, *nodebase;
    byte	counts[256];
    int		numhnodes;

    cin.hnodes1 = Z_Malloc(256 * 256 * 2 * 4);
    memset(cin.hnodes1, 0, 256 * 256 * 2 * 4);

    for (prev = 0; prev < 256; prev++)
    {
        memset(cin.h_count, 0, sizeof(cin.h_count));
        memset(cin.h_used, 0, sizeof(cin.h_used));

        // read a row of counts
        FS_Read(counts, sizeof(counts), cin.file);
        for (j = 0; j < 256; j++)
            cin.h_count[j] = counts[j];

        // build the nodes
        numhnodes = 256;
        nodebase = cin.hnodes1 + prev * 256 * 2;

        while (numhnodes != 511)
        {
            node = nodebase + (numhnodes - 256) * 2;

            // pick two lowest counts
            node[0] = SmallestNode1(numhnodes);
            if (node[0] == -1)
                break;	// no more

            node[1] = SmallestNode1(numhnodes);
            if (node[1] == -1)
                break;

            cin.h_count[numhnodes] = cin.h_count[node[0]] + cin.h_count[node[1]];
            numhnodes++;
        }

        cin.numhnodes1[prev] = numhnodes - 1;
    }
}

/*
==================
Huff1Decompress
==================
*/
cblock_t Huff1Decompress(cblock_t in)
{
    byte		*input;
    byte		*out_p;
    int			nodenum;
    int			count;
    cblock_t	out;
    int			inbyte;
    int			*hnodes, *hnodesbase;
    //int		i;

        // get decompressed count
    count = in.data[0] + (in.data[1] << 8) + (in.data[2] << 16) + (in.data[3] << 24);
    input = in.data + 4;
    out_p = out.data = Z_Malloc(count);

    // read bits

    hnodesbase = cin.hnodes1 - 256 * 2;	// nodes 0-255 aren't stored

    hnodes = hnodesbase;
    nodenum = cin.numhnodes1[0];
    while (count)
    {
        inbyte = *input++;
        //-----------
        if (nodenum < 256)
        {
            hnodes = hnodesbase + (nodenum << 9);
            *out_p++ = nodenum;
            if (!--count)
                break;
            nodenum = cin.numhnodes1[nodenum];
        }
        nodenum = hnodes[nodenum * 2 + (inbyte & 1)];
        inbyte >>= 1;
        //-----------
        if (nodenum < 256)
        {
            hnodes = hnodesbase + (nodenum << 9);
            *out_p++ = nodenum;
            if (!--count)
                break;
            nodenum = cin.numhnodes1[nodenum];
        }
        nodenum = hnodes[nodenum * 2 + (inbyte & 1)];
        inbyte >>= 1;
        //-----------
        if (nodenum < 256)
        {
            hnodes = hnodesbase + (nodenum << 9);
            *out_p++ = nodenum;
            if (!--count)
                break;
            nodenum = cin.numhnodes1[nodenum];
        }
        nodenum = hnodes[nodenum * 2 + (inbyte & 1)];
        inbyte >>= 1;
        //-----------
        if (nodenum < 256)
        {
            hnodes = hnodesbase + (nodenum << 9);
            *out_p++ = nodenum;
            if (!--count)
                break;
            nodenum = cin.numhnodes1[nodenum];
        }
        nodenum = hnodes[nodenum * 2 + (inbyte & 1)];
        inbyte >>= 1;
        //-----------
        if (nodenum < 256)
        {
            hnodes = hnodesbase + (nodenum << 9);
            *out_p++ = nodenum;
            if (!--count)
                break;
            nodenum = cin.numhnodes1[nodenum];
        }
        nodenum = hnodes[nodenum * 2 + (inbyte & 1)];
        inbyte >>= 1;
        //-----------
        if (nodenum < 256)
        {
            hnodes = hnodesbase + (nodenum << 9);
            *out_p++ = nodenum;
            if (!--count)
                break;
            nodenum = cin.numhnodes1[nodenum];
        }
        nodenum = hnodes[nodenum * 2 + (inbyte & 1)];
        inbyte >>= 1;
        //-----------
        if (nodenum < 256)
        {
            hnodes = hnodesbase + (nodenum << 9);
            *out_p++ = nodenum;
            if (!--count)
                break;
            nodenum = cin.numhnodes1[nodenum];
        }
        nodenum = hnodes[nodenum * 2 + (inbyte & 1)];
        inbyte >>= 1;
        //-----------
        if (nodenum < 256)
        {
            hnodes = hnodesbase + (nodenum << 9);
            *out_p++ = nodenum;
            if (!--count)
                break;
            nodenum = cin.numhnodes1[nodenum];
        }
        nodenum = hnodes[nodenum * 2 + (inbyte & 1)];
        inbyte >>= 1;
    }

    if (input - in.data != in.count && input - in.data != in.count + 1)
    {
        Com_Printf("Decompression overread by %li", (input - in.data) - in.count);
    }
    out.count = out_p - out.data;

    return out;
}

extern uint32_t d_8to24table[256];

/*
==================
SCR_ReadNextFrame
==================
*/
qhandle_t SCR_ReadNextFrame(void)
{
    int		r;
    int		command;
    byte	samples[22050 / 14 * 4];
    byte	compressed[0x20000];
    int		size;
    byte	*pic;
    cblock_t	in, huf1;
    int		start, end, count;

    // read the next frame
    r = FS_Read(&command, 4, cin.file);
    if (r == 0)		// we'll give it one more chance
        r = FS_Read(&command, 4, cin.file);

    if (r != 4)
        return 0;
    command = LittleLong(command);
    if (command == 2)
        return 0;	// last frame marker

    if (command == 1)
    {	// read palette
        FS_Read(cin.palette, sizeof(cin.palette), cin.file);
        cin.palette_active = true;
    }

    // decompress the next frame
    FS_Read(&size, 4, cin.file);
    size = LittleLong(size);
    if (size > sizeof(compressed) || size < 1)
        Com_Error(ERR_DROP, "Bad compressed frame size");
    FS_Read(compressed, size, cin.file);

    // read sound
    start = cin.frame_index*cin.s_rate / 14;
    end = (cin.frame_index + 1)*cin.s_rate / 14;
    count = end - start;

    FS_Read(samples, count*cin.s_width*cin.s_channels, cin.file);

    S_RawSamples(count, cin.s_rate, cin.s_width, cin.s_channels, samples, 1.0f);

    in.data = compressed;
    in.count = size;

    huf1 = Huff1Decompress(in);

    pic = huf1.data;

    uint32_t* rgba = Z_Malloc(cin.width * cin.height * 4);
    uint32_t* wptr = rgba;

    for (int y = 0; y < cin.height; y++)
    {
        if (cin.palette_active)
        {
            for (int x = 0; x < cin.width; x++)
            {
                byte* src = cin.palette + (*pic) * 3;
                *wptr = MakeColor(src[0], src[1], src[2], 255);
                pic++;
                wptr++;
            }
        }
        else
        {
            for (int x = 0; x < cin.width; x++)
            {
                *wptr = d_8to24table[*pic];
                pic++;
                wptr++;
            }
        }
    }

    Z_Free(huf1.data);

    cin.frame_index++;

    const char* image_name = va("%s[%d]", cin.file_name, cin.frame_index);
    return R_RegisterRawImage(image_name, cin.width, cin.height, (byte*)rgba, IT_SPRITE, IF_SRGB);
}


/*
==================
SCR_RunCinematic

==================
*/
void SCR_RunCinematic(void)
{
    int		frame;

    if (cin.start_time <= 0)
        return;

    if (cin.frame_index == -1)
        return; // static image

    // frame duration in milliseconds: 1000 * den / num
    const int frame_ms = 1000 * cin.fps_den / cin.fps_num;

    if (cls.key_dest != KEY_GAME)
    {
        // pause if menu or console is up
        cin.start_time = cls.realtime - cin.frame_index * frame_ms;

        S_UnqueueRawSamples();

        return;
    }

    // keep the audio queue fed regardless of whether a new video frame is due,
    // otherwise the sound starves between frames
    if (OGV_IsActive())
    {
        OGV_PumpAudio();
        OGV_SyncToAudio();
    }

    // The audio device is heard later than we queue it, so the picture can run
    // slightly ahead of the sound. How much depends on the sound hardware, so
    // it is a tunable rather than a constant: cl_hd_cinematics_delay holds the
    // video back by that many milliseconds. Only applies to .ogv playback.
    int av_delay = 0;
    if (OGV_IsActive())
    {
        av_delay = (int)Cvar_VariableValue("cl_hd_cinematics_delay");
        av_delay = av_delay < 0 ? 0 : (av_delay > 2000 ? 2000 : av_delay);
    }

    frame = (int)((int64_t)(cls.realtime - cin.start_time - av_delay) * cin.fps_num
                  / (1000 * cin.fps_den));
    if (frame <= cin.frame_index)
        return;
    if (frame > cin.frame_index + 1)
    {
        // Com_Printf("Dropped frame: %i > %i\n", frame, cin.frame_index + 1);
        cin.start_time = cls.realtime - cin.frame_index * frame_ms;
    }

    R_UnregisterImage(cl.image_precache[0]);
    cl.image_precache[0] = OGV_IsActive() ? OGV_ReadNextFrame() : SCR_ReadNextFrame();

    if (!cl.image_precache[0])
    {
        SCR_FinishCinematic();
        cin.start_time = 1;	// hack to get the black screen behind loading
        SCR_BeginLoadingPlaque();
        cin.start_time = 0;
        return;
    }
}

/*
===============================================================================

OGG THEORA + VORBIS  -  the rerelease's remastered cutscenes

1920x1080 @ 30fps Theora video with 48kHz stereo Vorbis audio. These are
streamed with plain stdio rather than through the FS: they are 100-320 MB each,
they are never inside a pak, and libogg wants to own its own read buffer.

===============================================================================
*/

typedef struct
{
    bool                active;
    FILE                *file;

    ogg_sync_state      sync;
    ogg_stream_state    v_stream;       // theora
    ogg_stream_state    a_stream;       // vorbis
    bool                have_video;
    bool                have_audio;

    th_info             ti;
    th_comment          tc;
    th_dec_ctx          *td;

    vorbis_info         vi;
    vorbis_comment      vc;
    vorbis_dsp_state    vd;
    vorbis_block        vb;
    bool                audio_ready;

    int                 width;          // cropped picture, not the coded frame
    int                 height;
    int                 pic_x;
    int                 pic_y;

    // kept for A/V diagnostics: compare frames*1000/fps against
    // audio_samples*1000/rate to see the two clocks drift apart
    int                 audio_samples;
    int                 start_realtime; // cin.start_time is cleared before shutdown

    bool                eof;
} ogv_t;

static ogv_t ogv;

#define OGV_READ_CHUNK      65536
#define OGV_AUDIO_CHUNK     1024    // samples per S_RawSamples call
#define OGV_MAX_CHANNELS    8

static int16_t ogv_audio_buf[OGV_AUDIO_CHUNK * OGV_MAX_CHANNELS];

static bool OGV_IsActive(void)
{
    return ogv.active;
}

// clamp() in shared.h assigns to its first argument, so it cannot take the
// const results of the colour conversion
static inline int OGV_Clip255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static int OGV_BufferData(void)
{
    char *buf = ogg_sync_buffer(&ogv.sync, OGV_READ_CHUNK);
    int  n;

    if (!buf)
        return 0;

    n = (int)fread(buf, 1, OGV_READ_CHUNK, ogv.file);
    ogg_sync_wrote(&ogv.sync, n);
    return n;
}

static void OGV_RoutePage(ogg_page *og)
{
    int serial = ogg_page_serialno(og);

    if (ogv.have_video && serial == ogv.v_stream.serialno)
        ogg_stream_pagein(&ogv.v_stream, og);
    else if (ogv.have_audio && serial == ogv.a_stream.serialno)
        ogg_stream_pagein(&ogv.a_stream, og);
}

// pulls exactly one more page into its stream. false means end of file.
static bool OGV_PumpPage(void)
{
    ogg_page og;

    while (ogg_sync_pageout(&ogv.sync, &og) <= 0)
    {
        if (OGV_BufferData() == 0)
        {
            ogv.eof = true;
            return false;
        }
    }

    OGV_RoutePage(&og);
    return true;
}

static void OGV_Shutdown(void)
{
    if (!ogv.active)
        return;

    if (ogv.start_realtime && cin.frame_index)
    {
        int elapsed = cls.realtime - ogv.start_realtime;
        int starts = 0, min_queued = 0;

        S_GetRawStreamStats(&starts, &min_queued, false);

        // starts > 1 means the audio ran dry: every restart inserts a gap and
        // leaves the sound permanently behind the picture
        if (elapsed > 0)
            Com_DPrintf("OGV: %d frames in %d ms = %.1f fps (target %.1f), "
                       "%d audio samples, %d audio starts, min queue %d\n",
                       cin.frame_index, elapsed,
                       cin.frame_index * 1000.0 / elapsed,
                       (double)cin.fps_num / cin.fps_den,
                       ogv.audio_samples, starts, min_queued);
    }

    if (ogv.td)
        th_decode_free(ogv.td);

    if (ogv.have_video)
    {
        ogg_stream_clear(&ogv.v_stream);
        th_info_clear(&ogv.ti);
        th_comment_clear(&ogv.tc);
    }

    if (ogv.have_audio)
    {
        if (ogv.audio_ready)
        {
            vorbis_block_clear(&ogv.vb);
            vorbis_dsp_clear(&ogv.vd);
        }
        ogg_stream_clear(&ogv.a_stream);
        vorbis_comment_clear(&ogv.vc);
        vorbis_info_clear(&ogv.vi);
    }

    ogg_sync_clear(&ogv.sync);

    if (ogv.file)
        fclose(ogv.file);

    memset(&ogv, 0, sizeof(ogv));
}

/*
==================
OGV_Open

Demuxes the BOS pages to find the two streams, reads the three header packets
each codec needs, and allocates the decoders. Returns false and cleans up after
itself on any malformed file, so the caller can fall back to the .cin.
==================
*/
static bool OGV_Open(const char *path)
{
    th_setup_info   *ts = NULL;
    ogg_page        og;
    ogg_packet      op;
    int             video_hdrs = 0, audio_hdrs = 0;
    bool            bos_done = false;

    memset(&ogv, 0, sizeof(ogv));

    ogv.file = fopen(path, "rb");
    if (!ogv.file)
    {
        Com_WPrintf("Couldn't open remastered cinematic \"%s\".\n", path);
        return false;
    }

    ogv.active = true;
    ogv.start_realtime = cls.realtime;
    S_GetRawStreamStats(NULL, NULL, true);
    ogg_sync_init(&ogv.sync);
    th_info_init(&ogv.ti);
    th_comment_init(&ogv.tc);
    vorbis_info_init(&ogv.vi);
    vorbis_comment_init(&ogv.vc);

    // the BOS pages, one per stream, announce what is in the file
    while (!bos_done)
    {
        if (OGV_BufferData() == 0)
            break;

        while (ogg_sync_pageout(&ogv.sync, &og) > 0)
        {
            ogg_stream_state test;

            if (!ogg_page_bos(&og))
            {
                // first non-BOS page: the announcements are over, and this page
                // is already real stream data
                OGV_RoutePage(&og);
                bos_done = true;
                break;
            }

            ogg_stream_init(&test, ogg_page_serialno(&og));
            ogg_stream_pagein(&test, &og);

            if (ogg_stream_packetout(&test, &op) != 1)
            {
                ogg_stream_clear(&test);
                continue;
            }

            if (!ogv.have_video && th_decode_headerin(&ogv.ti, &ogv.tc, &ts, &op) >= 0)
            {
                ogv.v_stream = test;
                ogv.have_video = true;
                video_hdrs = 1;
            }
            else if (!ogv.have_audio && vorbis_synthesis_headerin(&ogv.vi, &ogv.vc, &op) >= 0)
            {
                ogv.a_stream = test;
                ogv.have_audio = true;
                audio_hdrs = 1;
            }
            else
            {
                // some other stream (skeleton, subtitles); ignore it
                ogg_stream_clear(&test);
            }
        }
    }

    if (!ogv.have_video)
    {
        Com_WPrintf("\"%s\" has no Theora stream.\n", path);
        goto fail;
    }

    // the remaining two header packets for each codec
    while ((video_hdrs < 3) || (ogv.have_audio && audio_hdrs < 3))
    {
        while (video_hdrs < 3 && ogg_stream_packetout(&ogv.v_stream, &op) > 0)
        {
            if (th_decode_headerin(&ogv.ti, &ogv.tc, &ts, &op) < 0)
            {
                Com_WPrintf("\"%s\" has a corrupt Theora header.\n", path);
                goto fail;
            }
            video_hdrs++;
        }

        while (ogv.have_audio && audio_hdrs < 3 &&
               ogg_stream_packetout(&ogv.a_stream, &op) > 0)
        {
            if (vorbis_synthesis_headerin(&ogv.vi, &ogv.vc, &op) < 0)
            {
                // bad audio is survivable - play it silent rather than not at all
                Com_WPrintf("\"%s\" has a corrupt Vorbis header; playing silent.\n", path);
                ogv.have_audio = false;
                break;
            }
            audio_hdrs++;
        }

        if ((video_hdrs < 3) || (ogv.have_audio && audio_hdrs < 3))
        {
            if (!OGV_PumpPage())
            {
                Com_WPrintf("\"%s\" ended inside its headers.\n", path);
                goto fail;
            }
        }
    }

    ogv.td = th_decode_alloc(&ogv.ti, ts);
    th_setup_free(ts);
    ts = NULL;

    if (!ogv.td)
    {
        Com_WPrintf("Couldn't allocate a Theora decoder for \"%s\".\n", path);
        goto fail;
    }

    // pic_y is exposed from the TOP here: libtheora already inverted the
    // stream's bottom-relative value (see decinfo.c). Crop from the top.
    ogv.width  = ogv.ti.pic_width;
    ogv.height = ogv.ti.pic_height;
    ogv.pic_x  = ogv.ti.pic_x;
    ogv.pic_y  = ogv.ti.pic_y;

    if (ogv.have_audio)
    {
        vorbis_synthesis_init(&ogv.vd, &ogv.vi);
        vorbis_block_init(&ogv.vd, &ogv.vb);
        ogv.audio_ready = true;
    }

    Com_DPrintf("Theora %dx%d @ %u/%u fps, audio %ld Hz %d ch\n",
                ogv.width, ogv.height,
                ogv.ti.fps_numerator, ogv.ti.fps_denominator,
                ogv.have_audio ? ogv.vi.rate : 0,
                ogv.have_audio ? ogv.vi.channels : 0);

    return true;

fail:
    if (ts)
        th_setup_free(ts);
    OGV_Shutdown();
    return false;
}

/*
==================
OGV_PumpAudio

Feeds decoded Vorbis into the raw sample queue, stopping once the queue is deep
enough. Same backpressure test the music streamer uses (see sound/ogg.c) - it
is what keeps this from decoding the whole track in one frame.
==================
*/
static void OGV_PumpAudio(void)
{
    ogg_packet  op;
    float       **pcm;
    int         samples;

    if (!ogv.have_audio || !ogv.audio_ready)
        return;

    const int ch = min(ogv.vi.channels, OGV_MAX_CHANNELS);

    while (S_NeedRawSamples())
    {
        // drain whatever the decoder is already holding
        while ((samples = vorbis_synthesis_pcmout(&ogv.vd, &pcm)) > 0)
        {
            const int count = min(samples, OGV_AUDIO_CHUNK);

            for (int i = 0; i < count; i++)
            {
                for (int c = 0; c < ch; c++)
                {
                    int v = (int)(pcm[c][i] * 32767.0f);

                    ogv_audio_buf[i * ch + c] =
                        (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
                }
            }

            // width is 2 (16-bit), which is not the same thing as the channel
            // count - see the note in sound/ogg.c's call
            S_RawSamples(count, ogv.vi.rate, 2, ch, (byte *)ogv_audio_buf, 1.0f);
            vorbis_synthesis_read(&ogv.vd, count);
            ogv.audio_samples += count;

            if (!S_NeedRawSamples())
                return;
        }

        // decoder is empty; hand it another packet
        if (ogg_stream_packetout(&ogv.a_stream, &op) > 0)
        {
            if (vorbis_synthesis(&ogv.vb, &op) == 0)
                vorbis_synthesis_blockin(&ogv.vd, &ogv.vb);
            continue;
        }

        if (!OGV_PumpPage())
            return;     // end of file
    }
}

/*
==================
OGV_SyncToAudio

Video is paced off cls.realtime, but the audio device is the real clock. What
is being heard right now is everything submitted minus whatever is still
queued, so comparing that against the video's own timeline exposes both a
constant offset (device latency, which is what makes the video look slightly
ahead) and any slow drift between the two clocks.

Rather than snap - which would visibly jerk - this slews cin.start_time a
fraction of the error each frame, so the correction is invisible.
==================
*/
static void OGV_SyncToAudio(void)
{
    if (!ogv.have_audio || !ogv.audio_samples || ogv.vi.rate <= 0)
        return;     // nothing to sync to; cls.realtime stands

    double heard = (double)ogv.audio_samples / (double)ogv.vi.rate
                   - S_GetRawStreamLatency();

    if (heard <= 0.0)
        return;     // still priming, nothing audible has played yet

    const int implied_start = cls.realtime - (int)(heard * 1000.0);
    const int error = implied_start - cin.start_time;

    // sub-frame noise is not worth chasing, and a huge error means something
    // exceptional happened (a stall, a skip, the queue being flushed) - a slew
    // is the wrong response to that
    if (abs(error) < 8 || abs(error) > 2000)
        return;

    cin.start_time += error / 8;
}

/*
==================
OGV_ReadNextFrame

Decodes one Theora frame, converts the cropped picture from YCbCr to RGBA and
hands it back as an image handle, matching what the .cin path returns. 0 means
the video is over.
==================
*/
static qhandle_t OGV_ReadNextFrame(void)
{
    ogg_packet      op;
    ogg_int64_t     granulepos = -1;
    th_ycbcr_buffer ycbcr;

    // find the next video packet, reading more of the file as needed
    for (;;)
    {
        if (ogg_stream_packetout(&ogv.v_stream, &op) > 0)
        {
            if (th_decode_packetin(ogv.td, &op, &granulepos) == 0)
                break;
            continue;   // dropped/duplicate packet, keep looking
        }

        if (!OGV_PumpPage())
            return 0;
    }

    if (th_decode_ycbcr_out(ogv.td, ycbcr) != 0)
        return 0;

    // 4:2:0 and 4:2:2 halve chroma horizontally; only 4:2:0 halves it
    // vertically. 4:4:4 does neither.
    const int cshift_x = (ogv.ti.pixel_fmt == TH_PF_444) ? 0 : 1;
    const int cshift_y = (ogv.ti.pixel_fmt == TH_PF_420) ? 1 : 0;

    const int w = ogv.width;
    const int h = ogv.height;

    uint32_t *rgba = Z_Malloc(w * h * 4);

    for (int y = 0; y < h; y++)
    {
        const int sy = y + ogv.pic_y;
        const byte *Y = ycbcr[0].data + (ptrdiff_t)sy * ycbcr[0].stride;
        const byte *U = ycbcr[1].data + (ptrdiff_t)(sy >> cshift_y) * ycbcr[1].stride;
        const byte *V = ycbcr[2].data + (ptrdiff_t)(sy >> cshift_y) * ycbcr[2].stride;
        uint32_t *out = rgba + (ptrdiff_t)y * w;

        for (int x = 0; x < w; x++)
        {
            const int sx = x + ogv.pic_x;
            // BT.601 studio swing, the Theora default
            const int yy = Y[sx] - 16;
            const int uu = U[sx >> cshift_x] - 128;
            const int vv = V[sx >> cshift_x] - 128;

            const int r = (298 * yy + 409 * vv + 128) >> 8;
            const int g = (298 * yy - 100 * uu - 208 * vv + 128) >> 8;
            const int b = (298 * yy + 516 * uu + 128) >> 8;

            out[x] = MakeColor(OGV_Clip255(r), OGV_Clip255(g), OGV_Clip255(b), 255);
        }
    }

    cin.frame_index++;

    const char *image_name = va("%s[%d]", cin.file_name, cin.frame_index);
    return R_RegisterRawImage(image_name, w, h, (byte *)rgba, IT_SPRITE, IF_SRGB);
}

// the game directory the remastered cutscenes are shipped in
#define REMASTER_GAME   "rerelease"

/*
==================
SCR_ResolveRemasteredCinematic

The rerelease ships its cutscenes as 1080p30 Ogg Theora + Vorbis in
video/<name>.ogv, under exactly the same base names as the classic .cin files.
At 2.7 GB the set never lives in a pak and is normally a junction to the retail
install, so these are opened by absolute path instead of through the search
paths - FS_OpenFile is search path scoped and guards traversal.

Looks in the active game directory first, then in the port's own rerelease/
directory, so the remastered videos also play in the classic campaign where
rerelease/ is not on the search path at all.

Returns false if cl_hd_cinematics is off or no .ogv exists, and the caller then
falls back to the original .cin. That is also what makes idlog/logo work with
no special casing: the rerelease has no .ogv for either.
==================
*/
static bool SCR_ResolveRemasteredCinematic(const char *name, char *path, size_t size)
{
    char        base[MAX_QPATH];
    const char  *game;

    if (!Cvar_VariableValue("cl_hd_cinematics"))
        return false;

    if (COM_StripExtension(base, name, sizeof(base)) >= sizeof(base))
        return false;

    game = fs_game->string[0] ? fs_game->string : BASEGAME;

    if (Q_snprintf(path, size, "%s/%s/video/%s.ogv",
                   sys_basedir->string, game, base) < size
        && os_access(path, F_OK) == 0)
        return true;

    // let the classic campaign use the remastered videos too
    if (strcmp(game, REMASTER_GAME)
        && Q_snprintf(path, size, "%s/"REMASTER_GAME"/video/%s.ogv",
                      sys_basedir->string, base) < size
        && os_access(path, F_OK) == 0)
        return true;

    return false;
}

/*
==================
SCR_PlayCinematic

==================
*/
void SCR_PlayCinematic(const char *name)
{
    int		width, height;
    int		old_khz;

    // make sure CD isn't playing music
    OGG_Stop();

    cin.s_khz_original = 0;

    cin.frame_index = 0;
    cin.start_time = 0;

    if (!COM_CompareExtension(name, ".pcx"))
    {
        cl.image_precache[0] = R_RegisterPic2(name);
        if (!cl.image_precache[0]) {
            SCR_FinishCinematic();
            return;
        }
    }
    else if (!COM_CompareExtension(name, ".cin"))
    {
        if (!Cvar_VariableValue("cl_cinematics"))
        {
            SCR_FinishCinematic();
            return;
        }

        // prefer the rerelease's remastered .ogv, and fall back to the
        // original .cin if there isn't one or it won't open
        char ogv_path[MAX_OSPATH];
        if (SCR_ResolveRemasteredCinematic(name, ogv_path, sizeof(ogv_path))
            && OGV_Open(ogv_path))
        {
            Q_strlcpy(cin.file_name, name, sizeof(cin.file_name));

            cin.width  = ogv.width;
            cin.height = ogv.height;
            cin.fps_num = ogv.ti.fps_numerator;
            cin.fps_den = ogv.ti.fps_denominator;

            if (cin.fps_num <= 0 || cin.fps_den <= 0)
            {
                // a zero rate would divide by zero in SCR_RunCinematic
                Com_WPrintf("\"%s\" declares a %d/%d frame rate; assuming 30.\n",
                            name, cin.fps_num, cin.fps_den);
                cin.fps_num = 30;
                cin.fps_den = 1;
            }

            cin.frame_index = 0;
            cl.image_precache[0] = OGV_ReadNextFrame();

            if (cl.image_precache[0])
            {
                OGV_PumpAudio();
                cin.start_time = cls.realtime;
                goto started;
            }

            // decoded nothing: give up on the .ogv and use the .cin
            Com_WPrintf("\"%s\" decoded no frames; using the original.\n", ogv_path);
            OGV_Shutdown();
            cin.frame_index = 0;
        }

        cin.fps_num = 14;   // .cin is always 14 fps
        cin.fps_den = 1;

        Q_snprintf(cin.file_name, sizeof(cin.file_name), "video/%s", name);

        FS_OpenFile(cin.file_name, &cin.file, FS_MODE_READ);
        if (!cin.file)
        {
            Com_WPrintf("Cinematic \"%s\" not found. Skipping.\n", name);
            SCR_FinishCinematic();
            return;
        }

        FS_Read(&width, 4, cin.file);
        FS_Read(&height, 4, cin.file);
        cin.width = LittleLong(width);
        cin.height = LittleLong(height);

        FS_Read(&cin.s_rate, 4, cin.file);
        cin.s_rate = LittleLong(cin.s_rate);
        FS_Read(&cin.s_width, 4, cin.file);
        cin.s_width = LittleLong(cin.s_width);
        FS_Read(&cin.s_channels, 4, cin.file);
        cin.s_channels = LittleLong(cin.s_channels);

        Huff1TableInit();

        cin.palette_active = false;

        // switch to 22 khz sound if necessary
        old_khz = Cvar_VariableValue("s_khz");
        if (old_khz != cin.s_rate / 1000 && s_started == SS_DMA)
        {
            cin.s_khz_original = old_khz;
            Cvar_Set("s_khz", va("%d", cin.s_rate / 1000));
        }

        cin.frame_index = 0;
        cl.image_precache[0] = SCR_ReadNextFrame();
        cin.start_time = cls.realtime;
    }
    else
    {
        SCR_FinishCinematic();
        return;
    }

started:
    cls.state = ca_cinematic;

    SCR_EndLoadingPlaque();     // get rid of loading plaque
    Con_Close(false);          // get rid of connection screen
}
