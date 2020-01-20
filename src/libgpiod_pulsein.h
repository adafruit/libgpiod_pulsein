#include <gpiod.h>
#include <stdbool.h>

//#define FOLLOW_PULSE  19
#define MAX_PULSE_BUFFER 1000

void set_max_priority(void);
void sig_handler(int signo);
void print_pulses(void);
float calculate_us_per_tick(struct gpiod_line *line);
void pulse_output(struct gpiod_line *line, bool idle_state, int trigger_len_us);
void *polling_thread_runner(void *argsin);
void busy_wait_milliseconds(int millis);
