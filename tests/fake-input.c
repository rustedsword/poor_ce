/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/uinput.h>
#include <poor_array.h>
#include <poor_stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/*
 * Keyboard and pointer input of a virtual device, as a person would give it, to try the editor on a desktop. The
 * compositor maps the absolute pointer to the whole desktop, whose size in logical pixels it needs. Positions are
 * relative to an origin and divided by a scale, so that they can come from a screenshot of a window.
 */
constexpr uint32_t SETTLE_MS = 700;
constexpr uint32_t STEP_MS = 30;
constexpr int32_t ABS_RANGE = 65535;

struct key {
	const char *name;
	uint16_t code;
};

static const struct key keys[] = {
	{ "esc", KEY_ESC }, { "f1", KEY_F1 }, { "f2", KEY_F2 }, { "f3", KEY_F3 }, { "f4", KEY_F4 }, { "f5", KEY_F5 },
	{ "f6", KEY_F6 }, { "f7", KEY_F7 }, { "f8", KEY_F8 }, { "f9", KEY_F9 }, { "f10", KEY_F10 }, { "f11", KEY_F11 },
	{ "f12", KEY_F12 }, { "grave", KEY_GRAVE }, { "1", KEY_1 }, { "2", KEY_2 }, { "3", KEY_3 }, { "4", KEY_4 },
	{ "5", KEY_5 }, { "6", KEY_6 }, { "7", KEY_7 }, { "8", KEY_8 }, { "9", KEY_9 }, { "0", KEY_0 },
	{ "minus", KEY_MINUS }, { "equal", KEY_EQUAL }, { "backspace", KEY_BACKSPACE }, { "tab", KEY_TAB },
	{ "q", KEY_Q }, { "w", KEY_W }, { "e", KEY_E }, { "r", KEY_R }, { "t", KEY_T }, { "y", KEY_Y }, { "u", KEY_U },
	{ "i", KEY_I }, { "o", KEY_O }, { "p", KEY_P }, { "leftbrace", KEY_LEFTBRACE }, { "rightbrace", KEY_RIGHTBRACE },
	{ "enter", KEY_ENTER }, { "ctrl", KEY_LEFTCTRL }, { "a", KEY_A }, { "s", KEY_S }, { "d", KEY_D }, { "f", KEY_F },
	{ "g", KEY_G }, { "h", KEY_H }, { "j", KEY_J }, { "k", KEY_K }, { "l", KEY_L }, { "semicolon", KEY_SEMICOLON },
	{ "apostrophe", KEY_APOSTROPHE }, { "shift", KEY_LEFTSHIFT }, { "backslash", KEY_BACKSLASH }, { "z", KEY_Z },
	{ "x", KEY_X }, { "c", KEY_C }, { "v", KEY_V }, { "b", KEY_B }, { "n", KEY_N }, { "m", KEY_M },
	{ "comma", KEY_COMMA }, { "dot", KEY_DOT }, { "slash", KEY_SLASH }, { "alt", KEY_LEFTALT }, { "space", KEY_SPACE },
	{ "home", KEY_HOME }, { "up", KEY_UP }, { "pageup", KEY_PAGEUP }, { "left", KEY_LEFT }, { "right", KEY_RIGHT },
	{ "end", KEY_END }, { "down", KEY_DOWN }, { "pagedown", KEY_PAGEDOWN }, { "insert", KEY_INSERT },
	{ "delete", KEY_DELETE }, { "super", KEY_LEFTMETA }, { "altgr", KEY_RIGHTALT },
};

/* The keys of the printable ASCII characters on a US layout, from ' ', and whether they need Shift */
static const struct {
	uint16_t code;
	bool shift;
} ascii[] = {
	{ KEY_SPACE, false }, { KEY_1, true }, { KEY_APOSTROPHE, true }, { KEY_3, true }, { KEY_4, true }, { KEY_5, true },
	{ KEY_7, true }, { KEY_APOSTROPHE, false }, { KEY_9, true }, { KEY_0, true }, { KEY_8, true }, { KEY_EQUAL, true },
	{ KEY_COMMA, false }, { KEY_MINUS, false }, { KEY_DOT, false }, { KEY_SLASH, false }, { KEY_0, false },
	{ KEY_1, false }, { KEY_2, false }, { KEY_3, false }, { KEY_4, false }, { KEY_5, false }, { KEY_6, false },
	{ KEY_7, false }, { KEY_8, false }, { KEY_9, false }, { KEY_SEMICOLON, true }, { KEY_SEMICOLON, false },
	{ KEY_COMMA, true }, { KEY_EQUAL, false }, { KEY_DOT, true }, { KEY_SLASH, true }, { KEY_2, true },
	{ KEY_A, true }, { KEY_B, true }, { KEY_C, true }, { KEY_D, true }, { KEY_E, true }, { KEY_F, true },
	{ KEY_G, true }, { KEY_H, true }, { KEY_I, true }, { KEY_J, true }, { KEY_K, true }, { KEY_L, true },
	{ KEY_M, true }, { KEY_N, true }, { KEY_O, true }, { KEY_P, true }, { KEY_Q, true }, { KEY_R, true },
	{ KEY_S, true }, { KEY_T, true }, { KEY_U, true }, { KEY_V, true }, { KEY_W, true }, { KEY_X, true },
	{ KEY_Y, true }, { KEY_Z, true }, { KEY_LEFTBRACE, false }, { KEY_BACKSLASH, false }, { KEY_RIGHTBRACE, false },
	{ KEY_6, true }, { KEY_MINUS, true }, { KEY_GRAVE, false }, { KEY_A, false }, { KEY_B, false }, { KEY_C, false },
	{ KEY_D, false }, { KEY_E, false }, { KEY_F, false }, { KEY_G, false }, { KEY_H, false }, { KEY_I, false },
	{ KEY_J, false }, { KEY_K, false }, { KEY_L, false }, { KEY_M, false }, { KEY_N, false }, { KEY_O, false },
	{ KEY_P, false }, { KEY_Q, false }, { KEY_R, false }, { KEY_S, false }, { KEY_T, false }, { KEY_U, false },
	{ KEY_V, false }, { KEY_W, false }, { KEY_X, false }, { KEY_Y, false }, { KEY_Z, false }, { KEY_LEFTBRACE, true },
	{ KEY_BACKSLASH, true }, { KEY_RIGHTBRACE, true }, { KEY_GRAVE, true },
};

static_assert(ARRAY_SIZE(ascii) == '~' - ' ' + 1);

/* The device, the desktop that its pointer covers, and where positions start */
static struct {
	int fd;
	int32_t width, height, x, y;
	double scale;
} input;

[[noreturn]] static void usage(void)
{
	fprintf(stderr, "usage: fake-input -d WIDTHxHEIGHT [-o X,Y] [-s SCALE] COMMAND...\n"
			"  key NAME[+NAME]...  press keys together: key ctrl+s, key alt+left, key f12\n"
			"  down NAME, up NAME  hold a key down and let it go, for clicks with it\n"
			"  type TEXT           type ASCII text with the keys of a US layout\n"
			"  move X Y            move the pointer\n"
			"  click X Y           click, and middle, right and double click\n"
			"  drag X Y X Y        drag with the left button\n"
			"  wheel N             turn the wheel by N notches, positive is up\n"
			"  sleep MS            wait\n"
			"  run COMMAND         run a shell command and wait for it, like a capture between inputs\n");
	exit(2);
}

static void pause_ms(uint32_t ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = ms % 1000 * 1'000'000L };

	nanosleep(&ts, nullptr);
}

static void emit(uint16_t type, uint16_t code, int32_t value)
{
	struct input_event event = { .type = type, .code = code, .value = value };

	if (write(input.fd, &event, sizeof(event)) != sizeof(event)) {
		perror("fake-input: write");
		exit(1);
	}
}

static void report(void)
{
	emit(EV_SYN, SYN_REPORT, 0);
	pause_ms(STEP_MS);
}

static bool key_code(const char *name, size_t length, uint16_t *code)
{
	foreach_array_const_ref(keys, key) {
		if (strlen(key->name) == length && !strncasecmp(key->name, name, length)) {
			*code = key->code;
			return true;
		}
	}
	return false;
}

/* Keys go down in their order and up the other way */
static bool chord(const char *names, bool run)
{
	uint16_t codes[8];
	size_t count = 0;

	for (const char *name = names, *end; *name; name = *end ? end + 1 : end) {
		end = strchrnul(name, '+');
		if (count == ARRAY_SIZE(codes) || !key_code(name, end - name, &codes[count++]))
			return false;
	}
	if (!run)
		return count;
	foreach_array_const_ref(arrview_first(count, codes), code) {
		emit(EV_KEY, *code, 1);
		report();
	}
	foreach_array_const_ref_bw(arrview_first(count, codes), code) {
		emit(EV_KEY, *code, 0);
		report();
	}
	return true;
}

static bool type(const char *text, bool run)
{
	foreach_array_const_ref(array_ptr(text, strlen(text)), c) {
		if (*c < ' ' || *c > '~')
			return false;
		if (!run)
			continue;
		if (ascii[*c - ' '].shift) {
			emit(EV_KEY, KEY_LEFTSHIFT, 1);
			report();
		}
		emit(EV_KEY, ascii[*c - ' '].code, 1);
		report();
		emit(EV_KEY, ascii[*c - ' '].code, 0);
		report();
		if (ascii[*c - ' '].shift) {
			emit(EV_KEY, KEY_LEFTSHIFT, 0);
			report();
		}
	}
	return true;
}

static void move(double x, double y)
{
	emit(EV_ABS, ABS_X, (input.x + x / input.scale) * ABS_RANGE / input.width);
	emit(EV_ABS, ABS_Y, (input.y + y / input.scale) * ABS_RANGE / input.height);
	report();
}

static void button(uint16_t code, bool pressed)
{
	emit(EV_KEY, code, pressed);
	report();
}

static bool number(const char *string, double *value)
{
	char *end;

	*value = strtod(string, &end);
	return end != string && !*end;
}

/* Commands are checked before the device comes, and run after */
static void commands(int count, char **argv, bool run)
{
	for (int i = 0; i < count; i++) {
		const char *command = argv[i];
		double values[4];
		int needed = !strcmp(command, "drag") ? 4 :
			     !strcmp(command, "move") || !strcmp(command, "click") || !strcmp(command, "middle") ||
			     !strcmp(command, "right") || !strcmp(command, "double") ? 2 :
			     !strcmp(command, "wheel") || !strcmp(command, "sleep") ? 1 : 0;

		if (!strcmp(command, "key") || !strcmp(command, "type")) {
			if (++i == count || !(command[0] == 'k' ? chord(argv[i], run) : type(argv[i], run)))
				usage();
			continue;
		}
		if (!strcmp(command, "run")) {
			if (++i == count)
				usage();
			if (run && system(argv[i])) {
				fprintf(stderr, "fake-input: '%s' failed\n", argv[i]);
				exit(1);
			}
			continue;
		}
		if (!strcmp(command, "down") || !strcmp(command, "up")) {
			uint16_t code;

			if (++i == count || !key_code(argv[i], strlen(argv[i]), &code))
				usage();
			if (run) {
				emit(EV_KEY, code, command[0] == 'd');
				report();
			}
			continue;
		}
		if (!needed || i + needed >= count)
			usage();
		foreach_array_index(arrview_first(needed, values), j)
			if (!number(argv[i + 1 + j], &values[j]))
				usage();
		i += needed;
		if (!run)
			continue;
		if (!strcmp(command, "sleep")) {
			pause_ms(values[0]);
		} else if (!strcmp(command, "wheel")) {
			emit(EV_REL, REL_WHEEL, values[0]);
			report();
		} else if (!strcmp(command, "drag")) {
			move(values[0], values[1]);
			button(BTN_LEFT, true);
			for (int step = 1; step <= 10; step++)
				move(values[0] + (values[2] - values[0]) * step / 10, values[1] + (values[3] - values[1]) * step / 10);
			button(BTN_LEFT, false);
		} else {
			uint16_t code = !strcmp(command, "middle") ? BTN_MIDDLE : !strcmp(command, "right") ? BTN_RIGHT : BTN_LEFT;

			move(values[0], values[1]);
			for (int times = !strcmp(command, "double") ? 2 : !strcmp(command, "move") ? 0 : 1; times; times--) {
				button(code, true);
				button(code, false);
			}
		}
	}
}

int main(int argc, char **argv)
{
	struct uinput_setup setup = { .id = { .bustype = BUS_VIRTUAL, .vendor = 0x1d6b, .product = 0x0104 } };
	struct uinput_abs_setup range = { .absinfo = { .minimum = 0, .maximum = ABS_RANGE } };
	int option;

	input.scale = 1;
	input.width = input.height = input.x = input.y = 0;
	while ((option = getopt(argc, argv, "+d:o:s:")) != -1) {
		if ((option == 'd' && sscanf(optarg, "%dx%d", &input.width, &input.height) != 2) ||
		    (option == 'o' && sscanf(optarg, "%d,%d", &input.x, &input.y) != 2) ||
		    (option == 's' && (sscanf(optarg, "%lf", &input.scale) != 1 || input.scale <= 0)) || option == '?')
			usage();
	}
	if (input.width <= 0 || input.height <= 0 || optind == argc)
		usage();
	commands(argc - optind, argv + optind, false);
	input.fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
	if (input.fd < 0) {
		perror("fake-input: /dev/uinput");
		return 1;
	}
	ioctl(input.fd, UI_SET_EVBIT, EV_KEY);
	foreach_array_const_ref(keys, key)
		ioctl(input.fd, UI_SET_KEYBIT, key->code);
	foreach_array_const_ref(((const uint16_t[]){ BTN_LEFT, BTN_RIGHT, BTN_MIDDLE }), code)
		ioctl(input.fd, UI_SET_KEYBIT, *code);
	ioctl(input.fd, UI_SET_EVBIT, EV_REL);
	ioctl(input.fd, UI_SET_RELBIT, REL_WHEEL);
	ioctl(input.fd, UI_SET_EVBIT, EV_ABS);
	foreach_array_const_ref(((const uint16_t[]){ ABS_X, ABS_Y }), code) {
		ioctl(input.fd, UI_SET_ABSBIT, *code);
		range.code = *code;
		ioctl(input.fd, UI_ABS_SETUP, &range);
	}
	snprintf_array(setup.name, "fake-input");
	if (ioctl(input.fd, UI_DEV_SETUP, &setup) < 0 || ioctl(input.fd, UI_DEV_CREATE) < 0) {
		perror("fake-input: uinput");
		return 1;
	}
	pause_ms(SETTLE_MS);
	commands(argc - optind, argv + optind, true);
	pause_ms(SETTLE_MS / 2);
	ioctl(input.fd, UI_DEV_DESTROY);
	close(input.fd);
	return 0;
}
