#include <gpiod.h>
#include "libgpiod_pulsein.h"

#include <stdio.h>
#include <string.h>
#include <poll.h>

struct mon_ctx {
	unsigned int offset;
	unsigned int events_wanted;
	unsigned int events_done;

	bool silent;
	char *fmt;

	int sigfd;
};


static void output_event(unsigned int offset,
				       const struct timespec *ts,
				       int event_type)
{
	char *evname;

	if (event_type == GPIOD_CTXLESS_EVENT_CB_RISING_EDGE)
		evname = " RISING EDGE";
	else
		evname = "FALLING EDGE";

	printf("event: %s offset: %u timestamp: [%8ld.%09ld]\n",
	       evname, offset, ts->tv_sec, ts->tv_nsec);
}

static int callback(int event_type, unsigned int line_offset,
		    const struct timespec *timestamp, void *data)
{
	struct mon_ctx *ctx = data;

	switch (event_type) {
	case GPIOD_CTXLESS_EVENT_CB_RISING_EDGE:
	case GPIOD_CTXLESS_EVENT_CB_FALLING_EDGE:
		if (!ctx->silent) {
			output_event(line_offset, timestamp, event_type);
		}
		break;
	}


	if (ctx->events_wanted && ctx->events_done >= ctx->events_wanted)
		return GPIOD_CTXLESS_EVENT_CB_RET_STOP;

	return GPIOD_CTXLESS_EVENT_CB_RET_OK;
}

int main(int argc, char **argv) {
	unsigned int offsets[GPIOD_LINE_BULK_MAX_LINES], num_lines = 0, offset;
	bool active_low = false;
	struct timespec timeout = { 10, 0 };
	int ret, event_type;
	struct mon_ctx ctx;

	printf("check on the mic\n");

	memset(&ctx, 0, sizeof(ctx));

	event_type = GPIOD_CTXLESS_EVENT_BOTH_EDGES;

        // XXX
        offset = 5;
	num_lines = 1;
        offsets[0] = offset;

	// ret = gpiod_ctxless_event_monitor_multiple(argv[0], event_type,
	ret = gpiod_ctxless_event_monitor_multiple("gpiochip0", event_type,
						   offsets, num_lines,
						   active_low, "gpiomon",
						   &timeout, NULL,
						   callback, &ctx);
	if (ret)
		printf("error waiting for events");

	return(0);
}

