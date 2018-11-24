#include <gpiod.h>
#include "libgpiod_pulsein.h"

#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>


int pulse_count = 0;
unsigned int pulses[MAX_PULSES] = {0};

const char *consumername = "libgpiod_pulsein";

static const struct option longopts[] = {
	{ "help",	no_argument,	NULL,	'h' },
	{ "version",	no_argument,	NULL,	'v' },
	{ "active-low",	no_argument,	NULL,	'l' },
	{ "trigger",    required_argument,      NULL,   'd' },
	{ "pulses",	required_argument,	NULL,	'p' },
	{ "timeout",	required_argument,	NULL,	't' },
};

static const char *const shortopts = "+hvlptd";

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
	  wanted_pulses = 0;
	int32_t timeout_microseconds = 0, trigger_len_us = 0;
	bool active_low = false, 
	  count_pulses = false, 
	  exit_on_timeout = false,
	  trigger_pulse = false,
	  fast_linux = false;
	char *device, *end;
	struct gpiod_chip *chip = NULL;
	struct gpiod_line *line;
#if defined(FOLLOW_PULSE)
	struct gpiod_line *line2;
#endif
	struct timeval time_event;
	double previous_time, current_time;
	long int previous_tick, current_tick;
	float us_per_tick = 0;

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

	if (signal(SIGINT, sig_handler) == SIG_ERR) {
	  printf("Can't catch SIGINT\n");
	  exit(1);
	}


	// Bump up process priority and change scheduler to try to try to make process more 'real time'.
	set_max_priority();


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

	if (!fast_linux && us_per_tick == 0) {
	  // self calibrate best we can
	  //printf("Calculating us per tick\n");

	  if (gpiod_line_request_input(line, consumername) != 0) {
	    printf("Unable to set line %d to input\n", offset);
	    exit(1);
	  }

	  gettimeofday(&time_event, NULL);
	  previous_time = time_event.tv_sec;
	  previous_time *= 1000000;
	  previous_time += time_event.tv_usec;

	  for (int i=0; i<100; i++) {
	    int ret = gpiod_line_get_value(line);
	    if (ret == -1) {
	      printf("Unable to read line %d\n", offset);
	      exit(1);
	    }
	  }
	  gettimeofday(&time_event, NULL);
	  current_time = time_event.tv_sec;
	  current_time *= 1000000;
	  current_time += time_event.tv_usec;
	  us_per_tick = (current_time - previous_time) / 100;
	  //printf("us_per_tick: %f\n", us_per_tick);
	  // Be kind, rewind!
	  gpiod_line_release(line);
	}

#if defined(FOLLOW_PULSE)
	// Helpful for debugging where we do our reads on a scope
	line2 = gpiod_chip_get_line(chip, FOLLOW_PULSE);
	if (!line2) {
	  printf("Unable to open line: %d\n", FOLLOW_PULSE);
	  exit(1);
	}
	gpiod_line_release(line2);
	if (gpiod_line_request_output(line2, consumername, 0) != 0) {
	    printf("Unable to set line %d to output\n", FOLLOW_PULSE);
	    exit(1);
	}
#endif

	if (trigger_pulse) {
	  //printf("Triggering output for %d microseconds\n", trigger_len_us);
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
	    printf("Unable to set line %d to active level\n", offset);
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

	if ( fast_linux) {
	  gettimeofday(&time_event, NULL);
	  previous_time = time_event.tv_sec;
	  previous_time *= 1000000;
	  previous_time += time_event.tv_usec;
	} else {
	  previous_tick = current_tick = 0;
	}


	previous_value = gpiod_line_get_value(line);
	if (previous_value == -1) {
	  printf("Unable to read line %d\n", offset);
	  exit(1);
	}

	for (;;) {
	  value = gpiod_line_get_value(line);
	  if (value < 0) {
	    printf("Unable to read line %d\n", offset);
	    exit(1);
	  }

	  if (! fast_linux) {
	    current_tick++;
	  }

	  double delta = 0;
	  if ( fast_linux) {
	    // Get current time
	    gettimeofday(&time_event, NULL);
	    current_time = time_event.tv_sec;
	    current_time *= 1000000;
	    current_time += time_event.tv_usec;
	    delta = current_time - previous_time;
	  } else {
	    delta = (current_tick - previous_tick) * us_per_tick;
	  }

	  // check for timeout:
	  if (exit_on_timeout) {
	    if (delta >= timeout_microseconds) {
	      print_pulses();
	      return EXIT_SUCCESS;
	    }
	  }

#if defined(FOLLOW_PULSE)
	  if (gpiod_line_set_value(line2, value) != 0) {
	    printf("Unable to set line %d to active level\n", FOLLOW_PULSE);
	    exit(1);
	    }
#endif
	  if (value != previous_value) {
	    pulses[pulse_count] = delta;
	    pulse_count++;
	    //printf("%d\t%0.f\n", value, delta);
	      
	    // If the user asked us to limit returned state changes, keep track
	    // and exit when the count is satisfied:
	    if (count_pulses) {
	      if (pulse_count == wanted_pulses) {
		printf("pulsed out\n");
		print_pulses();
		return EXIT_SUCCESS;
	      }
	    }
	    previous_value = value;
	    if ( fast_linux) {
	      previous_time = current_time;
	    } else {
	      previous_tick = current_tick;
	    }
	  }
	}

	print_pulses();
	return EXIT_SUCCESS;
}


void sig_handler(int signo)
{
  if (signo == SIGINT) {
    printf("received SIGINT\n");
  }
  print_pulses();
  exit(0);
}

void print_pulses(void) {
  printf("%d PULSES\n", pulse_count);
  for (int i=0; i<pulse_count; i++) {
    printf("%d, ", pulses[i]);
  }
  printf("\n");
}


void set_max_priority(void) {
  struct sched_param sched;
  memset(&sched, 0, sizeof(sched));
  // Use FIFO scheduler with highest priority for the lowest chance of the kernel context switching.
  sched.sched_priority = sched_get_priority_max(SCHED_FIFO);
  sched_setscheduler(0, SCHED_FIFO, &sched);
}
