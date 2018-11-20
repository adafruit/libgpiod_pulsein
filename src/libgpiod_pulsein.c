#include <gpiod.h>
#include "libgpiod_pulsein.h"

#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static const struct option longopts[] = {
	{ "help",	no_argument,	NULL,	'h' },
	{ "version",	no_argument,	NULL,	'v' },
	{ "active-low",	no_argument,	NULL,	'l' },
};

static const char *const shortopts = "+hvl";

static void print_help(void)
{
	printf("Usage: libgpiod_pulsein [OPTIONS] <chip name/number> <offset>\n");
	printf("Continuously poll line value from a GPIO chip\n");
	printf("\n");
	printf("Options:\n");
	printf("  -h, --help:\t\tdisplay this message and exit\n");
	printf("  -v, --version:\tdisplay the version and exit\n");
	printf("  -l, --active-low:\tset the line active state to low\n");
}

int main(int argc, char **argv) {
	int offset, optc, opti, value, previous_value;
	bool active_low = false;
	char *device, *end;
	struct timeval tv1, tv2;

	for (;;) {
		optc = getopt_long(argc, argv, shortopts, longopts, &opti);
		if (optc < 0)
			break;

		switch (optc) {
		case 'h':
			print_help();
			return EXIT_SUCCESS;
		case 'v':
			printf("libgpiod_pulsein v0.0.1");
			return EXIT_SUCCESS;
		case 'l':
			active_low = true;
			break;
		default:
			abort();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		printf("gpiochip must be specified");
		exit(1);
	}

	if (argc < 2) {
		printf("a single GPIO line offset must be specified");
		exit(1);
	}

	device = argv[0];
	offset = strtoul(argv[1], &end, 10);
	if (*end != '\0' || offset > INT_MAX) {
	        printf("invalid GPIO offset: %s", argv[1]);
		exit(1);
	}

	// Print initial value:
	previous_value = gpiod_ctxless_get_value(device, offset, active_low, "libgpiod_pulsein");
	printf("%d\t0\n", previous_value);
	gettimeofday(&tv1, NULL);

	for (;;) {
		value = gpiod_ctxless_get_value(device, offset, active_low, "libgpiod_pulsein");
		if (value < 0) {
	        	printf("error reading GPIO values");
			exit(1);
		}
		if (value != previous_value) {
	                gettimeofday(&tv2, NULL);
			printf(
				"%d\t%0.f\n",
				value,
                                ((double) (tv2.tv_usec - tv1.tv_usec) + (double) (tv2.tv_sec - tv1.tv_sec) * 1000000)
			);
			gettimeofday(&tv1, NULL);
		}
		previous_value = value;
	}

	return EXIT_SUCCESS;
}
