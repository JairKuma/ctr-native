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
// comportan igual: el analogico se activa con el boton ANALOG, como en PS1.

#include <macros.h>
#include <platform/native_input.h>
#include <psx/libpad.h>

#include <libpad.h>

#include <stdio.h>
#include <string.h>

#define PS2_INPUT_PHYSICAL_SLOT_COUNT 2
#define PS2_INPUT_PAD_DIGITAL         0x41
#define PS2_INPUT_PAD_DISCONNECT      0xff

struct Ps2InputPad
{
	int opened;
	int connected;
	u8 id;
	u8 buttons[2];
	u8 analog[4];
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

internal void Ps2Input_ReadPad(int port)
{
	struct Ps2InputPad *pad = &s_ps2Pads[port];
	struct padButtonStatus status;

	pad->connected = 0;

	if (!s_ps2PadLibReady || !pad->opened)
	{
		return;
	}

	const int state = padGetState(port, 0);
	if ((state != PAD_STATE_STABLE) && (state != PAD_STATE_FINDCTP1))
	{
		return;
	}

	memset(&status, 0, sizeof(status));
	if (padRead(port, 0, &status) == 0)
	{
		return;
	}

	const u16 buttons = status.btns;

	pad->connected = 1;
	pad->id = (status.mode != 0) ? status.mode : PS2_INPUT_PAD_DIGITAL;
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
	// Vibracion: pendiente (PadInfoAct devuelve 0 motores, asi que el juego no la pide).
	(void)port;
	(void)table;
	(void)len;
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
