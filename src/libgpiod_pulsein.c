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

	{ "pulses",	required_argument,	NULL,	'p' },
	{ "timeout",	required_argument,	NULL,	't' },
};

static const char *const shortopts = "+hvlpt";

static void print_help(void)
{
	printf("Usage: libgpiod_pulsein [OPTIONS] <chip name/number> <offset>\n");
	printf("Continuously poll line value from a GPIO chip\n");
	printf("\n");
	printf("Options:\n");
	printf("  -h, --help:\t\tdisplay this message and exit\n");
	printf("  -v, --version:\tdisplay the version and exit\n");
	printf("  -l, --active-low:\tset the line active state to low\n");
	printf("  -p, --pulses:\tnumber of state changes to record before exit\n");
	printf("  -t, --timeout:\tnumber microseconds to wait before exit\n");
}

int main(int argc, char **argv) {
	int offset, optc, opti, value, previous_value, wanted_pulses,
	    timeout_microseconds;
	double start_time;
	int pulse_count = 0;
	bool active_low = false, count_pulses = false, exit_on_timeout = false;
	char *device, *end;
	struct timeval previous_event, current_time;

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
		case 'p':
			count_pulses = true;
			wanted_pulses = strtoul(optarg, &end, 10);
			if (*end != '\0' || offset > INT_MAX) {
				printf("invalid pulse count: %s", optarg);
				exit(1);
			}
			break;
		case 't':
			exit_on_timeout = true;
			timeout_microseconds = strtoul(optarg, &end, 10);
			if (*end != '\0' || offset > INT_MAX) {
				printf("invalid timeout: %s", optarg);
				exit(1);
			}
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
	gettimeofday(&previous_event, NULL);
	start_time = previous_event.tv_usec + (previous_event.tv_sec * 1000000);

	for (;;) {
		// Get current time, check for timeout:
		gettimeofday(&current_time, NULL);
		if (exit_on_timeout) {
			if ((current_time.tv_usec + (current_time.tv_sec * 1000000) - start_time >= timeout_microseconds))
					return EXIT_SUCCESS;
		}

		value = gpiod_ctxless_get_value(device, offset, active_low, "libgpiod_pulsein");
		if (value < 0) {
	        	printf("error reading GPIO values");
			exit(1);
		}
		if (value != previous_value) {
			printf(
				"%d\t%0.f\n",
				value,
                                ((double) (current_time.tv_usec - previous_event.tv_usec) + (double) (current_time.tv_sec - previous_event.tv_sec) * 1000000)
			);
			gettimeofday(&previous_event, NULL);

			// If the user asked us to limit returned state changes, keep track
			// and exit when the count is satisfied:
			if (count_pulses) {
				pulse_count++;
				if (pulse_count == wanted_pulses)
					return EXIT_SUCCESS;

			}
		}
		previous_value = value;
	}

	return EXIT_SUCCESS;
}
