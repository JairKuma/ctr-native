// PS2 port - input backend (fase 1: mando conectado sin botones pulsados).
//
// Implementa la API de include/platform/native_input.h. En la fase 1 el mando 1
// aparece conectado (digital) y sin botones pulsados, y el resto desconectados,
// para que el juego no muestre el aviso de "mando desconectado" y entre en la
// demo. En la fase 2 se sustituye por padman (DualShock 2).

#include <macros.h>
#include <platform/native_input.h>
#include <psx/libpad.h>

#include <string.h>

#define PS2_INPUT_PHYSICAL_SLOT_COUNT 2
#define PS2_INPUT_PAD_DIGITAL         0x41
#define PS2_INPUT_PAD_DISCONNECT      0xff

global_variable unsigned char *s_ps2PadSlotData[PS2_INPUT_PHYSICAL_SLOT_COUNT];

internal void Ps2Input_WritePacket(unsigned char *dst, int connected)
{
	if (dst == NULL)
	{
		return;
	}

	dst[0] = connected ? 0x00 : PS2_INPUT_PAD_DISCONNECT;
	dst[1] = connected ? PS2_INPUT_PAD_DIGITAL : PS2_INPUT_PAD_DISCONNECT;
	dst[2] = 0xff; // botones activos a nivel bajo: 0xff = nada pulsado
	dst[3] = 0xff;
	dst[4] = 0x80;
	dst[5] = 0x80;
	dst[6] = 0x80;
	dst[7] = 0x80;
}

internal void Ps2Input_WriteBus(void)
{
	Ps2Input_WritePacket(s_ps2PadSlotData[0], 1);
	Ps2Input_WritePacket(s_ps2PadSlotData[1], 0);
}

int Platform_InputInit(void)
{
	memset(s_ps2PadSlotData, 0, sizeof(s_ps2PadSlotData));
	return 1;
}

void Platform_InputShutdown(void)
{
}

void Platform_InputUpdate(void)
{
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
	Ps2Input_WriteBus();
}

int Platform_InputPadGetState(int port)
{
	const int physicalSlot = (port >> 4) & 1;
	const int tap = port & 3;

	if ((physicalSlot == 0) && (tap == 0))
	{
		return PadStateStable;
	}

	return PadStateDiscon;
}

void Platform_InputPadVibrate(int port, unsigned char *table, int len)
{
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
