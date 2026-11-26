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
static cvar_t *ogg_q1_loudness;   /* LUFS a Quake 1 track is brought to, 0 = as recorded */
static cvar_t *ogg_q1_boost;      /* dB a Quake 1 track plays over that, in OpenAL's float mix */
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
	float   lufs;       // its loudness (Q1_Loudness), 0 when not known
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

/*
 * QUAKE 1 MUSIC LEVEL
 *
 * The rerelease's Quake 1 soundtrack is mastered far quieter than Quake II's.
 * Measured as ITU BS.1770 loudness (K-weighted and gated, as EBU R128 and
 * ReplayGain do), Quake II's tracks and Quake III's music sit near -12 LUFS,
 * and the Quake sound effects as loud as Quake II's - but id1's tracks 2-5 are
 * -19 to -20, hipnotic's and rogue's -16 to -21. On a Quake map the music was
 * half as loud against the effects, and ogg_volume could not make up for it:
 * at full volume it already is at the most S_GetLinearVolume gives. Nor would
 * one fixed boost: the Quake tracks differ among themselves by up to 9 dB
 * (id1's 6-9 are -12 to -15, mg3's -11 to -16).
 *
 * So each Quake 1 track is measured as it starts and raised or lowered to
 * ogg_q1_loudness (LUFS; 0 plays it as recorded), by no more than 12 dB.
 * Measured from 16 stretches of 2.5 s spread through the file, not the whole
 * of it: within 0.7 dB of a full decode on every rerelease track, in 40-120 ms
 * rather than up to 0.8 s, and once a session for each track.
 *
 * Raised to -12, the loudest hits go past full scale - under 1% of the
 * samples, hardly ever by more than 3 dB. A 5 ms look-ahead limiter holds
 * them at -1 dBFS instead of clipping them. Quake's loud passages have such a
 * hit in nearly every 400 ms, though, so the limiter keeps those down and a
 * track comes out a little under the target: id1's track 2 (-20 LUFS) at
 * -14.9, most at -12 to -13.5. A higher target only squashes them harder
 * (track 2 at -10 comes out at -14.1).
 *
 * Matt, 2026-11-25, at -12: "still needs to be a little bit more loud to match
 * the sound effects". Louder in 16 bits is only more of that squashing, so
 * ogg_q1_boost (dB, default 3) raises it AFTER the limiter, as the music
 * source's gain in OpenAL's float mix (al.c lets the stream go past 1 for
 * this). Peaks then pass full scale by up to the boost less 1 dB, and the
 * output limiter OpenAL Soft is opened with (qal/fixed.c al_limiter) holds
 * them there rather than the device clipping them.
 */
#define Q1_BOOST_MAX_DB         12.0f
#define Q1_LEVEL_STRETCHES      16
#define Q1_LEVEL_STRETCH_SEC    2.5
#define Q1_LEVEL_MAX_BLOCKS     512     // of 100 ms: 16 x 25, or all of a track up to 40 s
#define Q1_LEVEL_MAX_DB         12.0f
#define Q1_LEVEL_CEILING        (0.891f * 32767.0f)    // -1 dBFS
#define Q1_LIMIT_MAX_FRAMES     512     // 5 ms of look-ahead at up to 102 kHz

typedef struct {
	double  b0, b1, b2, a1, a2;
	double  z1[2], z2[2];
} q1_biquad_t;

static double Q1_Biquad(q1_biquad_t *f, int c, double x)
{
	double y = f->b0 * x + f->z1[c];

	f->z1[c] = f->b1 * x - f->a1 * y + f->z2[c];
	f->z2[c] = f->b2 * x - f->a2 * y;
	return y;
}

// BS.1770's K-weighting at (rate): a 4 dB shelf above about 1.7 kHz, then a
// high pass at 38 Hz (libebur128's design, which holds at any sample rate)
static void Q1_KWeighting(q1_biquad_t *shelf, q1_biquad_t *hp, int rate)
{
	double K = tan(M_PI * 1681.974450955533 / rate);
	double Q = 0.7071752369554196;
	double Vh = pow(10.0, 3.999843853973347 / 20.0);
	double Vb = pow(Vh, 0.4996667741545416);
	double a0 = 1.0 + K / Q + K * K;

	memset(shelf, 0, sizeof(*shelf));
	shelf->b0 = (Vh + Vb * K / Q + K * K) / a0;
	shelf->b1 = 2.0 * (K * K - Vh) / a0;
	shelf->b2 = (Vh - Vb * K / Q + K * K) / a0;
	shelf->a1 = 2.0 * (K * K - 1.0) / a0;
	shelf->a2 = (1.0 - K / Q + K * K) / a0;

	K = tan(M_PI * 38.13547087602444 / rate);
	Q = 0.5003270373238773;
	a0 = 1.0 + K / Q + K * K;
	memset(hp, 0, sizeof(*hp));
	hp->b0 = 1.0;
	hp->b1 = -2.0;
	hp->b2 = 1.0;
	hp->a1 = 2.0 * (K * K - 1.0) / a0;
	hp->a2 = (1.0 - K / Q + K * K) / a0;
}

// LUFS of the Ogg Vorbis file at (path); 0, which no music measures, when it
// cannot be measured
static float Q1_MeasureLoudness(const char *path)
{
	static double   sub[Q1_LEVEL_MAX_BLOCKS];      // mean square of each 100 ms
	static bool     first[Q1_LEVEL_MAX_BLOCKS];    // the first of its stretch
	short           buf[4096];
	q1_biquad_t     shelf, hp;
	stb_vorbis_info info;
	stb_vorbis      *v;
	unsigned        length, stretch;
	double          scale, sum = 0, gate;
	int             stretches = Q1_LEVEL_STRETCHES, step, n = 0, count = 0, res = 0;
	FILE            *f = fopen(path, "rb");

	if (!f)
		return 0;
	v = stb_vorbis_open_file(f, true, &res, NULL);    // closes (f) itself on failure
	if (!v)
		return 0;

	info = stb_vorbis_get_info(v);
	length = stb_vorbis_stream_length_in_samples(v);
	step = info.sample_rate / 10;
	stretch = (unsigned)(Q1_LEVEL_STRETCH_SEC * info.sample_rate);
	if (step <= 0 || !length) {
		stb_vorbis_close(v);
		return 0;
	}
	if ((uint64_t)stretch * stretches >= length) {
		stretches = 1;          // a short track: all of it
		stretch = length;
	}

	for (int s = 0; s < stretches && n < Q1_LEVEL_MAX_BLOCKS; s++) {
		unsigned    done = 0;
		double      acc = 0;
		int         accn = 0;
		int         settle = info.sample_rate / 20;   // the filters, after a seek
		bool        start = true;

		if (stretches > 1 &&
			!stb_vorbis_seek(v, (unsigned)((s + 0.5) / stretches * length) - stretch / 2))
			break;
		Q1_KWeighting(&shelf, &hp, info.sample_rate);

		while (done < stretch && n < Q1_LEVEL_MAX_BLOCKS) {
			// two channels whatever the file has: stb_vorbis puts a mono
			// one in both
			int want = (int)min(stretch - done, sizeof(buf) / sizeof(buf[0]) / 2);
			int got = stb_vorbis_get_samples_short_interleaved(v, 2, buf, want * 2);

			if (got <= 0)
				break;
			done += got;

			for (int i = 0; i < got && n < Q1_LEVEL_MAX_BLOCKS; i++) {
				double e = 0;

				for (int c = 0; c < 2; c++) {
					double y = Q1_Biquad(&hp, c, Q1_Biquad(&shelf, c, buf[i * 2 + c] * (1.0 / 32768.0)));
					e += y * y;
				}
				if (settle > 0) {
					settle--;
					continue;
				}
				acc += e;
				if (++accn == step) {
					sub[n] = acc / step;
					first[n] = start;
					start = false;
					n++;
					acc = 0;
					accn = 0;
				}
			}
		}
	}
	stb_vorbis_close(v);

	// BS.1770 counts a mono file's one channel once, not in both
	scale = info.channels == 1 ? 0.5 : 1.0;

	// 400 ms blocks overlapping by 300 ms, none across a seek. Gated at
	// -70 LUFS, then at 10 LU under the mean of what passed that.
	gate = pow(10.0, (-70.0 + 0.691) / 10.0);
	for (int pass = 0; pass < 2; pass++) {
		sum = 0;
		count = 0;
		for (int i = 3; i < n; i++) {
			double z;

			if (first[i] || first[i - 1] || first[i - 2])
				continue;
			z = (sub[i] + sub[i - 1] + sub[i - 2] + sub[i - 3]) * 0.25 * scale;
			if (z > gate) {
				sum += z;
				count++;
			}
		}
		if (!count)
			return 0;       // silence, or too short to measure
		gate = sum / count * 0.1;
	}

	return (float)(-0.691 + 10.0 * log10(sum / count));
}

// (path)'s loudness, measured the first time it is asked for this session
static float Q1_Loudness(const char *path)
{
	static struct {
		char    path[MAX_OSPATH];
		float   lufs;
	} known[64];
	static int  numknown, next;
	unsigned    start;
	float       lufs;
	int         i;

	for (i = 0; i < numknown; i++)
		if (!Q_stricmp(known[i].path, path))
			return known[i].lufs;

	start = Sys_Milliseconds();
	lufs = Q1_MeasureLoudness(path);
	Com_DPrintf("OGG: %s measures %.1f LUFS (%u ms).\n", path, lufs, Sys_Milliseconds() - start);

	i = numknown < (int)q_countof(known) ? numknown++ : next++ % (int)q_countof(known);
	Q_strlcpy(known[i].path, path, sizeof(known[i].path));
	known[i].lufs = lufs;
	return lufs;
}

// the gain, in dB, that brings the Quake 1 track playing to ogg_q1_loudness
static float Q1_LevelDB(void)
{
	float db;

	if (!ogg_q1_loudness->value || !q1_music.lufs)
		return 0;
	db = ogg_q1_loudness->value - q1_music.lufs;
	return clamp(db, -Q1_LEVEL_MAX_DB, Q1_LEVEL_MAX_DB);
}

// ogg_q1_boost, in dB: what a Quake 1 track plays at over its level, as gain
// on the music source (QUAKE 1 MUSIC LEVEL)
static float Q1_BoostDB(void)
{
	float db = ogg_q1_boost->value;

	return clamp(db, 0.0f, Q1_BOOST_MAX_DB);
}

static struct {
	int     len;            // look-ahead in frames; 0 starts afresh
	int     pos;
	float   delay[Q1_LIMIT_MAX_FRAMES][2];
	float   gains[Q1_LIMIT_MAX_FRAMES];     // hold, over the last len frames
	double  sum;            // of gains[]
	float   hold;           // the gain the loudest frame ahead needs
	int     count;          // frames left to keep hold
	float   release;        // the share of the way back to 1 per frame
} q1_limit;

static short Q1_Clip16(float x)
{
	int i = Q_rint(x);

	return (short)clamp(i, -32768, 32767);
}

/*
 * Bring (frames) of the Quake 1 track playing to its level, in place.
 *
 * Gained frames go through a delay of len frames (5 ms). Each needs at most
 * ceiling / peak of gain; hold takes that as each frame comes in, keeps it
 * the len frames that frame takes to come out, then eases back up to 1 over
 * about 150 ms. The gain a frame goes out at is the mean of hold over the
 * len frames since it came in, which every peak in the delay keeps at or
 * under its own need, so it glides down over 5 ms to the gain the peak
 * needs as the peak comes out, without a click.
 */
static void Q1_Level(short *samples, int frames, int channels, int rate)
{
	const float db = Q1_LevelDB();
	float gain;

	if (!db || channels < 1 || channels > 2) {
		q1_limit.len = 0;       // as recorded, the limiter not in the way
		return;
	}
	gain = powf(10.0f, db / 20.0f);

	if (!q1_limit.len) {
		q1_limit.len = rate * 5 / 1000;
		clamp(q1_limit.len, 1, Q1_LIMIT_MAX_FRAMES);
		q1_limit.pos = 0;
		for (int i = 0; i < q1_limit.len; i++) {
			q1_limit.gains[i] = 1.0f;
			q1_limit.delay[i][0] = q1_limit.delay[i][1] = 0.0f;
		}
		q1_limit.sum = q1_limit.len;
		q1_limit.hold = 1.0f;
		q1_limit.count = 0;
		q1_limit.release = 1.0f - expf(-1.0f / (0.15f * rate));
	}

	for (int i = 0; i < frames; i++) {
		short   *s = samples + i * channels;
		float   l = s[0] * gain;
		float   r = channels == 2 ? s[1] * gain : l;
		float   peak = max(fabsf(l), fabsf(r));
		float   need = peak > Q1_LEVEL_CEILING ? Q1_LEVEL_CEILING / peak : 1.0f;
		int     p = q1_limit.pos;
		float   g;

		if (need <= q1_limit.hold) {
			q1_limit.hold = need;
			q1_limit.count = q1_limit.len;
		} else if (q1_limit.count > 0) {
			q1_limit.count--;
		} else {
			q1_limit.hold += (1.0f - q1_limit.hold) * q1_limit.release;
			if (need < q1_limit.hold) {
				q1_limit.hold = need;
				q1_limit.count = q1_limit.len;
			}
		}

		q1_limit.sum += q1_limit.hold - q1_limit.gains[p];
		q1_limit.gains[p] = q1_limit.hold;
		g = (float)(q1_limit.sum / q1_limit.len);

		// out goes the frame from len frames ago; in goes this one
		s[0] = Q1_Clip16(q1_limit.delay[p][0] * g);
		if (channels == 2)
			s[1] = Q1_Clip16(q1_limit.delay[p][1] * g);
		q1_limit.delay[p][0] = l;
		q1_limit.delay[p][1] = r;
		q1_limit.pos = (p + 1) % q1_limit.len;
	}
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
	q1_music.lufs = Q1_Loudness(path);
	q1_limit.len = 0;
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

		float volume = S_GetLinearVolume(ogg_volume->value);

		// a Quake 1 track at the loudness of Quake II's, then ogg_q1_boost
		// over it in the float mix
		if (ogg_curfile == OGG_Q1_MUSIC)
		{
			Q1_Level(samples, read_samples, ogg_file->channels, ogg_file->sample_rate);
			volume *= powf(10.0f, Q1_BoostDB() / 20.0f);
		}

		S_RawSamples(read_samples, ogg_file->sample_rate, ogg_file->channels, ogg_file->channels,
			(byte *)samples, volume);
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
		if (q1_music.lufs)
			Com_Printf("Loudness: %.1f LUFS, played %+.1f dB (ogg_q1_loudness %s), then %+.1f dB (ogg_q1_boost).\n",
				q1_music.lufs, Q1_LevelDB(), ogg_q1_loudness->string, Q1_BoostDB());
		else
			Com_Printf("Loudness: not measured, played as recorded, then %+.1f dB (ogg_q1_boost).\n",
				Q1_BoostDB());
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
		q1_limit.len = 0;
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
	// Quake II's and Quake III's music measure about -12 (QUAKE 1 MUSIC LEVEL)
	ogg_q1_loudness = Cvar_Get("ogg_q1_loudness", "-12", CVAR_ARCHIVE);
	// Matt, 2026-11-25: a little louder than that against the effects
	ogg_q1_boost = Cvar_Get("ogg_q1_boost", "3", CVAR_ARCHIVE);

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
