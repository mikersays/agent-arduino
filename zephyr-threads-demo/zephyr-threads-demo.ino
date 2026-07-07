/*
 * zephyr-threads-demo.ino — Real RTOS threads inside an Arduino sketch
 * ---------------------------------------------------------------------
 * WHAT THIS DEMONSTRATES
 *   The Arduino UNO Q's MCU core (STM32U585, Cortex-M33 @160MHz) runs a
 *   ZEPHYR RTOS underneath the Arduino API, and sketches are compiled as
 *   Zephyr "llext" loadable extensions. That means a plain .ino sketch can
 *   use REAL kernel primitives — k_thread_create(), k_mutex, k_msleep(),
 *   k_uptime_get() — something no classic AVR/SAMD Arduino can do.
 *
 *   This sketch is the canonical producer/consumer pattern, taught live on
 *   the built-in 8x13 LED matrix (no external hardware needed):
 *
 *     Thread A ("bouncer",  ~20 Hz) --\                       /--> matrix.draw()
 *                                      +--> shared 104-byte --+
 *     Thread B ("breather", ~5 Hz) ---/     frame buffer,     |
 *                                           guarded by a      +--> loop() is the
 *                                           k_mutex                COMPOSITOR @ ~30fps
 *
 *   - Thread A animates the LEFT half (cols 0..5): a dot bouncing off walls.
 *   - Thread B animates the RIGHT half (cols 7..12): a slow breathing
 *     brightness gradient. Column 6 stays dark as a visual divider.
 *   - Each thread locks the mutex ONLY while writing its half, then sleeps.
 *   - loop() locks the mutex just long enough to memcpy a snapshot, unlocks,
 *     and then calls matrix.draw() OUTSIDE the critical section. Keeping
 *     critical sections tiny is the whole art of cooperative mutex use.
 *
 *   WHY THE LOCK MATTERS: matrix.draw() streams all 104 bytes to the LED
 *   driver. Without the mutex, a thread could be preempted mid-update and
 *   the compositor would ship a half-written frame — "tearing". The mutex
 *   guarantees every displayed frame is internally consistent.
 *
 *   Extra visible proof that two independent threads are alive:
 *     - RGB LED3 toggles green on every Thread A iteration (fast heartbeat)
 *     - RGB LED4 toggles blue  on every Thread B iteration (slow heartbeat)
 *     - Every 5 s, loop() prints k_uptime_get() plus per-thread iteration
 *       counters over Serial (115200 baud).
 *
 * BUILD / DEPLOY (compile from the sketch's parent directory):
 *   arduino-cli compile -b arduino:zephyr:unoq ./zephyr-threads-demo
 *   arduino-cli upload  -b arduino:zephyr:unoq -p <board-ip> ./zephyr-threads-demo
 */

#include "Arduino_LED_Matrix.h"
#include <zephyr/kernel.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Matrix geometry                                                     */
/* ------------------------------------------------------------------ */
constexpr int WIDTH      = 13;              // columns
constexpr int HEIGHT     = 8;               // rows
constexpr int N_PIX      = WIDTH * HEIGHT;  // 104 bytes, row-major, row 0 = top
constexpr int SPLIT_COL  = 6;               // divider column (left = 0..5, right = 7..12)
constexpr uint8_t MAX_BRIGHT = 7;           // 3-bit grayscale: 0 (off) .. 7 (max)

Arduino_LED_Matrix matrix;

/* ------------------------------------------------------------------ */
/* Shared state: THE frame buffer + its mutex                          */
/* ------------------------------------------------------------------ */
/* K_MUTEX_DEFINE statically defines and initializes a kernel mutex at
 * load time — no runtime init call needed. Zephyr mutexes support
 * priority inheritance, so a low-priority thread holding the lock gets
 * temporarily boosted if a higher-priority thread is waiting on it.    */
K_MUTEX_DEFINE(frame_mutex);

/* The one true frame buffer. Producers write halves, compositor reads. */
static uint8_t frame[N_PIX];

/* Per-thread iteration counters for the status line. Each is written by
 * exactly one thread and only read (racily but harmlessly, it's just a
 * status print) by loop(), so they don't need the mutex.               */
static volatile uint32_t iters_a = 0;
static volatile uint32_t iters_b = 0;

/* ------------------------------------------------------------------ */
/* RGB heartbeat LEDs                                                  */
/* ------------------------------------------------------------------ */
/* Like LED_BUILTIN, the RGB LEDs are active-low: LOW = lit. Keep the
 * polarity behind constants so this reads sanely below.               */
constexpr int RGB_ON  = LOW;
constexpr int RGB_OFF = HIGH;

/* ------------------------------------------------------------------ */
/* Thread plumbing                                                     */
/* ------------------------------------------------------------------ */
/* Each Zephyr thread needs (1) a stack and (2) a `struct k_thread`
 * control block. Both must live for the thread's whole life, so they
 * are file-scope statics. K_KERNEL_STACK_DEFINE handles the alignment
 * and guard-region bookkeeping the kernel requires — never use a plain
 * byte array as a thread stack. 1 KiB is plenty: the animators keep
 * everything in a few locals and call only tiny kernel APIs.           */
constexpr size_t STACK_SIZE = 1024;
K_KERNEL_STACK_DEFINE(stack_a, STACK_SIZE);
K_KERNEL_STACK_DEFINE(stack_b, STACK_SIZE);
static struct k_thread thread_a;
static struct k_thread thread_b;

/* Priorities: K_PRIO_PREEMPT(n) — LOWER n = HIGHER priority. Both
 * animators sit at the same priority so neither starves the other, and
 * both sleep almost all the time anyway. loop() runs in the Arduino
 * main thread, which the core schedules for us.                        */
constexpr int ANIM_PRIO = 10;

/* ------------------------------------------------------------------ */
/* Thread A — bouncing dot, LEFT half, ~20 Hz                          */
/* ------------------------------------------------------------------ */
/* Zephyr entry points take three opaque void* args (we don't need
 * them). The thread body is the classic RTOS shape:
 *     forever { compute -> lock -> write shared state -> unlock -> sleep }
 * All the trigonometry-free "physics" happens BEFORE taking the lock;
 * the critical section is nothing but stores into frame[].             */
static void bouncer_entry(void *, void *, void *)
{
	pinMode(LED3_G, OUTPUT);
	digitalWrite(LED3_G, RGB_OFF);

	int x = 1, y = 2;    // dot position within cols 0..SPLIT_COL-1
	int dx = 1, dy = 1;  // velocity: one cell per tick, reflects off walls

	bool led_on = false;

	while (true) {
		/* --- physics (no lock needed: all locals) ------------------ */
		x += dx;
		y += dy;
		if (x <= 0)             { x = 0;             dx = 1;  }
		if (x >= SPLIT_COL - 1) { x = SPLIT_COL - 1; dx = -1; }
		if (y <= 0)             { y = 0;             dy = 1;  }
		if (y >= HEIGHT - 1)    { y = HEIGHT - 1;    dy = -1; }

		/* --- critical section: repaint ONLY our half --------------- */
		/* K_FOREVER = block until we get the lock. In a hard-real-time
		 * design you'd pass a timeout and handle failure; for a demo,
		 * waiting is correct and shows lock contention resolving.     */
		k_mutex_lock(&frame_mutex, K_FOREVER);
		for (int r = 0; r < HEIGHT; r++) {
			for (int c = 0; c < SPLIT_COL; c++) {
				frame[r * WIDTH + c] = 0;          // clear left half
			}
		}
		frame[y * WIDTH + x] = MAX_BRIGHT;         // draw the dot
		k_mutex_unlock(&frame_mutex);

		/* --- heartbeat + bookkeeping ------------------------------- */
		led_on = !led_on;
		digitalWrite(LED3_G, led_on ? RGB_ON : RGB_OFF);
		iters_a++;

		k_msleep(50);   // ~20 Hz. k_msleep yields the CPU — this thread
		                // costs nothing while it sleeps.
	}
}

/* ------------------------------------------------------------------ */
/* Thread B — breathing gradient, RIGHT half, ~5 Hz                    */
/* ------------------------------------------------------------------ */
/* Same skeleton as Thread A, different rate and different pixels —
 * proving two producers with unrelated timing can share one buffer
 * safely as long as both respect the same mutex.                       */
static void breather_entry(void *, void *, void *)
{
	pinMode(LED4_B, OUTPUT);
	digitalWrite(LED4_B, RGB_OFF);

	/* Breathing = brightness ramps 0..MAX_BRIGHT..0, triangle wave.
	 * phase runs 0..2*MAX_BRIGHT-1 and folds down the middle.          */
	int phase = 0;
	constexpr int PHASE_MAX = 2 * MAX_BRIGHT;   // 14 steps per breath

	bool led_on = false;

	while (true) {
		/* Triangle fold: 0,1,..,7,6,..,1 then repeat (~2.8 s/breath). */
		int level = (phase <= MAX_BRIGHT) ? phase : PHASE_MAX - phase;

		k_mutex_lock(&frame_mutex, K_FOREVER);
		for (int r = 0; r < HEIGHT; r++) {
			for (int c = SPLIT_COL + 1; c < WIDTH; c++) {
				/* Horizontal gradient: dimmest at the divider, the
				 * breathing `level` sets the ceiling. Integer scale
				 * keeps everything in 0..7 for 3-bit grayscale.       */
				int g = (level * (c - SPLIT_COL)) / (WIDTH - 1 - SPLIT_COL);
				frame[r * WIDTH + c] = (uint8_t)g;
			}
		}
		k_mutex_unlock(&frame_mutex);

		phase = (phase + 1) % PHASE_MAX;

		led_on = !led_on;
		digitalWrite(LED4_B, led_on ? RGB_ON : RGB_OFF);
		iters_b++;

		k_msleep(200);  // ~5 Hz — a quarter of Thread A's rate, on purpose:
		                // watching the halves update at visibly different
		                // speeds is the "two independent threads" payoff.
	}
}

/* ------------------------------------------------------------------ */
/* setup() — bring up peripherals, then spawn the producers            */
/* ------------------------------------------------------------------ */
void setup()
{
	Serial.begin(115200);

	matrix.begin();
	matrix.setGrayscaleBits(3);   // frame bytes are 0..7
	memset(frame, 0, sizeof(frame));

	/* k_thread_create wires a stack + control block + entry function
	 * into a live, scheduled thread. Arguments, in order:
	 *   control block, stack, stack size (via the SIZEOF macro, which
	 *   accounts for guard overhead), entry fn, three void* user args,
	 *   priority, options, start delay (K_NO_WAIT = run immediately).
	 * This is the same call the core's own CAN library uses, so it is
	 * fully supported from llext sketch code.                          */
	k_thread_create(&thread_a, stack_a, K_KERNEL_STACK_SIZEOF(stack_a),
	                bouncer_entry, nullptr, nullptr, nullptr,
	                K_PRIO_PREEMPT(ANIM_PRIO), 0, K_NO_WAIT);
	k_thread_name_set(&thread_a, "bouncer");

	k_thread_create(&thread_b, stack_b, K_KERNEL_STACK_SIZEOF(stack_b),
	                breather_entry, nullptr, nullptr, nullptr,
	                K_PRIO_PREEMPT(ANIM_PRIO), 0, K_NO_WAIT);
	k_thread_name_set(&thread_b, "breather");

	Serial.println("zephyr-threads-demo: 2 producer threads + compositor loop() running");
}

/* ------------------------------------------------------------------ */
/* loop() — the COMPOSITOR (~30 fps) + 5 s status line                 */
/* ------------------------------------------------------------------ */
void loop()
{
	/* Snapshot under the lock, draw outside it. matrix.draw() takes
	 * real time (it pushes 104 grayscale values to the driver); holding
	 * the mutex across it would stall both producers for no reason.
	 * The memcpy critical section is ~104 byte copies — microseconds.  */
	static uint8_t snapshot[N_PIX];

	k_mutex_lock(&frame_mutex, K_FOREVER);
	memcpy(snapshot, frame, sizeof(snapshot));
	k_mutex_unlock(&frame_mutex);

	matrix.draw(snapshot);   // exactly 104 bytes, row-major, 0..7

	/* Once every 5 s: kernel uptime + per-thread liveness counters.
	 * k_uptime_get() returns milliseconds since boot as int64_t.       */
	static int64_t next_status_ms = 0;
	int64_t now = k_uptime_get();
	if (now >= next_status_ms) {
		next_status_ms = now + 5000;
		Serial.print("[status] uptime_ms=");
		Serial.print((long)now);
		Serial.print(" bouncer_iters=");
		Serial.print((unsigned long)iters_a);
		Serial.print(" breather_iters=");
		Serial.println((unsigned long)iters_b);
	}

	k_msleep(33);            // ~30 fps compositor cadence
}
