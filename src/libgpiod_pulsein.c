// MIT License
//
// Copyright (c) 2018 adafruit industries
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "libgpiod_pulsein.h"
#include "circular_buffer.h"
#include <getopt.h>
#include <gpiod.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/time.h>
#include <unistd.h>

#define VMSG_MAXSIZE 4096
struct vmsgbuf {
  long msg_type;
  char message[VMSG_MAXSIZE];
};

unsigned int pulses[MAX_PULSE_BUFFER] = {0};
cbuf_handle_t ringbuffer;
volatile bool paused = false;

const char *consumername = "libgpiod_pulsein";

static const struct option longopts[] = {
    {"help", no_argument, NULL, 'h'},
    {"version", no_argument, NULL, 'v'},
    {"idle_state", no_argument, NULL, 'i'},
    {"trigger", required_argument, NULL, 'd'},
    {"pulses", required_argument, NULL, 'p'},
    {"timeout", required_argument, NULL, 't'},
    {"queue", required_argument, NULL, 'q'},
    {"slow", no_argument, NULL, 's'},
};

static const char *const shortopts = "+hviptd";

static void print_help(void) {
  printf("Usage: libgpiod_pulsein [OPTIONS] <chip name/number> <offset>\n");
  printf("Continuously poll line value from a GPIO chip\n");
  printf("\n");
  printf("Options:\n");
  printf("  -h, --help:\t\tdisplay this message and exit\n");
  printf("  -v, --version:\tdisplay the version and exit\n");
  printf(
      "  -i, --idle_state:\tset the line idle state to HIGH (defalt is low)\n");
  printf("  -p, --pulses:\tnumber of pulses to store in ring buffer\n");
  printf("  -t, --timeout:\tnumber microseconds to wait before exit\n");
  printf("  -d, --trigger:\tSend an initial output pulse of n microseconds\n");
  printf("  -q, --queue:\tID number of SYSV queue for IPC\n");
  printf("  -s, --slow:\tWe're running on a slow linux machine,\ntry to "
         "calibrate us-per-tick - values may not be true us");
}

int main(int argc, char **argv) {
  int offset, optc, opti, value, previous_value;
  int max_pulses = MAX_PULSE_BUFFER;
  int32_t timeout_microseconds = 0, trigger_len_us = 0;
  bool idle_state = false, exit_on_timeout = false, trigger_pulse = false,
       fast_linux = true, waiting_for_first_change = true;
  char *device, *end;
  struct gpiod_chip *chip = NULL;
  struct gpiod_line *line;
#if defined(FOLLOW_PULSE)
  struct gpiod_line *line2;
#endif
  struct timeval time_event;
  double previous_time = 0, current_time;
  long int previous_tick, current_tick;
  float us_per_tick = 0;
  struct vmsgbuf vmbuf;
  int queue_id = 0, queue_key = 0;

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
    case 'i':
      idle_state = true;
      break;
    case 's':
      fast_linux = false;
      break;
    case 'p':
      max_pulses = strtoul(optarg, &end, 10);
      if (*end != '\0' || offset > INT_MAX) {
        printf("invalid max pulse count: %s", optarg);
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
    case 'q':
      queue_key = strtoul(optarg, &end, 10);
      if (*end != '\0' || offset > INT_MAX) {
        printf("invalid queue key: %s", optarg);
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

  // Bump up process priority and change scheduler to try
  // to make process more 'real time'.
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

  // Set up message passing system, if requested
  if (queue_key != 0) {
    queue_id = msgget(queue_key, IPC_CREAT);
    if (queue_id == -1) {
      printf("Unable to create message queue\n");
      exit(1);
    }
    memset(vmbuf.message, 0, VMSG_MAXSIZE);
    while (msgrcv(queue_id, (struct msgbuf *)&vmbuf, VMSG_MAXSIZE, 1,
                  IPC_NOWAIT) != -1) {
      // flush by reading every message
    }
    // tell them we're ready!
    vmbuf.message[0] = '!';
    vmbuf.msg_type = 2;
    msgsnd(queue_id, (struct msgbuf *)&vmbuf, 1, 0);
  }

  if (!fast_linux && us_per_tick == 0) {
    us_per_tick = calculate_us_per_tick(line);
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

  // set to an input
  if (gpiod_line_request_input(line, consumername) != 0) {
    printf("Unable to set line %d to input\n", offset);
    exit(1);
  }

  if (trigger_pulse) {
    pulse_output(line, idle_state, trigger_len_us);
  }

  if (fast_linux) {
    gettimeofday(&time_event, NULL);
    previous_time = time_event.tv_sec;
    previous_time *= 1000000;
    previous_time += time_event.tv_usec;
  } else {
    previous_tick = current_tick = 0;
  }

  // a simple ring buffer
  ringbuffer = circular_buf_init(pulses, max_pulses);
  circular_buf_reset(ringbuffer);

  // We record the first change from the idle_state
  previous_value = idle_state;

  for (;;) {
    if (queue_key != 0) {
      vmbuf.msg_type = 1;
      int msglen = msgrcv(queue_id, (struct msgbuf *)&vmbuf, VMSG_MAXSIZE - 1,
                          1, IPC_NOWAIT);
      if ((msglen != -1) && (msglen >= 1)) {
        vmbuf.message[msglen] = 0; // null terminate message to keep neat
        bool was_paused = paused;

        // printf("got %d byte message: %s\n", msglen, vmbuf.message);
        char cmd = vmbuf.message[0];
        if (cmd == 'p') {
          // pause
          paused = true;
        } else if (cmd == 'r') {
          // resume
          paused = false;
        } else if (cmd == 'c') {
          // clear
          circular_buf_reset(ringbuffer);
        } else if (cmd == 'l') {
          // send back length
          int buflen = circular_buf_size(ringbuffer);
          snprintf(vmbuf.message, 15, "%d", buflen);
          vmbuf.msg_type = 2;
          msgsnd(queue_id, (struct msgbuf *)&vmbuf, strlen(vmbuf.message), 0);
        } else if (cmd == 't') {
          // Resume with trigger pulse!
          unsigned int trigger_len = strtoul(vmbuf.message + 1, NULL, 10);
          // printf("trigger %d\n", trigger_len);
          pulse_output(line, idle_state, trigger_len);
          paused = false;
        } else if (cmd == '^') {
          // pop one message off and send it
          unsigned int pulse;
          if (circular_buf_get(ringbuffer, &pulse) == -1) {
            pulse = -1;
          }
          snprintf(vmbuf.message, 15, "%d", pulse);
          vmbuf.msg_type = 2;
          msgsnd(queue_id, (struct msgbuf *)&vmbuf, strlen(vmbuf.message), 0);
        } else if (cmd == 'i') {
          // query one element by index #
          int index = strtol(vmbuf.message + 1, NULL, 10);
          int buf_len = circular_buf_size(ringbuffer);
          unsigned int pulse = 0;
          if ((index >= buf_len) || (index <= -buf_len)) {
            pulse = -1; // invalid, we're seeking beyond the buffer
          } else {
            if (index < 0) { // back indexing from end
              index = buf_len + index;
            }
            // peek in the queue!
            if (circular_buf_peek(ringbuffer, index, &pulse) == -1) {
              pulse = -1;
            }
          }
          // OK reply back!
          snprintf(vmbuf.message, 15, "%d", pulse);
          vmbuf.msg_type = 2;
          msgsnd(queue_id, (struct msgbuf *)&vmbuf, strlen(vmbuf.message), 0);
        }
        if (was_paused && !paused) {
          // reset the timestamp when unpaused
          if (fast_linux) {
            gettimeofday(&time_event, NULL);
            previous_time = time_event.tv_sec;
            previous_time *= 1000000;
            previous_time += time_event.tv_usec;
          } else {
            previous_tick = current_tick = 0;
          }
          previous_value = idle_state;
          waiting_for_first_change = true;
        }
      }
    }
    if (paused) {
      continue;
    }
    value = gpiod_line_get_value(line);
    if (value < 0) {
      printf("Unable to read line %d\n", offset);
      exit(1);
    }

    if (!fast_linux) {
      current_tick++;
    }

    double delta = 0;
    if (fast_linux) {
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
      if (waiting_for_first_change && (value != idle_state)) {
        // we *dont* save the first transition from idle value
        waiting_for_first_change = false;
      } else {
        circular_buf_put(ringbuffer, delta);
      }

      previous_value = value;
      if (fast_linux) {
        previous_time = current_time;
      } else {
        previous_tick = current_tick;
      }
    }
  }

  print_pulses();
  return EXIT_SUCCESS;
}

void sig_handler(int signo) {
  if (signo == SIGINT) {
    fprintf(stderr, "received SIGINT\n");
    print_pulses();
    exit(0);
  }
  if (signo == SIGTSTP) {
    fprintf(stderr, "pausing\n");
    paused = true;
  }
  if (signo == SIGCONT) {
    fprintf(stderr, "un-pausing\n");
    paused = false;
  }
}

void print_pulses(void) {
  int pulse_count = circular_buf_size(ringbuffer);
  for (int i = 0; i < pulse_count; i++) {
    unsigned int pulse = 0;
    circular_buf_get(ringbuffer, &pulse);

    printf("%d", pulse);
    if (i != pulse_count - 1) {
      printf(", ");
    }
  }
  printf("\n");
}

void set_max_priority(void) {
  struct sched_param sched;
  memset(&sched, 0, sizeof(sched));
  // Use FIFO scheduler with highest priority for the lowest chance of the
  // kernel context switching.
  sched.sched_priority = sched_get_priority_max(SCHED_FIFO);
  sched_setscheduler(0, SCHED_FIFO, &sched);
}

void pulse_output(struct gpiod_line *line, bool idle_state,
                  int trigger_len_us) {
  // printf("Triggering output for %d microseconds\n", trigger_len_us);
  gpiod_line_release(line);
  // set to an output
  if (gpiod_line_request_output(line, consumername, idle_state) != 0) {
    printf("Unable to set line to output\n");
    exit(1);
  }
  // set 'active'
  if (gpiod_line_set_value(line, !idle_state) != 0) {
    printf("Unable to set line for trigger pulse\n");
    exit(1);
  }
  // wait
  usleep(trigger_len_us);
  // set back to idle
  if (gpiod_line_set_value(line, idle_state) != 0) {
    printf("Unable to set line for trigger pulse\n");
    exit(1);
  }

  // release for input usage
  gpiod_line_release(line);

  // set back to an input
  if (gpiod_line_request_input(line, consumername) != 0) {
    printf("Unable to set line to input\n");
    exit(1);
  }
}

float calculate_us_per_tick(struct gpiod_line *line) {
  struct timeval time_event;
  double previous_time, current_time;
  // self calibrate best we can
  // printf("Calculating us per tick\n");

  if (gpiod_line_request_input(line, consumername) != 0) {
    printf("Unable to set line to input\n");
    exit(1);
  }

  gettimeofday(&time_event, NULL);
  previous_time = time_event.tv_sec;
  previous_time *= 1000000;
  previous_time += time_event.tv_usec;

  for (int i = 0; i < 100; i++) {
    int ret = gpiod_line_get_value(line);
    if (ret == -1) {
      printf("Unable to read line during calibration\n");
      exit(1);
    }
  }
  gettimeofday(&time_event, NULL);
  current_time = time_event.tv_sec;
  current_time *= 1000000;
  current_time += time_event.tv_usec;
  float us_per_tick = (current_time - previous_time) / 100;
  // printf("us_per_tick: %f\n", us_per_tick);
  // Be kind, rewind!
  gpiod_line_release(line);
  return us_per_tick;
}
