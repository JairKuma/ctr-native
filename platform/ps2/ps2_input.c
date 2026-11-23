// PS2 port - input backend con libpadx (rom0:XSIO2MAN + rom0:XPADMAN).
//
// Implementa la API de include/platform/native_input.h.
//
// El mando de PS2 entrega los datos en el mismo formato que el de PS1:
//   - byte de ID: 0x41 digital, 0x73 analogico (DualShock 1 y DualShock 2),
//   - 2 bytes de botones activos a nivel bajo con el mismo orden de bits,
//   - 4 bytes de sticks en el mismo orden (derecho X/Y, izquierdo X/Y).
// Asi que se copian tal cual al paquete que lee el juego. No se activa el modo
// de botones sensibles a la presion, de modo que DualShock 1 y DualShock 2 se
// comportan igual. Vibracion de los dos motores del DualShock. Al conectar el mando se enciende el modo analogico (sin
// bloquearlo: el boton ANALOG lo puede apagar).

#include <macros.h>
#include <platform/native_input.h>
#include <psx/libpad.h>

#include <libpad.h>

#include <stdio.h>
#include <string.h>

#define PS2_INPUT_PHYSICAL_SLOT_COUNT 2
#define PS2_INPUT_PAD_DIGITAL         0x41
#define PS2_INPUT_PAD_DISCONNECT      0xff
#define PS2_INPUT_PAD_ANALOG          0x73
// Frames que se mantiene el mando como "conectado" mientras cambia de modo
// (boton ANALOG): durante el cambio libpad no deja leer y el juego mostraria
// "controller unplugged" un segundo.
#define PS2_INPUT_GRACE_FRAMES        90
// Preparacion de los motores: intentos por conexion/cambio de modo y frames
// maximos esperando la respuesta (asi nunca se queda repitiendo la orden).
#define PS2_INPUT_ALIGN_MAX_TRIES     3
#define PS2_INPUT_ALIGN_MAX_WAIT      120

struct Ps2InputPad
{
	int opened;
	int connected;
	u8 id;
	u8 buttons[2];
	u8 analog[4];
	int graceFrames;   // frames restantes de "conectado" sin poder leer
	int modeStep;      // 0 = pedir modo analogico, 1 = esperando, 2 = hecho
	int modeTries;     // intentos esperando a que el mando diga sus modos
	int verifyLeft;    // comprobaciones que quedan de que quedo en analogico
	// Vibracion (DualShock): el juego da con PadSetAct un buffer de 2 bytes
	// (motor pequeno on/off, motor grande 0-255) que rellena cada fotograma;
	// la libpad de PS1 lo enviaba sola en cada lectura del mando.
	const unsigned char *actTable;
	int actLen;
	int alignStep;     // 0 = pedir motores, 1 = esperando, 2 = listos
	int alignTries;    // intentos fallidos desde el ultimo cambio de modo
	int alignWait;     // frames esperando la respuesta de padSetActAlign
	int lastModeId;    // modo del mando (PAD_MODECURID) visto la ultima vez
	u8 actSent[2];
	int actSentValid;
};

// libpad necesita 256 bytes alineados a 64 por puerto.
global_variable u8 s_ps2PadArea[PS2_INPUT_PHYSICAL_SLOT_COUNT][256] __attribute__((aligned(64)));
global_variable struct Ps2InputPad s_ps2Pads[PS2_INPUT_PHYSICAL_SLOT_COUNT];
global_variable unsigned char *s_ps2PadSlotData[PS2_INPUT_PHYSICAL_SLOT_COUNT];
global_variable int s_ps2PadLibReady = 0;

internal void Ps2Input_WritePacket(unsigned char *dst, const struct Ps2InputPad *pad)
{
	if (dst == NULL)
	{
		return;
	}

	if ((pad == NULL) || !pad->connected)
	{
		dst[0] = PS2_INPUT_PAD_DISCONNECT;
		dst[1] = PS2_INPUT_PAD_DISCONNECT;
		dst[2] = 0xff;
		dst[3] = 0xff;
		dst[4] = 0x80;
		dst[5] = 0x80;
		dst[6] = 0x80;
		dst[7] = 0x80;
		return;
	}

	dst[0] = 0x00;
	dst[1] = pad->id;
	dst[2] = pad->buttons[0];
	dst[3] = pad->buttons[1];
	dst[4] = pad->analog[0];
	dst[5] = pad->analog[1];
	dst[6] = pad->analog[2];
	dst[7] = pad->analog[3];
}

internal void Ps2Input_WriteBus(void)
{
	for (int slot = 0; slot < PS2_INPUT_PHYSICAL_SLOT_COUNT; slot++)
	{
		Ps2Input_WritePacket(s_ps2PadSlotData[slot], &s_ps2Pads[slot]);
	}
}

internal void Ps2Input_KeepDuringTransition(struct Ps2InputPad *pad)
{
	// Sin datos nuevos: si hace poco estaba conectado, se mantiene con los
	// botones sueltos y los sticks al centro (no salta el aviso de desconexion).
	if (pad->graceFrames > 0)
	{
		pad->graceFrames--;
		pad->connected = 1;
		pad->buttons[0] = 0xff;
		pad->buttons[1] = 0xff;
		pad->analog[0] = 0x80;
		pad->analog[1] = 0x80;
		pad->analog[2] = 0x80;
		pad->analog[3] = 0x80;
	}
	else
	{
		pad->connected = 0;
	}
}

internal void Ps2Input_TrySetAnalog(int port, struct Ps2InputPad *pad)
{
	// Al conectar el mando se enciende el modo analogico (sin bloquearlo: el
	// boton ANALOG sigue funcionando), igual que hacen los juegos de PS2.
	if (pad->modeStep == 0)
	{
		const int modes = padInfoMode(port, 0, PAD_MODETABLE, -1);
		int hasDualShock = 0;
		for (int i = 0; i < modes; i++)
		{
			if (padInfoMode(port, 0, PAD_MODETABLE, i) == PAD_TYPE_DUALSHOCK)
			{
				hasDualShock = 1;
			}
		}
		if (modes <= 0)
		{
			// Recien conectado: el mando aun no ha dicho sus modos. Reintentar
			// (antes se daba por "mando digital" y el analogico no se encendia).
			if (++pad->modeTries < 600)
			{
				return;
			}
		}
		if (!hasDualShock)
		{
			pad->modeStep = 2; // mando digital: nada que hacer
			return;
		}
		if (padInfoMode(port, 0, PAD_MODECURID, 0) == PAD_TYPE_DUALSHOCK)
		{
			pad->modeStep = 2; // ya esta en analogico
			return;
		}
		if (padSetMainMode(port, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_UNLOCK) == 1)
		{
			pad->modeStep = 1;
			pad->graceFrames = PS2_INPUT_GRACE_FRAMES;
		}
	}
	else if (pad->modeStep == 1)
	{
		const int req = padGetReqState(port, 0);
		if (req == PAD_RSTAT_COMPLETE)
		{
			pad->modeStep = 2;
		}
		else if (req == PAD_RSTAT_FAILED)
		{
			pad->modeStep = 0; // reintentar
		}
	}
	else if ((pad->modeStep == 2) && (pad->verifyLeft > 0))
	{
		// Comprobar que de verdad quedo en analogico (algunos mandos ignoran la
		// primera orden justo al conectarse).
		const int cur = padInfoMode(port, 0, PAD_MODECURID, 0);
		pad->verifyLeft--;
		if ((cur != 0) && (cur != PAD_TYPE_DUALSHOCK) && (padInfoMode(port, 0, PAD_MODETABLE, -1) > 0))
		{
			pad->modeStep = 0; // no quedo en analogico: volver a pedirlo
		}
		else
		{
			pad->verifyLeft = 0; // ya comprobado
		}
	}
}

// Prepara los motores del DualShock (una vez por conexion o cambio de modo).
// Solo en modo analogico: en digital no se mandan ordenes de configuracion al
// mando (antes, al apagar ANALOG, la orden se repetia sin fin, el mando se
// quedaba ocupado casi siempre y los botones iban a tirones o no iban).
internal void Ps2Input_PrepareMotors(int port, struct Ps2InputPad *pad)
{
	if (pad->alignStep == 0)
	{
		if (pad->alignTries >= PS2_INPUT_ALIGN_MAX_TRIES)
		{
			return; // el mando no tiene motores o no acepta la orden
		}
		if (padInfoAct(port, 0, -1, 0) <= 0)
		{
			pad->alignTries++;
			return;
		}
		static const char align[6] = {0, 1, (char)0xff, (char)0xff, (char)0xff, (char)0xff};
		pad->alignTries++; // cada orden enviada cuenta: como mucho 3 por cambio de modo
		if (padSetActAlign(port, 0, align) == 1)
		{
			pad->alignStep = 1;
			pad->alignWait = 0;
		}
	}
	else if (pad->alignStep == 1)
	{
		const int req = padGetReqState(port, 0);
		if (req == PAD_RSTAT_COMPLETE)
		{
			pad->alignStep = 2;
			pad->actSentValid = 0;
			printf("[CTR PS2] Pad %d: motores listos\n", port + 1);
		}
		else if ((req == PAD_RSTAT_FAILED) || (++pad->alignWait > PS2_INPUT_ALIGN_MAX_WAIT))
		{
			pad->alignStep = 0;
			printf("[CTR PS2] Pad %d: motores no preparados (intento %d)\n", port + 1, pad->alignTries);
		}
	}
}

// Manda al mando lo que el juego ha puesto en su buffer de vibracion.
internal void Ps2Input_SendMotors(int port, struct Ps2InputPad *pad)
{
	if (!pad->connected || (pad->alignStep != 2))
	{
		return;
	}
	u8 small = 0;
	u8 big = 0;
	if ((pad->actTable != NULL) && (pad->actLen > 0))
	{
		small = (pad->actTable[0] != 0) ? 1 : 0;
		big = (pad->actLen > 1) ? pad->actTable[1] : 0;
	}
	if (pad->actSentValid && (pad->actSent[0] == small) && (pad->actSent[1] == big))
	{
		return;
	}
	char act[6] = {(char)small, (char)big, 0, 0, 0, 0};
	if (padSetActDirect(port, 0, act) == 1)
	{
		pad->actSent[0] = small;
		pad->actSent[1] = big;
		pad->actSentValid = 1;
	}
}

// Para native_libpad.c: lo que la libpad de PS1 respondia sobre los motores.
int Ps2Input_PadInfoAct(int id, int acno, int term)
{
	const int slot = (id >> 4) & 1;
	if (((id & 3) != 0) || !s_ps2PadLibReady || !s_ps2Pads[slot].connected)
	{
		return 0;
	}
	if (acno == -1)
	{
		return padInfoAct(slot, 0, -1, 0);
	}
	return padInfoAct(slot, 0, acno, term);
}

int Ps2Input_PadSetMainMode(int id)
{
	// El juego pide el modo analogico antes de usar los motores (en PS1
	// devuelve 1 si se pudo). Aqui el modo ya lo gestiona el port.
	const int slot = (id >> 4) & 1;
	return ((id & 3) == 0) && s_ps2Pads[slot].connected;
}

internal void Ps2Input_ReadPad(int port)
{
	struct Ps2InputPad *pad = &s_ps2Pads[port];
	struct padButtonStatus status;

	if (!s_ps2PadLibReady || !pad->opened)
	{
		pad->connected = 0;
		return;
	}

	const int state = padGetState(port, 0);
	if (state == PAD_STATE_DISCONN)
	{
		// Mando quitado de verdad: la proxima vez se vuelve a poner en analogico
		// y se vuelven a preparar los motores.
		pad->modeStep = 0;
		pad->modeTries = 0;
		pad->verifyLeft = 3;
		pad->alignStep = 0;
		pad->alignTries = 0;
		pad->alignWait = 0;
		pad->actSentValid = 0;
		pad->lastModeId = 0;
		Ps2Input_KeepDuringTransition(pad);
		return;
	}
	if ((state != PAD_STATE_STABLE) && (state != PAD_STATE_FINDCTP1))
	{
		Ps2Input_KeepDuringTransition(pad);
		return;
	}

	if ((state == PAD_STATE_STABLE) && ((pad->modeStep != 2) || (pad->verifyLeft > 0)))
	{
		Ps2Input_TrySetAnalog(port, pad);
	}
	else if ((state == PAD_STATE_STABLE) && (pad->modeStep == 2))
	{
		// Boton ANALOG: si el modo cambia, los motores se preparan de nuevo
		// (solo una vez, y solo en analogico).
		const int cur = padInfoMode(port, 0, PAD_MODECURID, 0);
		if ((cur != 0) && (cur != pad->lastModeId))
		{
			printf("[CTR PS2] Pad %d: modo %d -> %d\n", port + 1, pad->lastModeId, cur);
			pad->lastModeId = cur;
			if (pad->alignStep != 1) // si hay una orden en curso, se deja terminar
			{
				pad->alignStep = 0;
			}
			pad->alignTries = 0;
			pad->actSentValid = 0;
		}
		if (cur == PAD_TYPE_DUALSHOCK)
		{
			Ps2Input_PrepareMotors(port, pad);
		}
		else if (pad->alignStep == 2)
		{
			pad->alignStep = 0; // en digital no se manda vibracion
		}
	}

	memset(&status, 0, sizeof(status));
	if (padRead(port, 0, &status) == 0)
	{
		Ps2Input_KeepDuringTransition(pad);
		return;
	}

	const u16 buttons = status.btns;
	u8 id = (status.mode != 0) ? status.mode : PS2_INPUT_PAD_DIGITAL;

	// 0x79 = DualShock 2 con botones de presion: para el juego es un DualShock.
	if (id == 0x79)
	{
		id = PS2_INPUT_PAD_ANALOG;
	}

	pad->connected = 1;
	pad->graceFrames = PS2_INPUT_GRACE_FRAMES;
	pad->id = id;
	pad->buttons[0] = (u8)(buttons & 0xff);
	pad->buttons[1] = (u8)(buttons >> 8);
	pad->analog[0] = status.rjoy_h;
	pad->analog[1] = status.rjoy_v;
	pad->analog[2] = status.ljoy_h;
	pad->analog[3] = status.ljoy_v;
}

int Platform_InputInit(void)
{
	memset(s_ps2Pads, 0, sizeof(s_ps2Pads));
	memset(s_ps2PadSlotData, 0, sizeof(s_ps2PadSlotData));

	if (padInit(0) != 1)
	{
		printf("[CTR PS2] padInit failed\n");
		s_ps2PadLibReady = 0;
		return 0;
	}

	s_ps2PadLibReady = 1;

	for (int port = 0; port < PS2_INPUT_PHYSICAL_SLOT_COUNT; port++)
	{
		s_ps2Pads[port].opened = (padPortOpen(port, 0, s_ps2PadArea[port]) != 0);
		s_ps2Pads[port].verifyLeft = 3;
		printf("[CTR PS2] Pad port %d: %s\n", port + 1, s_ps2Pads[port].opened ? "open" : "FAILED");
	}

	return 1;
}

void Platform_InputShutdown(void)
{
	if (!s_ps2PadLibReady)
	{
		return;
	}

	for (int port = 0; port < PS2_INPUT_PHYSICAL_SLOT_COUNT; port++)
	{
		if (s_ps2Pads[port].opened)
		{
			padPortClose(port, 0);
			s_ps2Pads[port].opened = 0;
		}
	}

	padEnd();
	s_ps2PadLibReady = 0;
}

void Platform_InputUpdate(void)
{
	for (int port = 0; port < PS2_INPUT_PHYSICAL_SLOT_COUNT; port++)
	{
		Ps2Input_ReadPad(port);
		Ps2Input_SendMotors(port, &s_ps2Pads[port]);
	}

	Ps2Input_WriteBus();
}

void Platform_InputControllerAdded(int deviceIndex)
{
	(void)deviceIndex;
}

void Platform_InputControllerRemoved(int instanceId)
{
	(void)instanceId;
}

int Platform_InputCycleKeyboardController(void)
{
	return 0;
}

int Platform_InputCycleGamepadController(void)
{
	return 0;
}

void Platform_InputPadInit(int slot, unsigned char *padData)
{
	if ((slot < 0) || (slot >= PS2_INPUT_PHYSICAL_SLOT_COUNT))
	{
		return;
	}

	s_ps2PadSlotData[slot] = padData;
	Ps2Input_WritePacket(padData, &s_ps2Pads[slot]);
}

int Platform_InputPadGetState(int port)
{
	const int physicalSlot = (port >> 4) & 1;
	const int tap = port & 3;

	// Sin multitap de momento: solo el mando directo de cada puerto.
	if ((tap != 0) || (physicalSlot >= PS2_INPUT_PHYSICAL_SLOT_COUNT))
	{
		return PadStateDiscon;
	}

	return s_ps2Pads[physicalSlot].connected ? PadStateStable : PadStateDiscon;
}

void Platform_InputPadVibrate(int port, unsigned char *table, int len)
{
	// PadSetAct: se guarda el buffer; se lee y se envia en cada actualizacion.
	const int slot = (port >> 4) & 1;
	if ((port & 3) != 0)
	{
		return;
	}
	s_ps2Pads[slot].actTable = table;
	s_ps2Pads[slot].actLen = len;
	s_ps2Pads[slot].actSentValid = 0;
}

int Platform_InputCapturePadSnapshots(struct PlatformInputPadSnapshot *dst, int count)
{
	(void)dst;
	(void)count;
	return 0;
}

int Platform_InputInstallPadSnapshots(const struct PlatformInputPadSnapshot *src, int count)
{
	(void)src;
	(void)count;
	return 0;
}

void Platform_InputClearInstalledPadSnapshots(void)
{
}

int Platform_InputGetStateSize(void)
{
	return 0;
}

int Platform_InputCaptureState(void *dst, int dstSize)
{
	(void)dst;
	(void)dstSize;
	return 0;
}

int Platform_InputRestoreState(const void *src, int srcSize)
{
	(void)src;
	(void)srcSize;
	return 0;
}
