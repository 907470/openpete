/* probe.c - rumble probe, a PS-EXE run by the reference emulator.
 *
 * The probe talks to the port 1 controller over SIO0 by polling, with
 * interrupts off, and sends every actuator byte pair through the DualShock
 * protocol a game uses:
 *
 *   01 43 00 01 00 ...           enter configuration mode
 *   01 44 00 01 03 00 00 00 00   analog mode, mode button locked
 *   01 4D 00 00 01 FF FF FF FF   actuator byte 0 drives the small motor,
 *                                byte 1 the large motor
 *   01 43 00 00 00 00 00 00 00   leave configuration mode
 *   01 42 00 SS LL 00 00 00 00   poll, small motor byte SS, large motor LL
 *
 * Polls, in order: (0,1) (0,0) (1,0) (0,0), so each motor's zero is sent
 * once while the other is zero; then every pair with the small byte in the
 * outer loop (the large byte changes on every poll), then every pair with
 * the large byte in the outer loop (the small byte changes on every poll);
 * then (0,0), after which the probe idles. The emulator's motor log
 * (regtest -motor-log) records each change of a motor's state with the
 * other motor's state, so the two sweeps give every (byte, other byte)
 * combination for each motor.
 *
 * Register addresses and bits: psx-spx, "Controllers and Memory Cards". */
#include <stdint.h>

typedef volatile uint8_t  vu8;
typedef volatile uint16_t vu16;

#define JOY_DATA (*(vu8*)0x1F801040u)
#define JOY_STAT (*(vu16*)0x1F801044u)
#define JOY_MODE (*(vu16*)0x1F801048u)
#define JOY_CTRL (*(vu16*)0x1F80104Au)
#define JOY_BAUD (*(vu16*)0x1F80104Eu)

#define STAT_TX_READY 0x0001u
#define STAT_RX_READY 0x0002u
#define STAT_IRQ      0x0200u
#define CTRL_SELECT   0x1003u   /* TX enable, /JOYn on port 1, ACK interrupt enable */
#define CTRL_ACK      0x0010u
#define CTRL_RESET    0x0040u

/** @brief Spin for about n loop iterations. */
static void delay(uint32_t n)
{
	while (n--)
		__asm__ volatile("nop");
}

/** @brief Exchange one byte; wait for the controller's acknowledge unless last. */
static uint8_t xfer(uint8_t out, int last)
{
	uint32_t t;
	uint8_t in;
	for (t = 0; t < 100000u && !(JOY_STAT & STAT_TX_READY); t++)
		;
	JOY_DATA = out;
	for (t = 0; t < 100000u && !(JOY_STAT & STAT_RX_READY); t++)
		;
	in = JOY_DATA;
	if (!last)
	{
		for (t = 0; t < 20000u && !(JOY_STAT & STAT_IRQ); t++)
			;
	}
	JOY_CTRL = CTRL_SELECT | CTRL_ACK;
	return in;
}

/** @brief Send one command to the port 1 controller. */
static void command(const uint8_t* tx, int n)
{
	int i;
	JOY_CTRL = CTRL_SELECT;
	delay(200);
	for (i = 0; i < n; i++)
		(void)xfer(tx[i], i == n - 1);
	JOY_CTRL = 0;
	delay(400);
}

/** @brief Poll the controller with one actuator byte pair. */
static void poll(uint8_t small, uint8_t large)
{
	uint8_t tx[9] = {0x01, 0x42, 0x00, small, large, 0x00, 0x00, 0x00, 0x00};
	command(tx, 9);
}

int main(void)
{
	static const uint8_t enter[9] = {0x01, 0x43, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
	static const uint8_t analog[9] = {0x01, 0x44, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00};
	static const uint8_t map[9] = {0x01, 0x4D, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF};
	static const uint8_t leave[9] = {0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	uint32_t a, b;

	JOY_CTRL = CTRL_RESET;
	JOY_MODE = 0x000D;
	JOY_BAUD = 0x0088;
	JOY_CTRL = 0;
	delay(20000);

	poll(0, 0);
	command(enter, 9);
	command(analog, 9);
	command(map, 9);
	command(leave, 9);

	poll(0, 1);
	poll(0, 0);
	poll(1, 0);
	poll(0, 0);
	for (a = 0; a < 256; a++)
		for (b = 0; b < 256; b++)
			poll((uint8_t)a, (uint8_t)b);
	for (a = 0; a < 256; a++)
		for (b = 0; b < 256; b++)
			poll((uint8_t)b, (uint8_t)a);
	poll(0, 0);
	for (;;)
		;
}
