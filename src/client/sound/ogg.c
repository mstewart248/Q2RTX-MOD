/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307,
 * USA.
 *
 * =======================================================================
 *
 * This file implements an interface to libvorbis for decoding
 * OGG/Vorbis files. Strongly spoken this file isn't part of the sound
 * system but part of the main client. It justs converts Vorbis streams
 * into normal, raw Wave stream which are injected into the backends as 
 * if they were normal "raw" samples. At this moment only background
 * music playback and in theory .cin movie file playback is supported.
 *
 * =======================================================================
 */

// This file has been adapted from Yamagi Quake 2: 
// https://github.com/yquake2/yquake2/blob/master/src/client/sound/ogg.c

#ifndef _WIN32
#include <sys/time.h>
#endif

#include <errno.h>

#include "shared/shared.h"
#include "sound.h"
#include "client/sound/vorbis.h"

#if defined(__GNUC__)
// Warnings produced by std_vorbis
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wunused-value"
#endif

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

static cvar_t *ogg_shuffle;        /* Shuffle playback */
static cvar_t *ogg_ignoretrack0;  /* Toggle track 0 playing */
static cvar_t *ogg_volume;        /* Music volume. */
static cvar_t* ogg_enable;        /* Music enable flag to toggle from the menu. */
static int ogg_curfile;           /* Index of currently played file. */
static int ogg_numbufs;           /* Number of buffers for OpenAL */
static int ogg_numsamples;        /* Number of sambles read from the current file */
static ogg_status_t ogg_status;   /* Status indicator. */
static stb_vorbis *ogg_file;      /* Ogg Vorbis file. */
static bool ogg_started;      /* Initialization flag. */

/* The rerelease numbers its music up into the 70s - rerelease/music ships
   track64..track79 - and the maps ask for tracks 64-77. At the old limit of 32
   those files were never even looked for, so every one of those requests found
   nothing. 128 covers them with room to spare; the array is just pointers. */
enum { MAX_NUM_OGGTRACKS = 128 };
static char* ogg_tracks[MAX_NUM_OGGTRACKS];
static int ogg_maxfileindex;

/* Every track number that actually has a file, packed with no holes, so a
   request for a track we do not have can be folded onto one we do. The
   rerelease maps ask for tracks 64-77 (67 alone is used by 11 maps) while
   baseq2/music only ships 02-21, and even rerelease/music has nothing between
   21 and 64 - so both the out-of-range and the sparse-hole cases are real. */
static int ogg_tracklist[MAX_NUM_OGGTRACKS];
static int ogg_numtracks;
static int ogg_firsttrack;


enum GameType {
	other, // incl. baseq2
	xatrix,
	rogue
};

struct {
	bool saved;
	int curfile;        // OGG_Q1_MUSIC: q1_music.path, which a stop keeps
	int numsamples;
} ogg_saved_state;

/*
 * QUAKE 1 MUSIC
 *
 * A Quake 1 map's worldspawn "sounds" is a CD track number like Quake II's,
 * but of its own soundtrack. The rerelease keeps each episode's tracks loose
 * in <basedir>/id1/<episode>/music/trackNN.ogg - id1 2-11, hipnotic and rogue
 * 2-9, mg3 2-13 - and an episode with none of its own (dopa, mg1, ctf) plays
 * id1's, as the rerelease does; so does a track number an episode's folder
 * lacks. Not ogg_tracks[]: that is Quake II's list for the current gamedir,
 * and a q1/ map plays inside baseq2 or rerelease, whose track 6 is not Quake's.
 * The client says which episode (precache.c, CL_PlayMapMusic, from the map's
 * name - files.c puts hipnotic's maps under maps/q1/hipnotic/). The track
 * loops, as Quake's CD music did; ogg_curfile is OGG_Q1_MUSIC.
 */
#define OGG_Q1_MUSIC    -3

static struct {
	char    path[MAX_OSPATH];
	int     track;
} q1_music;

// --------

/*
 * The GOG version of Quake2 has the music tracks in music/TrackXX.ogg
 * That music/ dir is next to baseq2/ (not in it) and contains Track02.ogg to Track21.ogg
 * There
 * - Track02 to Track11 correspond to Quake2 (baseq2) CD tracks 2-11
 * - Track12 to Track21 correspond to the Ground Zero (rogue) addon's CD tracks 2-11
 * - The "The Reckoning" (xatrix) addon also had 11 tracks, that were a mix of the ones
 *   from the main game (baseq2) and the rogue addon.
 *   See below how the CD track is mapped to GOG track numbers
 */
static int getMappedGOGtrack(int track, enum GameType gameType)
{
	if(track <= 0)
		return 0;

	if(track == 1)
		return 0; // 1 is illegal (=> data track on CD), 0 means "no track"

	if(gameType == other)
		return track;
	if(gameType == rogue)
		return track + 10;

	// apparently it's xatrix => map the track to the corresponding TrackXX.ogg from GOG
	switch(track)
	{
		case  2: return 9;  // baseq2 9
		case  3: return 13; // rogue  3
		case  4: return 14; // rogue  4
		case  5: return 7;  // baseq2 6
		case  6: return 16; // rogue  6
		case  7: return 2;  // baseq2 2
		case  8: return 15; // rogue  5
		case  9: return 3;  // baseq2 3
		case 10: return 4;  // baseq2 4
		case 11: return 18; // rogue  8
		default:
			return track;
	}
}

/*
 * Load list of Ogg Vorbis files in "music/".
 */
static void
OGG_ScanTrackList(void)
{
	for (int i=0; i<MAX_NUM_OGGTRACKS; ++i)
	{
		if (ogg_tracks[i] != NULL)
		{
			free(ogg_tracks[i]);
			ogg_tracks[i] = NULL;
		}
	}

	ogg_maxfileindex = 0;

	const char* potMusicDirs[5] = {0};
	char fullMusicDir[MAX_QPATH] = {0};
	cvar_t* gameCvar = Cvar_Get("game", "", CVAR_LATCH | CVAR_SERVERINFO);

	Q_snprintf(fullMusicDir, sizeof(fullMusicDir), "%s/" BASEGAME "/music/", sys_basedir->string);

	potMusicDirs[0] = "music/"; // $mod/music/
	// The 2023 remaster keeps its soundtrack one level down, in
	// rerelease/baseq2/music/, so a verbatim copy of its rerelease folder
	// has no music/ at the top at all and used to come up silent. Second in
	// line, so an extracted rerelease/music/ still wins.
	potMusicDirs[1] = BASEGAME "/music/"; // <gamedir>/baseq2/music/ (rerelease layout)
	potMusicDirs[2] = "../music/"; // global music dir (GOG)
	potMusicDirs[3] = "../" BASEGAME "/music/"; // baseq2/music/
	potMusicDirs[4] = fullMusicDir; // e.g. "/usr/share/games/xatrix/music"

	enum GameType gameType = other;

	if (strcmp("xatrix", gameCvar->string) == 0)
	{
		gameType = xatrix;
	}
	else if (strcmp("rogue", gameCvar->string) == 0)
	{
		gameType = rogue;
	}

	for (int potMusicDirIdx = 0; potMusicDirIdx < sizeof(potMusicDirs)/sizeof(potMusicDirs[0]); ++potMusicDirIdx)
	{
		const char* musicDir = potMusicDirs[potMusicDirIdx];

		if (musicDir == NULL)
		{
			break;
		}

		char fullMusicPath[MAX_OSPATH] = {0};
		if (strcmp(musicDir, fullMusicDir) == 0) {
			Q_snprintf(fullMusicPath, MAX_OSPATH, "%s", musicDir);
		}
		else
		{
			Q_snprintf(fullMusicPath, MAX_OSPATH, "%s/%s", fs_gamedir, musicDir);
		}

		if(!Sys_IsDir(fullMusicPath))
		{
			continue;
		}

		char testFileName[MAX_OSPATH];
		char testFileName2[MAX_OSPATH];

		// the simple case (like before: $mod/music/02.ogg - 11.ogg or whatever)
		Q_snprintf(testFileName, MAX_OSPATH, "%s02.ogg", fullMusicPath);

		if(Sys_IsFile(testFileName))
		{
			ogg_tracks[2] = strdup(testFileName);

			for(int i=3; i<MAX_NUM_OGGTRACKS; ++i)
			{
				Q_snprintf(testFileName, MAX_OSPATH, "%s%02i.ogg", fullMusicPath, i);

				if(Sys_IsFile(testFileName))
				{
					ogg_tracks[i] = strdup(testFileName);
					ogg_maxfileindex = i;
				}
			}

			return;
		}

		// the GOG case: music/Track02.ogg to Track21.ogg
		int gogTrack = getMappedGOGtrack(8, gameType);

		Q_snprintf(testFileName, MAX_OSPATH, "%sTrack%02i.ogg", fullMusicPath, gogTrack); // uppercase T
		Q_snprintf(testFileName2, MAX_OSPATH, "%strack%02i.ogg", fullMusicPath, gogTrack); // lowercase t

		if(Sys_IsFile(testFileName) || Sys_IsFile(testFileName2))
		{
			for(int i=2; i<MAX_NUM_OGGTRACKS; ++i)
			{
				int gogTrack = getMappedGOGtrack(i, gameType);

				Q_snprintf(testFileName, MAX_OSPATH, "%sTrack%02i.ogg", fullMusicPath, gogTrack); // uppercase T
				Q_snprintf(testFileName2, MAX_OSPATH, "%strack%02i.ogg", fullMusicPath, gogTrack); // lowercase t

				if(Sys_IsFile(testFileName))
				{
					ogg_tracks[i] = strdup(testFileName);
					ogg_maxfileindex = i;
				}
				else if (Sys_IsFile(testFileName2))
				{
					ogg_tracks[i] = strdup(testFileName2);
					ogg_maxfileindex = i;
				}
			}

			return;
		}
	}

	// if tracks have been found above, we would've returned there
	Com_Printf("No Ogg Vorbis music tracks have been found, so there will be no music.\n");
}

void
OGG_InitTrackList(void)
{
	OGG_ScanTrackList();

	/* pack the tracks we found into a hole-free list for OGG_WrapTrack() */
	ogg_numtracks = 0;
	ogg_firsttrack = 0;

	for (int i = 0; i < MAX_NUM_OGGTRACKS; ++i)
	{
		if (ogg_tracks[i] != NULL)
		{
			if (ogg_numtracks == 0)
			{
				ogg_firsttrack = i;
			}

			ogg_tracklist[ogg_numtracks++] = i;
		}
	}

	if (ogg_numtracks > 0)
	{
		Com_Printf("Found %d Ogg Vorbis music tracks (%d..%d).\n", ogg_numtracks,
			ogg_firsttrack, ogg_tracklist[ogg_numtracks - 1]);
	}
}

/*
 * Rebuild the track list because the GAME DIRECTORY changed.
 *
 * OGG_ScanTrackList() resolves every path against fs_gamedir, but the list was
 * only ever built once, from S_Init(). That is too early: `q2rtx +game
 * rerelease` - what rerelease.bat runs - boots in baseq2, scans baseq2/music,
 * and only THEN restarts the filesystem into rerelease. So the rerelease played
 * baseq2's twenty tracks for the whole session: every map asking for 2..21 got
 * the 1997 recording instead of the remaster, and the sixteen tracks the
 * rerelease adds (64..79) were absent, so OGG_WrapTrack() folded them onto
 * whatever was in range - track 67, which eleven maps use, came out as track 7.
 *
 * Called from CL_RestartFilesystem(), the one place the gamedir actually moves.
 */
void
OGG_RestartTrackList(void)
{
	if (!ogg_started)
	{
		return;
	}

	/* Whatever is playing came out of the OLD gamedir and its path string is
	   about to be freed, so stop before rescanning. The caller reloads the map
	   afterwards, which re-issues CS_CDTRACK against the new list. */
	OGG_Stop();
	ogg_curfile = -1;

	OGG_InitTrackList();
}

/*
 * Fold a requested track number onto one we actually have.
 *
 * Anything we do have is returned untouched, so normal playback is unaffected.
 * Anything else wraps cyclically through the tracks that exist, which is what
 * makes the rerelease maps' track 64-77 requests play something instead of
 * printing an error and leaving the level silent.
 */
static int
OGG_WrapTrack(int trackNo)
{
	int index;

	if (ogg_numtracks <= 0)
	{
		return trackNo;
	}

	/* we have this one - nothing to do */
	if (trackNo >= 0 && trackNo < MAX_NUM_OGGTRACKS && ogg_tracks[trackNo] != NULL)
	{
		return trackNo;
	}

	index = (trackNo - ogg_firsttrack) % ogg_numtracks;

	if (index < 0)
	{
		index += ogg_numtracks;
	}

	return ogg_tracklist[index];
}

// --------

/*
 * QUAKE III ARENA MUSIC
 *
 * A Quake III map names its music in worldspawn's "music" key: one .wav, or
 * an intro and a loop ("music/fla22k_04_intro.wav music/fla22k_04_loop.wav"),
 * kept in its pk3s - not the Ogg tracks this player otherwise streams from
 * disk. The game passes that string on in CS_CDTRACK and the client hands it
 * here (precache.c, CL_PlayMapMusic). Plain PCM, so it goes to the same raw
 * sample stream with no decoding: the intro once, then the loop over and over
 * - the intro itself, when there is no loop, as Quake III does. ogg_status and
 * the volume work for it as for a track; ogg_curfile is OGG_WAV_MUSIC.
 */
#define OGG_WAV_MUSIC   -2

static struct {
	byte        *file;          // the whole .wav (FS_LoadFile)
	const byte  *pcm;           // its data chunk
	size_t      size, pos;      // bytes
	int         rate, width, channels;
	char        intro[MAX_QPATH];
	char        loop[MAX_QPATH];
	bool        in_loop;
} wav_music;

static void WAV_Close(void)
{
	if (wav_music.file)
		FS_FreeFile(wav_music.file);
	wav_music.file = NULL;
	wav_music.pcm = NULL;
	wav_music.size = wav_music.pos = 0;
}

static bool WAV_Open(const char *name)
{
	byte    *file = NULL;
	int     len;
	const byte *p, *end;
	bool    have_fmt = false;

	WAV_Close();

	len = FS_LoadFile(name, (void **)&file);
	if (!file) {
		Com_Printf("Music: couldn't load %s\n", name);
		return false;
	}
	if (len < 12 || memcmp(file, "RIFF", 4) || memcmp(file + 8, "WAVE", 4)) {
		Com_Printf("Music: %s is not a WAV file\n", name);
		FS_FreeFile(file);
		return false;
	}

	// walk the chunks for "fmt " and "data"
	p = file + 12;
	end = file + len;
	while (end - p >= 8) {
		uint32_t size = p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24);
		const byte *body = p + 8;

		if ((size_t)(end - body) < size)
			size = (uint32_t)(end - body);

		if (!memcmp(p, "fmt ", 4) && size >= 16) {
			int format = body[0] | (body[1] << 8);
			wav_music.channels = body[2] | (body[3] << 8);
			wav_music.rate = body[4] | (body[5] << 8) | (body[6] << 16) | (body[7] << 24);
			wav_music.width = (body[14] | (body[15] << 8)) / 8;
			if (format != 1 || (wav_music.width != 1 && wav_music.width != 2) ||
				wav_music.channels < 1 || wav_music.channels > 2 || wav_music.rate <= 0) {
				Com_Printf("Music: %s is not 8 or 16 bit PCM mono or stereo\n", name);
				break;
			}
			have_fmt = true;
		} else if (!memcmp(p, "data", 4) && have_fmt) {
			wav_music.file = file;
			wav_music.pcm = body;
			wav_music.size = size - size % (wav_music.width * wav_music.channels);
			wav_music.pos = 0;
			return wav_music.size > 0;
		}

		p = body + size + (size & 1);
	}

	Com_Printf("Music: %s has no PCM data\n", name);
	FS_FreeFile(file);
	return false;
}

static void WAV_Read(void)
{
	const size_t frame = (size_t)wav_music.width * wav_music.channels;
	size_t bytes;

	if (!wav_music.pcm || wav_music.pos >= wav_music.size) {
		// the intro is over, or the loop came round: the loop from its start
		if (!WAV_Open(wav_music.loop[0] ? wav_music.loop : wav_music.intro)) {
			ogg_status = STOP;
			ogg_curfile = -1;
			return;
		}
		wav_music.in_loop = true;
	}

	bytes = min(wav_music.size - wav_music.pos, frame * 4096);
	S_RawSamples((int)(bytes / frame), wav_music.rate, wav_music.width, wav_music.channels,
		(byte *)(wav_music.pcm + wav_music.pos), S_GetLinearVolume(ogg_volume->value));
	wav_music.pos += bytes;
}

/*
 * Play a Quake III map's music: (intro), then (loop) round and round. Paths
 * as the filesystem takes them. Leaves the same music playing alone.
 */
void
OGG_PlayMusicFiles(const char *intro, const char *loop)
{
	if (s_started == SS_NOT || !intro || !intro[0])
		return;

	if (ogg_curfile == OGG_WAV_MUSIC && ogg_status != STOP &&
		!Q_stricmp(wav_music.intro, intro) && !Q_stricmp(wav_music.loop, loop ? loop : ""))
		return;

	OGG_Stop();

	Q_strlcpy(wav_music.intro, intro, sizeof(wav_music.intro));
	Q_strlcpy(wav_music.loop, loop ? loop : "", sizeof(wav_music.loop));
	wav_music.in_loop = false;
	if (!WAV_Open(intro))
		return;

	ogg_curfile = OGG_WAV_MUSIC;
	ogg_numsamples = 0;
	ogg_status = ogg_enable->integer ? PLAY : PAUSE;
}

// <basedir>/id1/<dir>/music/trackNN.ogg (or TrackNN, for a case-sensitive disk)
static bool Q1_TrackIn(const char *dir, int track, char *out, size_t size)
{
	if (Q_snprintf(out, size, "%s/id1/%s/music/track%02d.ogg", sys_basedir->string, dir, track) < size &&
		Sys_IsFile(out))
		return true;
	if (Q_snprintf(out, size, "%s/id1/%s/music/Track%02d.ogg", sys_basedir->string, dir, track) < size &&
		Sys_IsFile(out))
		return true;
	return false;
}

/*
 * The episode's own track, else id1's; else <basedir>/id1/music/ (a
 * rerelease id1 folder copied in whole). A number nobody has - none of the
 * rerelease's maps asks for one - folds onto id1's 2-11.
 */
static bool Q1_FindTrack(const char *episode, int track, char *out, size_t size)
{
	if (Q1_TrackIn(episode, track, out, size))
		return true;
	if (Q_stricmp(episode, "id1") && Q1_TrackIn("id1", track, out, size))
		return true;
	if (Q_snprintf(out, size, "%s/id1/music/track%02d.ogg", sys_basedir->string, track) < size &&
		Sys_IsFile(out))
		return true;
	if (track > 11)
		return Q1_TrackIn("id1", 2 + (track - 2) % 10, out, size);
	return false;
}

// (re)open q1_music.path from the start
static bool Q1_OpenMusic(void)
{
	int res = 0;
	FILE *f = fopen(q1_music.path, "rb");

	if (!f)
	{
		Com_Printf("OGG: could not open %s: %s.\n", q1_music.path, strerror(errno));
		return false;
	}

	// a file that will not open is closed by stb_vorbis itself (close_on_free)
	ogg_file = stb_vorbis_open_file(f, true, &res, NULL);
	if (!ogg_file)
	{
		Com_Printf("OGG: '%s' is not a valid Ogg Vorbis file (error %i).\n", q1_music.path, res);
		return false;
	}

	ogg_curfile = OGG_Q1_MUSIC;
	ogg_numsamples = 0;
	ogg_status = ogg_enable->integer ? PLAY : PAUSE;
	return true;
}

/*
 * Play Quake 1 CD track (track) of (episode): "id1", "hipnotic", "rogue",
 * "mg3"... Track 0 is no music, as in Quake. Leaves the same track playing
 * alone - a reload, the next map of a run asking for the one already on.
 */
void
OGG_PlayQuake1Track(const char *episode, int track)
{
	char path[MAX_OSPATH];

	if (s_started == SS_NOT)
		return;

	if (track < 2 || !Q1_FindTrack(episode, track, path, sizeof(path)))
	{
		if (track >= 2)
			Com_Printf("OGG: no Quake 1 track %d for %s.\n", track, episode);
		OGG_Stop();
		ogg_curfile = -1;
		return;
	}

	if (ogg_curfile == OGG_Q1_MUSIC && ogg_status != STOP && !Q_stricmp(q1_music.path, path))
		return;

	OGG_Stop();
	ogg_curfile = -1;

	Q_strlcpy(q1_music.path, path, sizeof(q1_music.path));
	q1_music.track = track;
	Q1_OpenMusic();
}

/*
 * Play a portion of the currently opened file.
 */
void
static OGG_Read(void)
{
	if (ogg_curfile == OGG_WAV_MUSIC) {
		WAV_Read();
		return;
	}

	if (!ogg_file) {
		ogg_status = STOP;
		return;
	}

	short samples[4096] = {0};

	int read_samples = stb_vorbis_get_samples_short_interleaved(ogg_file, ogg_file->channels, samples,
		sizeof(samples) / ogg_file->channels);

	if (read_samples > 0)
	{
		ogg_numsamples += read_samples;

		S_RawSamples(read_samples, ogg_file->sample_rate, ogg_file->channels, ogg_file->channels,
			(byte *)samples, S_GetLinearVolume(ogg_volume->value));
	}
	else
	{
		// We cannot call OGG_Stop() here. It flushes the OpenAL sample
		// queue, thus about 12 seconds of music are lost. Instead we
		// just set the OGG state to stop and open a new file. The new
		// files content is added to the sample queue after the remaining
		// samples from the old file.
		stb_vorbis_close(ogg_file);
		ogg_file = NULL;    // not left dangling if the next track will not open
		ogg_status = STOP;
		ogg_numbufs = 0;
		ogg_numsamples = 0;

		// a Quake 1 track round again; a Quake II one through OGG_PlayTrack
		if (ogg_curfile == OGG_Q1_MUSIC)
		{
			if (!Q1_OpenMusic())
				ogg_curfile = -1;
		}
		else
			OGG_PlayTrack(ogg_curfile);
	}
}

/*
 * Stream music.
 */
void
OGG_Stream(void)
{
	if (!ogg_started)
	{
		return;
	}

	if (ogg_status == PLAY)
	{
#ifdef USE_OPENAL
		if (s_started == SS_OAL)
		{
			/* Calculate the number of buffers used
			   for storing decoded OGG/Vorbis data.
			   We take the number of active buffers
			   and add 256. 256 are about 12 seconds
			   worth of sound, more than enough to
			   be resilent against underruns. */
			if (ogg_numbufs == 0 || active_buffers < ogg_numbufs - 256)
			{
				ogg_numbufs = active_buffers + 256;
			}

			/* active_buffers are all active OpenAL buffers,
			   buffering normal sfx _and_ ogg/vorbis samples. */
			// a read can end the music (a Quake III loop that will not
			// open): no buffer comes of it then, so stop asking. Nor does
			// one OpenAL refuses (another rate already queued: a video's
			// sound, al.c AL_RawSamples) - asking again would run through
			// the whole track this frame - nor the read that moves on to
			// the next track: either way, the next frame tries again.
			while (active_buffers <= ogg_numbufs && ogg_status == PLAY)
			{
				int before = active_buffers;

				OGG_Read();
				if (active_buffers == before)
					break;
			}
		}
		else /* using SDL */
#endif
		{
			if (s_started == SS_DMA)
			{
				/* Read that number samples into the buffer, that
				   were played since the last call to this function.
				   This keeps the buffer at all times at an "optimal"
				   fill level. */
				while (paintedtime + S_MAX_RAW_SAMPLES - 2048 > s_rawend && ogg_status == PLAY)
				{
					OGG_Read();
				}
			}
		}
	}
}

// --------

/*
 * play the ogg file that corresponds to the CD track with the given number
 */
void
OGG_PlayTrack(int trackNo)
{
	if (s_started == SS_NOT)
		return;

	// Track 0 means "stop music".
	if(trackNo == 0)
	{
		if(ogg_ignoretrack0->value == 0)
		{
			OGG_Stop();
		}

		// Special case: If ogg_ignoretrack0 is 0 we stopped the music (see above)
		// and ogg_curfile is still holding the last track played (track >1). So
		// this triggers and we return. If ogg_ignoretrack is 1 we didn't stop the
		// music, as soon as the tracks ends OGG_Read() starts it over. Until here
		// everything's okay.
		// But if ogg_ignoretrack0 is 1, the game was just restarted and a save game
		// load send us trackNo 0, we would end up without music. Since we have no
		// way to get the last track before trackNo 0 was set just fall through and
		// shuffle a random track (see below).
		if (ogg_curfile > 0)
		{
			return;
		}
	}

	// Player has requested shuffle playback.
	if((trackNo == 0) || ogg_shuffle->value)
	{
		if(ogg_maxfileindex > 0)
		{
			trackNo = Q_rand() % (ogg_maxfileindex+1);
			int retries = 100;
			while(ogg_tracks[trackNo] == NULL && retries-- > 0)
			{
				trackNo = Q_rand() % (ogg_maxfileindex+1);
			}
		}
	}

	if(ogg_maxfileindex == 0)
	{
		return; // no ogg files at all, ignore this silently instead of printing warnings all the time
	}

	if (trackNo < 2)
	{
		Com_Printf("OGG_PlayTrack: %d out of range.\n", trackNo);
		return;
	}

	/* a track we do not have folds onto one we do, rather than playing nothing */
	{
		int wrapped = OGG_WrapTrack(trackNo);

		if (wrapped != trackNo)
		{
			Com_Printf("OGG_PlayTrack: no file for track %d, playing %d instead.\n",
				trackNo, wrapped);
			trackNo = wrapped;
		}
	}

	/* a Quake III or Quake 1 map's music gives way to the track */
	if (ogg_curfile == OGG_WAV_MUSIC || ogg_curfile == OGG_Q1_MUSIC)
	{
		OGG_Stop();
		ogg_curfile = -1;
	}

	/* Check running music. */
	if (ogg_status == PLAY)
	{
		if (ogg_curfile == trackNo)
		{
			return;
		}
		else
		{
			OGG_Stop();
		}
	}

	if (ogg_tracks[trackNo] == NULL)
	{
		Com_Printf("OGG_PlayTrack: I don't have a file for track %d!\n", trackNo);

		return;
	}

	/* Open ogg vorbis file. */
	FILE* f = fopen(ogg_tracks[trackNo], "rb");

	if (f == NULL)
	{
		Com_Printf("OGG_PlayTrack: could not open file %s for track %d: %s.\n", ogg_tracks[trackNo], trackNo, strerror(errno));
		ogg_tracks[trackNo] = NULL;

		return;
	}

	int res = 0;
	ogg_file = stb_vorbis_open_file(f, true, &res, NULL);

	if (res != 0)
	{
		Com_Printf("OGG_PlayTrack: '%s' is not a valid Ogg Vorbis file (error %i).\n", ogg_tracks[trackNo], res);
		fclose(f);

		return;
	}

	/* Play file. */
	ogg_curfile = trackNo;
	ogg_numsamples = 0;
	if (ogg_enable->integer)
		ogg_status = PLAY;
	else
		ogg_status = PAUSE;
}

// ----

/*
 * List Ogg Vorbis files and print current playback state.
 */
static void
OGG_Info(void)
{
	Com_Printf("Tracks:\n");
	int numFiles = 0;

	for (int i = 2; i <= ogg_maxfileindex; i++)
	{
		if(ogg_tracks[i])
		{
			Com_Printf(" - %02d %s\n", i, ogg_tracks[i]);
			++numFiles;
		}
		else
		{
			Com_Printf(" - %02d <none>\n", i);
		}
	}

	Com_Printf("Total: %d Ogg/Vorbis files.\n", ogg_maxfileindex+1);

	if (ogg_curfile == OGG_WAV_MUSIC && ogg_status != STOP)
	{
		Com_Printf("State: %s Quake III music %s%s%s.\n", ogg_status == PLAY ? "Playing" : "Paused",
			wav_music.in_loop && wav_music.loop[0] ? wav_music.loop : wav_music.intro,
			wav_music.loop[0] && !wav_music.in_loop ? ", then " : "",
			wav_music.loop[0] && !wav_music.in_loop ? wav_music.loop : "");
		return;
	}

	if (ogg_curfile == OGG_Q1_MUSIC && ogg_status != STOP)
	{
		Com_Printf("State: %s Quake 1 track %d (%s) at %i samples.\n",
			ogg_status == PLAY ? "Playing" : "Paused", q1_music.track, q1_music.path,
			ogg_file ? stb_vorbis_get_sample_offset(ogg_file) : 0);
		return;
	}

	switch (ogg_status)
	{
		case PLAY:
			Com_Printf("State: Playing file %d (%s) at %i samples.\n",
			           ogg_curfile, ogg_tracks[ogg_curfile], stb_vorbis_get_sample_offset(ogg_file));
			break;

		case PAUSE:
			Com_Printf("State: Paused file %d (%s) at %i samples.\n",
			           ogg_curfile, ogg_tracks[ogg_curfile], stb_vorbis_get_sample_offset(ogg_file));
			break;

		case STOP:
			if (ogg_curfile == -1)
			{
				Com_Printf("State: Stopped.\n");
			}
			else
			{
				Com_Printf("State: Stopped file %d (%s).\n", ogg_curfile, ogg_tracks[ogg_curfile]);
			}

			break;
	}
}

/*
 * Stop playing the current file.
 */
void
OGG_Stop(void)
{
	if (ogg_status == STOP)
	{
		return;
	}

#ifdef USE_OPENAL
	if (s_started == SS_OAL)
	{
		AL_UnqueueRawSamples();
	}
#endif

	if (ogg_curfile == OGG_WAV_MUSIC)
	{
		// no track to name afterwards: ogg_tracks[] has no entry for it
		WAV_Close();
		ogg_curfile = -1;
	}
	else
	{
		stb_vorbis_close(ogg_file);
		// nor for a Quake 1 track (OGG_Info would index ogg_tracks[-3])
		if (ogg_curfile == OGG_Q1_MUSIC)
			ogg_curfile = -1;
	}
	ogg_file = NULL;
	ogg_status = STOP;
	ogg_numbufs = 0;
}

/*
 * Pause or resume playback.
 */
static void
OGG_TogglePlayback(void)
{
	if (ogg_status == PLAY)
	{
		ogg_status = PAUSE;
		ogg_numbufs = 0;

#ifdef USE_OPENAL
		if (s_started == SS_OAL)
		{
			AL_UnqueueRawSamples();
		}
#endif
	}
	else if (ogg_status == PAUSE)
	{
		ogg_status = PLAY;
	}
}

/*
 * Prints a help message for the 'ogg' cmd.
 */
void
OGG_HelpMsg(void)
{
	Com_Printf("Unknown sub command %s\n\n", Cmd_Argv(1));
	Com_Printf("Commands:\n");
	Com_Printf(" - info: Print information about playback state and tracks\n");
	Com_Printf(" - play <track>: Play track number <track>\n");
	Com_Printf(" - stop: Stop playback\n");
	Com_Printf(" - toggle: Toggle pause\n");
}

/*
 * The 'ogg' cmd. Gives some control and information about the playback state.
 */
void
OGG_Cmd(void)
{
	if (Cmd_Argc() < 2)
	{
		OGG_HelpMsg();
		return;
	}

	if (Q_stricmp(Cmd_Argv(1), "info") == 0)
	{
		OGG_Info();
	}
	else if (Q_stricmp(Cmd_Argv(1), "play") == 0)
	{
		if (Cmd_Argc() != 3)
		{
			Com_Printf("ogg play <track> : Play <track>");
			return;
		}

		int track = (int)strtol(Cmd_Argv(2), NULL, 10);

		/* no upper bound: OGG_PlayTrack folds a track we do not have onto
		   one we do, exactly as it does for a map that asks for one */
		if (track < 2)
		{
			Com_Printf("invalid track %s, must be a number >= 2 (have %d..%d)\n",
				Cmd_Argv(2), ogg_firsttrack,
				ogg_numtracks > 0 ? ogg_tracklist[ogg_numtracks - 1] : 0);
			return;
		}
		else
		{
			OGG_PlayTrack(track);
		}
	}
	else if (Q_stricmp(Cmd_Argv(1), "stop") == 0)
	{
		OGG_Stop();
	}
	else if (Q_stricmp(Cmd_Argv(1), "toggle") == 0)
	{
		OGG_TogglePlayback();
	}
	else
	{
		OGG_HelpMsg();
	}
}

/*
 * Saves the current state of the subsystem.
 */
void
OGG_SaveState(void)
{
	// a Quake III map's music starts again with its map (CL_PlayMapMusic)
	if (ogg_status != PLAY || ogg_curfile == OGG_WAV_MUSIC)
	{
		ogg_saved_state.saved = false;
		return;
	}

	ogg_saved_state.saved = true;
	ogg_saved_state.curfile = ogg_curfile;
	ogg_saved_state.numsamples = ogg_numsamples;
}

/*
 * Recover the previously saved state.
 */
void
OGG_RecoverState(void)
{
	if (!ogg_saved_state.saved)
	{
		return;
	}

	// a Quake 1 track: q1_music.path outlives the stop, so the same file
	if (ogg_saved_state.curfile == OGG_Q1_MUSIC)
	{
		if (Q1_OpenMusic())
		{
			stb_vorbis_seek_frame(ogg_file, ogg_saved_state.numsamples);
			ogg_numsamples = ogg_saved_state.numsamples;
		}
		return;
	}

	// Mkay, ultra evil hack to recover the state in case of
	// shuffled playback. OGG_PlayTrack() does the shuffeling,
	// so switch it of before and enable after state recovery.
	int shuffle_state = ogg_shuffle->value;
	Cvar_SetValue(ogg_shuffle, 0, FROM_CODE);

	OGG_PlayTrack(ogg_saved_state.curfile);
	stb_vorbis_seek_frame(ogg_file, ogg_saved_state.numsamples);
	ogg_numsamples = ogg_saved_state.numsamples;

	Cvar_SetValue(ogg_shuffle, shuffle_state, FROM_CODE);
}

// --------

static void ogg_enable_changed(cvar_t *self)
{
	if ((ogg_enable->integer && ogg_status == PAUSE) || (!ogg_enable->integer && ogg_status == PLAY))
	{
		OGG_TogglePlayback();
	}
}


/*
 * Initialize the Ogg Vorbis subsystem.
 */
void
OGG_Init(void)
{
	// Cvars
	ogg_shuffle = Cvar_Get("ogg_shuffle", "0", CVAR_ARCHIVE);
	ogg_ignoretrack0 = Cvar_Get("ogg_ignoretrack0", "0", CVAR_ARCHIVE);
	ogg_volume = Cvar_Get("ogg_volume", "1.0", CVAR_ARCHIVE);
	ogg_enable = Cvar_Get("ogg_enable", "1", CVAR_ARCHIVE);
	ogg_enable->changed = ogg_enable_changed;

	// Commands
	Cmd_AddCommand("ogg", OGG_Cmd);

	// Global variables
	ogg_curfile = -1;
	ogg_numsamples = 0;
	ogg_status = STOP;

	ogg_started = true;
}

/*
 * Shutdown the Ogg Vorbis subsystem.
 */
void
OGG_Shutdown(void)
{
	if (!ogg_started)
	{
		return;
	}

	// Music must be stopped.
	OGG_Stop();

	// Free file lsit.
	for(int i=0; i<MAX_NUM_OGGTRACKS; ++i)
	{
		if(ogg_tracks[i] != NULL)
		{
			free(ogg_tracks[i]);
			ogg_tracks[i] = NULL;
		}
	}
	ogg_maxfileindex = 0;

	// Remove console commands
	Cmd_RemoveCommand("ogg");

	ogg_started = false;
}
