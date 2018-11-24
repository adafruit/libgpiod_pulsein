#include <gpiod.h>
#include "libgpiod_pulsein.h"

#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

const char *consumername = "libgpiod_pulsein";

static const struct option longopts[] = {
	{ "help",	no_argument,	NULL,	'h' },
	{ "version",	no_argument,	NULL,	'v' },
	{ "active-low",	no_argument,	NULL,	'l' },
	{ "trigger",    required_argument,      NULL,   'd' },
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
	printf("  -d, --trigger:\tSend an initial output pulse of n microseconds\n");
}

int main(int argc, char **argv) {
	int offset, optc, opti, 
	  value, previous_value, 
	  wanted_pulses;
	int32_t timeout_microseconds, trigger_len_us;
	int pulse_count = 0;
	bool active_low = false, 
	  count_pulses = false, 
	  exit_on_timeout = false,
	  trigger_pulse = false;
	char *device, *end;
	struct gpiod_chip *chip = NULL;
	struct gpiod_line *line;
	struct timeval time_event;
	double previous_time, current_time;

	for (;;) {
		optc = getopt_long(argc, argv, shortopts, longopts, &opti);
		if (optc < 0)
			break;

		switch (optc) {
		case 'h':
			print_help();
			return EXIT_SUCCESS;
		case 'v':
			printf("libgpiod_pulsein v0.0.1\n");
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
		case 'd':
		        trigger_pulse = true;
			trigger_len_us = strtoul(optarg, &end, 10);
			if (*end != '\0' || offset > INT_MAX) {
				printf("invalid trigger length: %s", optarg);
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
		printf("gpiochip must be specified\n");
		print_help();
		exit(1);
	}

	if (argc < 2) {
		printf("a single GPIO line offset must be specified\n");
		print_help();
		exit(1);
	}

	device = argv[0];
	offset = strtoul(argv[1], &end, 10);
	if (*end != '\0' || offset > INT_MAX) {
	        printf("invalid GPIO offset: %s", argv[1]);
		exit(1);
	}

	chip = gpiod_chip_open_by_name(device);
	if (!chip) {
	  printf("Unable to open chip: %s\n", device);
	  exit(1);
	}
	line = gpiod_chip_get_line(chip, offset);
	if (!line) {
	  printf("Unable to open line: %d\n", offset);
	  exit(1);
	}
	// Be kind, rewind!
	gpiod_line_release(line);

	if (trigger_pulse) {
	  printf("Triggering output for %d microseconds\n", trigger_len_us);
	  // set to an output
	  if (gpiod_line_request_output(line, consumername, active_low) != 0) {
	    printf("Unable to set line %d to output\n", offset);
	    exit(1);
	  }
	  // set 'high'
	  if (gpiod_line_set_value(line, !active_low) != 0) {
	    printf("Unable to set line %d to active level\n", offset);
	    exit(1);
	  }
	  // wait
	  usleep(trigger_len_us);
	  // set 'low'
	  if (gpiod_line_set_value(line, active_low) != 0) {
	    printf("Unable to set line %d to unactive level\n", offset);
	    exit(1);
	  }

	  // release for input usage
	  gpiod_line_release(line);
	}

	// set to an input
	if (gpiod_line_request_input(line, consumername) != 0) {
	  printf("Unable to set line %d to input\n", offset);
	  exit(1);
	}	

	// Print initial value:
	previous_value = gpiod_line_get_value(line);
	if (previous_value == -1) {
	  printf("Unable to read line %d\n", offset);
	  exit(1);
	}
	printf("%d\t0\n", previous_value);
	gettimeofday(&time_event, NULL);
	previous_time = time_event.tv_sec;
	previous_time *= 1000000;
	previous_time += time_event.tv_usec;

	for (;;) {
	  value = gpiod_line_get_value(line);
	  if (value < 0) {
	    printf("Unable to read line %d\n", offset);
	    exit(1);
	  }

	  if (value != previous_value) {
	    // Get current time
	    gettimeofday(&time_event, NULL);
	    current_time = time_event.tv_sec;
	    current_time *= 1000000;
	    current_time += time_event.tv_usec;

	    // check for timeout:
	    if (exit_on_timeout) {
	      if (current_time - previous_time >= timeout_microseconds)
		return EXIT_SUCCESS;
	    }
	    printf(
		   "%d\t%0.f\n",
		   value, current_time - previous_time);
	    
	    // If the user asked us to limit returned state changes, keep track
	    // and exit when the count is satisfied:
	    if (count_pulses) {
	      pulse_count++;
	      if (pulse_count == wanted_pulses)
		return EXIT_SUCCESS;
	      
	    }
	  }
	  previous_value = value;
	  previous_time = current_time;
	}

	return EXIT_SUCCESS;
}
