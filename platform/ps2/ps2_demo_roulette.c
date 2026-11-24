// PS2 port - melodia de la ruleta de la demo de CTR (opcional).
//
// En la demo, al coger una caja sonaba la cancion 3 del paquete de musica de la
// pista (una secuencia corta), y en el juego final se cambio por el efecto 0x5d
// y se quito esa cancion. Si en el disco esta DEMO.HWL (el KART.HWL de la demo
// del propio usuario), al arrancar se saca de ahi la cancion 3 del primer
// paquete y los sonidos de sus instrumentos, se suben a una zona libre de la RAM
// de la SPU2 y la melodia se toca con voces libres del core 1 (2..9), sin tocar
// las voces ni la memoria de sonido del juego. Sin DEMO.HWL, nada cambia.
//
// Los datos de la demo NO van en el codigo: se leen del archivo del usuario.
// Incluido en el build unity (main_ps2.c), despues del juego.

#include "ps2_spu2.h"

#define PS2_DEMO_ROULETTE_SONG          3
#define PS2_DEMO_ROULETTE_MAX_SONG      2048 // bytes de la cancion (la de la demo: 484)
#define PS2_DEMO_ROULETTE_MAX_TRACKS    4
#define PS2_DEMO_ROULETTE_MAX_INSTR     4
#define PS2_DEMO_ROULETTE_FIRST_VOICE   2 // voces 0 y 1 del core 1: XA
#define PS2_DEMO_ROULETTE_NUM_VOICES    8
#define PS2_DEMO_ROULETTE_MAX_SAMPLE    (64 * 1024)
#define PS2_DEMO_ROULETTE_SONG_VOLUME   0xbe // como SongPool_Start para la cancion 3

int Ps2Cd_SearchFile(const char *name, unsigned int *lsnOut, unsigned int *sizeOut); // ps2_cd.c
int Ps2Cd_ReadSectors(unsigned int lsn, unsigned int count, void *buf);

struct Ps2DemoInstrument
{
	u8 used;
	u8 volume;
	u16 basePitch;
	u16 ad;
	u16 sr;
	u32 spu2Addr; // en la RAM de la SPU2 (bytes)
};

struct Ps2DemoTrack
{
	const u8 *firstNote;
	const u8 *currNote;
	u32 noteLength; // ticks hasta el siguiente evento
	u32 elapsed;
	u8 playing;
	u8 drums;
	u8 instrument;
	u8 volume; // opcode 06
	u8 pan;    // opcode 07
};

global_variable int s_ps2DemoReady = 0;
global_variable int s_ps2DemoPlaying = 0;
global_variable u8 s_ps2DemoSong[PS2_DEMO_ROULETTE_MAX_SONG] __attribute__((aligned(4)));
global_variable u32 s_ps2DemoNumTracks = 0;
global_variable u32 s_ps2DemoTempo = 0; // ticks por VBlank en 16.16
global_variable u32 s_ps2DemoTickFrac = 0;
global_variable struct Ps2DemoInstrument s_ps2DemoInstr[256];
global_variable struct Ps2DemoTrack s_ps2DemoTracks[PS2_DEMO_ROULETTE_MAX_TRACKS];
// Que nota de que pista suena en cada voz (para apagarla con el opcode 01).
global_variable s16 s_ps2DemoVoiceTrack[PS2_DEMO_ROULETTE_NUM_VOICES];
global_variable u8 s_ps2DemoVoiceNote[PS2_DEMO_ROULETTE_NUM_VOICES];
global_variable u32 s_ps2DemoVoiceAge[PS2_DEMO_ROULETTE_NUM_VOICES];
global_variable u32 s_ps2DemoAgeClock = 0;
global_variable u32 s_ps2DemoStarts = 0;

// Lectura del archivo por bytes (los sectores del DVD son de 2048 bytes).
global_variable u8 s_ps2DemoSector[2048 * 2] __attribute__((aligned(64)));
global_variable u32 s_ps2DemoLsn = 0;
global_variable u32 s_ps2DemoSize = 0;

internal int Ps2Demo_Read(u32 offset, void *dst, u32 size)
{
	u8 *out = (u8 *)dst;
	if ((offset > s_ps2DemoSize) || (size > s_ps2DemoSize - offset))
	{
		return 0;
	}
	while (size != 0)
	{
		const u32 sector = offset / 2048u;
		const u32 skip = offset % 2048u;
		u32 chunk = 2048u - skip;
		if (chunk > size)
		{
			chunk = size;
		}
		if (!Ps2Cd_ReadSectors(s_ps2DemoLsn + sector, 1, s_ps2DemoSector))
		{
			return 0;
		}
		memcpy(out, s_ps2DemoSector + skip, chunk);
		out += chunk;
		offset += chunk;
		size -= chunk;
	}
	return 1;
}

internal u32 Ps2Demo_U16(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8);
}

internal u32 Ps2Demo_U32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

// Mismas longitudes de evento que el juego (data.opcodeOffset).
global_variable const u8 s_ps2DemoOpcodeLen[11] = {0x01, 0x02, 0x02, 0x01, 0x02, 0x03, 0x02, 0x02, 0x02, 0x02, 0x02};

// Recorre una pista y apunta que instrumentos usa (opcode 09). 0 = mal formada.
internal int Ps2Demo_ScanTrack(const u8 *p, const u8 *end, u8 *usedInstr)
{
	for (int guard = 0; guard < 4096; guard++)
	{
		int len;
		p = howl_GetNextNote((u8 *)p, &len);
		if (p >= end)
		{
			return 0;
		}
		const u32 op = p[0];
		if (op > 10)
		{
			return 0;
		}
		if ((op == 9) && (p + 1 < end))
		{
			usedInstr[p[1]] = 1;
		}
		if (op == 3)
		{
			return 1;
		}
		p += s_ps2DemoOpcodeLen[op];
	}
	return 0;
}

void Ps2DemoRoulette_Init(void)
{
	static u8 header[4096];
	static u8 pack[64 * 1024];
	static u8 sample[PS2_DEMO_ROULETTE_MAX_SAMPLE] __attribute__((aligned(64)));

	if (!g_ps2Spu2Active)
	{
		printf("[CTR PS2] ruleta demo: sin chip SPU2, no se usa\n");
		return;
	}
	if (!Ps2Cd_SearchFile("\\DEMO.HWL;1", &s_ps2DemoLsn, &s_ps2DemoSize))
	{
		printf("[CTR PS2] ruleta demo: no hay DEMO.HWL en el disco (suena la ruleta normal)\n");
		return;
	}

	// Cabecera del HOWL (como howl_ParseHeader).
	if (!Ps2Demo_Read(0, header, sizeof(header)) || (memcmp(header, "HOWL", 4) != 0))
	{
		printf("[CTR PS2] ruleta demo: DEMO.HWL no es un archivo HOWL\n");
		return;
	}
	const u32 numSpu = Ps2Demo_U32(&header[0x10]);
	const u32 numOther = Ps2Demo_U32(&header[0x14]);
	const u32 numEngine = Ps2Demo_U32(&header[0x18]);
	const u32 numBanks = Ps2Demo_U32(&header[0x1c]);
	const u32 numPacks = Ps2Demo_U32(&header[0x20]);
	const u32 spuTable = 0x28;
	const u32 bankTable = spuTable + 4 * numSpu + 8 * numOther + 8 * numEngine;
	const u32 packTable = bankTable + 2 * numBanks;
	if ((numPacks == 0) || (packTable + 2 * numPacks > sizeof(header)))
	{
		printf("[CTR PS2] ruleta demo: cabecera de DEMO.HWL rara\n");
		return;
	}

	// Primer paquete de musica (como howl_ParseCseqHeader).
	const u32 packOffset = Ps2Demo_U16(&header[packTable]) * 2048u;
	u8 first[8];
	if (!Ps2Demo_Read(packOffset, first, sizeof(first)))
	{
		return;
	}
	const u32 packSize = Ps2Demo_U32(first);
	if ((packSize < 8) || (packSize > sizeof(pack)) || !Ps2Demo_Read(packOffset, pack, packSize))
	{
		printf("[CTR PS2] ruleta demo: paquete de musica demasiado grande (%u)\n", (unsigned)packSize);
		return;
	}
	const u32 numLong = pack[4];
	const u32 numShort = pack[5];
	const u32 numSongs = Ps2Demo_U16(&pack[6]);
	u32 addr = 8 + 12 * numLong + 8 * numShort;
	const u32 songOffsets = addr;
	addr += 2 * numSongs;
	if (addr & 1)
	{
		addr++;
	}
	if (addr & 2)
	{
		addr += 2;
	}
	const u32 songData = addr;
	if (numSongs <= PS2_DEMO_ROULETTE_SONG)
	{
		printf("[CTR PS2] ruleta demo: el paquete solo tiene %u canciones\n", (unsigned)numSongs);
		return;
	}
	const u32 songStart = songData + Ps2Demo_U16(&pack[songOffsets + 2 * PS2_DEMO_ROULETTE_SONG]);
	const u32 songEnd = (PS2_DEMO_ROULETTE_SONG + 1 < numSongs) ? (songData + Ps2Demo_U16(&pack[songOffsets + 2 * (PS2_DEMO_ROULETTE_SONG + 1)])) : packSize;
	if ((songStart >= songEnd) || (songEnd > packSize) || (songEnd - songStart > sizeof(s_ps2DemoSong)))
	{
		printf("[CTR PS2] ruleta demo: cancion 3 con tamano raro\n");
		return;
	}
	const u32 songBytes = songEnd - songStart;
	memcpy(s_ps2DemoSong, &pack[songStart], songBytes);

	// Cabecera de la cancion: unk, pistas, bpm, tpqn y desplazamientos.
	s_ps2DemoNumTracks = s_ps2DemoSong[1];
	const s32 bpm = (s16)Ps2Demo_U16(&s_ps2DemoSong[2]);
	const s32 tpqn = (s16)Ps2Demo_U16(&s_ps2DemoSong[4]);
	if ((s_ps2DemoNumTracks == 0) || (s_ps2DemoNumTracks > PS2_DEMO_ROULETTE_MAX_TRACKS) || (bpm <= 0) || (tpqn <= 0))
	{
		printf("[CTR PS2] ruleta demo: cancion 3 rara (%u pistas)\n", (unsigned)s_ps2DemoNumTracks);
		return;
	}
	// Ticks por VBlank (60 Hz) en 16.16.
	s_ps2DemoTempo = (u32)(((u64)bpm * (u64)tpqn << 16) / 3600u);

	u8 usedInstr[256];
	memset(usedInstr, 0, sizeof(usedInstr));
	const u8 *firstNotes = &s_ps2DemoSong[6 + 2 * s_ps2DemoNumTracks];
	const u8 *songLimit = &s_ps2DemoSong[songBytes];
	for (u32 t = 0; t < s_ps2DemoNumTracks; t++)
	{
		const u8 *trackHeader = firstNotes + Ps2Demo_U16(&s_ps2DemoSong[6 + 2 * t]);
		if (trackHeader + 2 >= songLimit)
		{
			printf("[CTR PS2] ruleta demo: pista %u fuera de la cancion\n", (unsigned)t);
			return;
		}
		s_ps2DemoTracks[t].firstNote = trackHeader + 2; // SongNoteHeader: flags, unk
		s_ps2DemoTracks[t].drums = (trackHeader[0] & 1) ? 1 : 0;
		if (s_ps2DemoTracks[t].drums)
		{
			printf("[CTR PS2] ruleta demo: la pista %u es de percusion (no soportada, se calla)\n", (unsigned)t);
			continue;
		}
		if (!Ps2Demo_ScanTrack(s_ps2DemoTracks[t].firstNote, songLimit, usedInstr))
		{
			printf("[CTR PS2] ruleta demo: pista %u mal formada\n", (unsigned)t);
			return;
		}
	}
	// El instrumento 0 se usa si una pista no pone ninguno (instrumentID inicial 0).
	usedInstr[0] = 1;

	// Instrumentos usados: parametros (SampleInstrument) y su sonido.
	u32 spu2Addr = PS2_SPU2_EXTRA_BASE;
	u32 uploaded = 0;
	memset(s_ps2DemoInstr, 0, sizeof(s_ps2DemoInstr));
	for (u32 i = 0; i < numLong; i++)
	{
		if (!usedInstr[i])
		{
			continue;
		}
		const u8 *inst = &pack[8 + 12 * i];
		const u32 spuIndex = Ps2Demo_U16(&inst[6]);
		if (spuIndex >= numSpu)
		{
			continue;
		}
		const u32 sampleSize = Ps2Demo_U16(&header[spuTable + 4 * spuIndex + 2]) * 8u;
		if ((sampleSize == 0) || (sampleSize > sizeof(sample)) || (spu2Addr + sampleSize > 0x100000u))
		{
			continue;
		}
		// Buscar un banco que lleve ese sonido: el primer sector del banco tiene
		// el numero de sonidos y sus indices; los datos van detras, en orden.
		int found = 0;
		for (u32 b = 0; (b < numBanks) && !found; b++)
		{
			const u32 bankOffset = Ps2Demo_U16(&header[bankTable + 2 * b]) * 2048u;
			u8 bankHead[2048];
			if (!Ps2Demo_Read(bankOffset, bankHead, sizeof(bankHead)))
			{
				break;
			}
			const u32 count = Ps2Demo_U16(bankHead);
			u32 pos = bankOffset + 2048u;
			for (u32 k = 0; (k < count) && (2 + 2 * k + 1 < sizeof(bankHead)); k++)
			{
				const u32 idx = Ps2Demo_U16(&bankHead[2 + 2 * k]);
				if (idx == spuIndex)
				{
					found = Ps2Demo_Read(pos, sample, sampleSize);
					break;
				}
				if (idx < numSpu)
				{
					pos += Ps2Demo_U16(&header[spuTable + 4 * idx + 2]) * 8u;
				}
			}
		}
		if (!found)
		{
			printf("[CTR PS2] ruleta demo: no encuentro el sonido %u del instrumento %u\n", (unsigned)spuIndex, (unsigned)i);
			continue;
		}
		// Se sube en multiplos de 64 bytes (lo que sobra son ceros).
		const u32 padded = (sampleSize + 63u) & ~63u;
		if (padded > sampleSize)
		{
			memset(sample + sampleSize, 0, padded - sampleSize);
		}
		Ps2Spu2_UploadRaw(spu2Addr, sample, padded);
		s_ps2DemoInstr[i].used = 1;
		s_ps2DemoInstr[i].volume = inst[1];
		s_ps2DemoInstr[i].basePitch = (u16)Ps2Demo_U16(&inst[4]);
		s_ps2DemoInstr[i].ad = (u16)Ps2Demo_U16(&inst[8]);
		s_ps2DemoInstr[i].sr = (u16)Ps2Demo_U16(&inst[10]);
		s_ps2DemoInstr[i].spu2Addr = spu2Addr;
		spu2Addr += padded;
		uploaded++;
	}
	if (uploaded == 0)
	{
		printf("[CTR PS2] ruleta demo: ningun instrumento disponible\n");
		return;
	}
	for (int v = 0; v < PS2_DEMO_ROULETTE_NUM_VOICES; v++)
	{
		s_ps2DemoVoiceTrack[v] = -1;
	}
	s_ps2DemoReady = 1;
	printf("[CTR PS2] ruleta demo: lista (cancion %d: %u pistas, %d bpm, %u bytes; %u instrumentos, %u KB en la SPU2)\n", PS2_DEMO_ROULETTE_SONG,
	       (unsigned)s_ps2DemoNumTracks, (int)bpm, (unsigned)songBytes, (unsigned)uploaded, (unsigned)((spu2Addr - PS2_SPU2_EXTRA_BASE) / 1024u));
}

internal void Ps2Demo_AllNotesOff(void)
{
	u32 bits = 0;
	for (int v = 0; v < PS2_DEMO_ROULETTE_NUM_VOICES; v++)
	{
		bits |= 1u << (PS2_DEMO_ROULETTE_FIRST_VOICE + v);
		s_ps2DemoVoiceTrack[v] = -1;
	}
	Ps2Spu2_ExtraVoiceOff(bits);
}

internal void Ps2Demo_NoteOff(int track, u32 note)
{
	u32 bits = 0;
	for (int v = 0; v < PS2_DEMO_ROULETTE_NUM_VOICES; v++)
	{
		if ((s_ps2DemoVoiceTrack[v] == track) && (s_ps2DemoVoiceNote[v] == note))
		{
			bits |= 1u << (PS2_DEMO_ROULETTE_FIRST_VOICE + v);
			s_ps2DemoVoiceTrack[v] = -1;
		}
	}
	if (bits != 0)
	{
		Ps2Spu2_ExtraVoiceOff(bits);
	}
}

internal void Ps2Demo_NoteOn(int track, u32 note, u32 velocity)
{
	struct Ps2DemoTrack *tr = &s_ps2DemoTracks[track];
	const struct Ps2DemoInstrument *inst = &s_ps2DemoInstr[tr->instrument];
	if (!inst->used || (sdata->vol_Music == 0) || (tr->volume == 0))
	{
		return;
	}
	// Voz libre (o la que lleva mas tiempo sonando).
	int voice = 0;
	for (int v = 0; v < PS2_DEMO_ROULETTE_NUM_VOICES; v++)
	{
		if (s_ps2DemoVoiceTrack[v] < 0)
		{
			voice = v;
			break;
		}
		if (s_ps2DemoVoiceAge[v] < s_ps2DemoVoiceAge[voice])
		{
			voice = v;
		}
	}
	// Tono y volumen: mismas cuentas que howl_InitChannelAttr_Music.
	const u32 pitch = howl_InstrumentPitch(inst->basePitch, (int)note, 0x80) & 0xffffu;
	u32 gain = (u32)sdata->vol_Music * PS2_DEMO_ROULETTE_SONG_VOLUME * tr->volume;
	gain = (gain >> 10) * inst->volume * velocity;
	u32 volume = gain >> 15;
	if (volume >= 0x4000)
	{
		volume = 0x3fff;
	}
	int left = (int)volume;
	int right = (int)volume;
	if (sdata->boolStereoEnabled == 1)
	{
		left = (int)((volume * GAME_VOLUME_LR[0xFF - tr->pan]) >> 8);
		right = (int)((volume * GAME_VOLUME_LR[0x00 + tr->pan]) >> 8);
	}
	Ps2Spu2_ExtraVoiceOn(PS2_DEMO_ROULETTE_FIRST_VOICE + voice, inst->spu2Addr, pitch, left, right, inst->ad, inst->sr);
	s_ps2DemoVoiceTrack[voice] = (s16)track;
	s_ps2DemoVoiceNote[voice] = (u8)note;
	s_ps2DemoVoiceAge[voice] = ++s_ps2DemoAgeClock;
}

// Al coger una caja (en vez del efecto 0x5d). 1 = suena la de la demo.
int Ps2DemoRoulette_Start(void)
{
	if (!s_ps2DemoReady)
	{
		return 0;
	}
	Ps2Demo_AllNotesOff();
	for (u32 t = 0; t < s_ps2DemoNumTracks; t++)
	{
		struct Ps2DemoTrack *tr = &s_ps2DemoTracks[t];
		int len = 0;
		tr->playing = !tr->drums;
		tr->instrument = 0;
		tr->volume = 0xff;
		tr->pan = 0x80;
		tr->elapsed = 0;
		tr->currNote = howl_GetNextNote((u8 *)tr->firstNote, &len);
		tr->noteLength = (u32)len;
	}
	s_ps2DemoTickFrac = 0;
	s_ps2DemoPlaying = 1;
	s_ps2DemoStarts++;
	return 1;
}

// Donde el juego para su efecto de ruleta (OtherFX_Stop2(0x5d)).
void Ps2DemoRoulette_Stop(void)
{
	if (!s_ps2DemoPlaying)
	{
		return;
	}
	s_ps2DemoPlaying = 0;
	Ps2Demo_AllNotesOff();
}

// Cada VBlank (ps2_platform.c). Avanza la melodia; vuelve a empezar al acabar
// (como en la demo) hasta que el juego la para.
void Ps2DemoRoulette_Tick(void)
{
	if (!s_ps2DemoPlaying)
	{
		return;
	}
	// En pausa, el juego calla sus sonidos: aqui se para.
	if ((sdata->gGT != NULL) && ((sdata->gGT->gameMode1 & PAUSE_ALL) != 0))
	{
		Ps2DemoRoulette_Stop();
		return;
	}
	s_ps2DemoTickFrac += s_ps2DemoTempo;
	const u32 ticks = s_ps2DemoTickFrac >> 16;
	s_ps2DemoTickFrac &= 0xffff;
	for (u32 t = 0; t < s_ps2DemoNumTracks; t++)
	{
		struct Ps2DemoTrack *tr = &s_ps2DemoTracks[t];
		if (!tr->playing)
		{
			continue;
		}
		tr->elapsed += ticks;
		for (int guard = 0; (guard < 64) && tr->playing && (tr->noteLength <= tr->elapsed); guard++)
		{
			tr->elapsed -= tr->noteLength;
			const u8 *note = tr->currNote;
			const u32 op = note[0];
			if (op > 10)
			{
				tr->playing = 0;
				break;
			}
			int restart = 0;
			switch (op)
			{
			case 1:
				Ps2Demo_NoteOff((int)t, note[1]);
				break;
			case 3:
				restart = 1; // fin: vuelve a empezar
				break;
			case 5:
				Ps2Demo_NoteOn((int)t, note[1], note[2]);
				break;
			case 6:
				tr->volume = note[1];
				break;
			case 7:
				tr->pan = note[1];
				break;
			case 9:
				tr->instrument = note[1];
				break;
			default:
				break; // reverb (08), distorsion (0a) y vacios: se ignoran
			}
			int len = 0;
			const u8 *next = restart ? tr->firstNote : (note + s_ps2DemoOpcodeLen[op]);
			tr->currNote = howl_GetNextNote((u8 *)next, &len);
			tr->noteLength = (u32)len;
		}
	}
}
