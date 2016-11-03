#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <math.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>

#define SHOW   0
#define INC    1
#define DEC    2
#define SETLOG 3
#define SETLIN 4
#define SCALE  5

static const char self[] = "xcubiclight";

int action = SHOW;
char* display = NULL;
int devindex = 0;
int notches = 20;
int userval = 0;
short tozero = 0;

xcb_connection_t* conn;
xcb_atom_t Backlight;

struct backlight {
	xcb_randr_output_t oid;
	int level;
	int min;
	int max;
};

struct natscale {
	double a;
	double b;
	double c;
};

static void die(const char* fmt, ...) __attribute__((noreturn));
static void diex(xcb_generic_error_t* e) __attribute__((noreturn));

static int linval(struct natscale* sc, int i)
{
	double a = sc->a;
	double b = sc->b;
	double c = sc->c;

	double n = notches;
	double x = i/n;

	return (int)round(a*x*x*x + b*x + c);
}

static int natval(struct natscale* sc, int y)
{
	double a = sc->a;
	double b = sc->b;
	double c = sc->c;
	int n = notches;

	double x = 0.5;
	int i;

	for(i = 0; i < 5; i++) {
		double f = a*x*x*x + b*x + c - y;
		double fp = 3*a*x*x + b;
		x = x - f/fp;
	}

	return (int)(round(n*x));
}

static void make_scale(struct natscale* sc, struct backlight* bt)
{
	sc->c = bt->min;
	sc->b = notches;
	sc->a = (bt->max - bt->min) - sc->b;
}

static void die(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "%s: ", self);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(-1);
}

static void diex(xcb_generic_error_t* e)
{
	fprintf(stderr, "%s: response %i error %i maj %i min %i seq %i res %i\n",
			self,
			e->response_type,
			e->error_code,
			e->major_code,
			e->minor_code,
			e->sequence,
			e->resource_id);
	exit(-1);
}

static void handle_opt(int c, char* optarg)
{
	switch(c) {
		case 'i': action = INC; break;
		case 'd': action = DEC; break;
		case 's': action = SETLOG; userval = atoi(optarg); break;
		case 'e': action = SETLIN; userval = atoi(optarg); break;
		case 'q': action = SCALE; break;
		case 'n': notches = atoi(optarg); break;
		case 'o': devindex = atoi(optarg); break;
		case 'z': tozero = 1; break;
	}
}

/* Tiny custom getopt() implementation. This was a getopt call
   but I don't like the way getopt() reports errors. */

static void parse_opts(int argc, char** argv)
{
	const char* optstring = "idqo:n:s:e:z";
	int i;

	for(i = 1; i < argc; i++) {
		char* arg = argv[i];
		if(*arg != '-')
			break;

		for(arg++; *arg; arg++) {
			const char* key = optstring;
			while(*key && *key != *arg)
				key++;
			if(!*key)
				die("unknown option -%c", *arg);

			if(*(key+1) != ':') {
				handle_opt(*arg, NULL);
				continue;
			} else if(*(arg + 1)) {
				handle_opt(*arg, arg + 1);
				break;;
			}

			if(++i >= argc)
				die("argument required");

			handle_opt(*arg, argv[i]);
			break;
		}
	}

	if(i < argc)
		die("too many arguments");
}

static void check_randr_version(void)
{
	xcb_randr_query_version_cookie_t cookie;
	xcb_randr_query_version_reply_t* reply;
	xcb_generic_error_t* error = NULL;

	cookie = xcb_randr_query_version(conn, 1, 2);
	reply = xcb_randr_query_version_reply(conn, cookie, &error);

	if(error)
		diex(error);
	if(!reply)
		die("randr support missing");

	if(reply->major_version != 1 || reply->minor_version < 2)
		die("unsupported randr version");

	free(reply);
}

static void intern_backlight_atom(void)
{
	xcb_intern_atom_cookie_t cookie;
	xcb_intern_atom_reply_t *reply;
	xcb_generic_error_t* error = NULL;
	const char* atom = "Backlight";

	cookie = xcb_intern_atom(conn, 1, strlen(atom), atom);
	reply = xcb_intern_atom_reply(conn, cookie, &error);

	if(error)
		diex(error);
	if(!reply)
		die("cannot intern backlight atom");
	if(reply->atom == XCB_NONE)
		die("no backlight support on any output");

	Backlight = reply->atom;

	free(reply);
}

static void setup_connection(void)
{
	conn = xcb_connect(display, NULL);

	if(!conn)
		die("cannot connect to display");

	check_randr_version();
	intern_backlight_atom();
}

static int get_backlight_range(struct backlight* bt, xcb_randr_output_t out)
{
	xcb_randr_query_output_property_cookie_t cookie;
	xcb_randr_query_output_property_reply_t* reply;
	xcb_generic_error_t *error = NULL;

	cookie = xcb_randr_query_output_property(conn, out, Backlight);
	reply = xcb_randr_query_output_property_reply(conn, cookie, &error);

	bt->max = 0;
	bt->min = 0;

	if(error)
		diex(error);
	if(!reply)
		return 0;
	if(!reply->range)
		goto out;

	int len = xcb_randr_query_output_property_valid_values_length(reply);
	int32_t *values = xcb_randr_query_output_property_valid_values(reply);

	if(len != 2)
		goto out;

	bt->min = values[0];
	bt->max = values[1];

	free(reply);
	return 1;
out:
	free(reply);
	return 0;
}

static int get_backlight(struct backlight* bt, xcb_randr_output_t out)
{
	xcb_randr_get_output_property_cookie_t cookie;
	xcb_randr_get_output_property_reply_t *reply;
	xcb_generic_error_t *error = NULL;

	cookie = xcb_randr_get_output_property(conn, out, Backlight,
	                                       XCB_ATOM_NONE, 0, 4, 0, 0);
	reply = xcb_randr_get_output_property_reply(conn, cookie, &error);

	if(!reply)
		return 0;
	if(reply->type != XCB_ATOM_INTEGER)
		goto out;
	if(reply->num_items != 1)
		goto out;
	if(reply->format != 32)
		goto out;

	bt->oid = out;
	bt->level = *((int32_t*) xcb_randr_get_output_property_data(reply));

	free(reply);
	return get_backlight_range(bt, out);
out:
	free(reply);
	return 0;
}

static int find_device_output(struct backlight* bt, xcb_window_t root)
{
	xcb_randr_get_screen_resources_cookie_t cookie;
	xcb_randr_get_screen_resources_reply_t* reply;
	xcb_generic_error_t* error = NULL;

	cookie = xcb_randr_get_screen_resources(conn, root);
	reply = xcb_randr_get_screen_resources_reply(conn, cookie, &error);

	if(error || !reply)
		return 0;

	xcb_randr_output_t *outs;
	outs = xcb_randr_get_screen_resources_outputs(reply);
	int i;

	for(i = 0; i < reply->num_outputs; i++)
		if(get_backlight(bt, outs[i]))
			return 1;

	return 0;
}

static void find_device(struct backlight* bt)
{
	xcb_screen_iterator_t iter;

	iter = xcb_setup_roots_iterator(xcb_get_setup(conn));

	while(iter.rem) {
		xcb_screen_t *screen = iter.data;
		xcb_window_t root = screen->root;

		if(find_device_output(bt, root))
			break;

		xcb_screen_next(&iter);
	}
}

static void set_backlight(struct backlight* bt, int level)
{
	xcb_void_cookie_t cookie;
	xcb_generic_error_t *error;

	if(level == bt->level)
		return;

	cookie = xcb_randr_change_output_property_checked(conn, bt->oid,
			Backlight, XCB_ATOM_INTEGER, 32, XCB_PROP_MODE_REPLACE,
	                1, (unsigned char*)&level);

	if((error = xcb_request_check(conn, cookie)))
		diex(error);
}

static void set_scaled(struct backlight* bt, struct natscale* ls, int lvl)
{
	set_backlight(bt, linval(ls, lvl));
}

static void inc_scaled(struct backlight* bt, struct natscale* ls, int lvl)
{
	if(lvl >= notches)
		return;

	set_scaled(bt, ls, lvl+1);
}

static void dec_scaled(struct backlight* bt, struct natscale* ls, int lvl)
{
	if(lvl < 2 && !tozero)
		return;
	if(lvl < 1)
		return;

	set_scaled(bt, ls, lvl-1);
}

static void show_backlight(struct backlight* bt, struct natscale* ls, int lvl)
{
	printf("%i (%i..%i) level %i/%i\n",
			bt->level, bt->min, bt->max, lvl, notches);
}

static void show_scale(struct natscale* ls)
{
	int i;

	for(i = 0; i <= notches; i++)
		printf("%s%i", i ? " " : "", linval(ls, i));

	printf("\n");
}

int main(int argc, char** argv)
{
	struct backlight bt;
	struct natscale ls;

	parse_opts(argc, argv);
	setup_connection();

	find_device(&bt);
	make_scale(&ls, &bt);

	int lvl = natval(&ls, bt.level);

	switch(action) {
		case SHOW:
			show_backlight(&bt, &ls, lvl);
			break;
		case SCALE:
			show_scale(&ls);
			break;
		case INC:
			inc_scaled(&bt, &ls, lvl);
			break;
		case DEC:
			dec_scaled(&bt, &ls, lvl);
			break;
		case SETLOG:
			set_scaled(&bt, &ls, userval);
			break;
		case SETLIN:
			set_backlight(&bt, userval);
			break;
	}

	return 0;
}
