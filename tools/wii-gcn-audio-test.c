// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include <alloca.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double seconds(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* Nonblocking direct PCM exercise: underruns and stalled DMA are failures. */
int main(int argc, char **argv)
{
	snd_pcm_t *pcm;
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t period, buffer;
	unsigned int rate, duration, periods, actual_rate;
	unsigned long frames, sent = 0;
	unsigned char samples[16384];
	double began, elapsed;
	int ret, direction = 0, tone, mmap_mode;

	if (argc != 7 && argc != 8) {
		fprintf(stderr, "Usage: %s DEVICE RATE SECONDS PERIOD_FRAMES PERIODS silence|tone [rw|mmap]\n", argv[0]);
		return 2;
	}
	rate = strtoul(argv[2], NULL, 10);
	duration = strtoul(argv[3], NULL, 10);
	period = strtoul(argv[4], NULL, 10);
	periods = strtoul(argv[5], NULL, 10);
	tone = !strcmp(argv[6], "tone");
	mmap_mode = argc == 8 && !strcmp(argv[7], "mmap");
	if ((!tone && strcmp(argv[6], "silence")) ||
	    (argc == 8 && !mmap_mode && strcmp(argv[7], "rw")))
		return 2;
	if ((rate != 32000 && rate != 48000) || !duration || duration > 120 ||
	    period < 256 || period > 4096 || periods < 2 || periods > 32)
		return 2;
	frames = rate * duration;
	buffer = period * periods;
	ret = snd_pcm_open(&pcm, argv[1], SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (ret < 0) {
		fprintf(stderr, "open: %s\n", snd_strerror(ret));
		return 1;
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_sw_params_alloca(&sw);
#define CHECK(call) do { ret = (call); if (ret < 0) { \
	fprintf(stderr, "%s: %s\n", #call, snd_strerror(ret)); goto fail; } } while (0)
	CHECK(snd_pcm_hw_params_any(pcm, hw));
	CHECK(snd_pcm_hw_params_set_access(pcm, hw, mmap_mode ?
		SND_PCM_ACCESS_MMAP_INTERLEAVED : SND_PCM_ACCESS_RW_INTERLEAVED));
	CHECK(snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_BE));
	CHECK(snd_pcm_hw_params_set_channels(pcm, hw, 2));
	actual_rate = rate;
	CHECK(snd_pcm_hw_params_set_rate_near(pcm, hw, &actual_rate, &direction));
	if (actual_rate != rate)
		goto fail;
	CHECK(snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &direction));
	CHECK(snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer));
	CHECK(snd_pcm_hw_params(pcm, hw));
	CHECK(snd_pcm_sw_params_current(pcm, sw));
	CHECK(snd_pcm_sw_params_set_start_threshold(pcm, sw, buffer));
	CHECK(snd_pcm_sw_params_set_avail_min(pcm, sw, period));
	CHECK(snd_pcm_sw_params(pcm, sw));
	printf("rate=%u period=%lu buffer=%lu frames=%lu mode=%s\n",
	       rate, period, buffer, frames, tone ? "tone" : "silence");
	fflush(stdout);
	began = seconds();
	while (sent < frames) {
		unsigned long chunk = frames - sent, offset = 0;

		if (chunk > 4096)
			chunk = 4096;
		for (unsigned long i = 0; i < chunk; i++) {
			/* Quiet stereo markers: 440 Hz left, 660 Hz right. */
			for (unsigned int ch = 0; ch < 2; ch++) {
				int16_t sample = tone ? (int16_t)(1200 *
					sin(2 * 3.141592653589793 * (ch ? 660 : 440) *
					    (sent + i) / rate)) : 0;
				unsigned int at = i * 4 + ch * 2;

				samples[at] = (uint16_t)sample >> 8;
				samples[at + 1] = (uint16_t)sample;
			}
		}
		while (offset < chunk) {
			snd_pcm_sframes_t n = mmap_mode ?
				snd_pcm_mmap_writei(pcm, samples + offset * 4, chunk - offset) :
				snd_pcm_writei(pcm, samples + offset * 4, chunk - offset);

			if (n > 0)
				offset += n;
			else if (n == -EAGAIN || n == -EINTR)
				snd_pcm_wait(pcm, 100);
			else {
				fprintf(stderr, "write: %s\n", snd_strerror(n));
				goto fail;
			}
			if (seconds() - began > duration + 5) {
				fprintf(stderr, "FAIL: playback deadline\n");
				goto fail;
			}
		}
		sent += chunk;
	}
	printf("submitted in %.3f seconds; draining\n", seconds() - began);
	fflush(stdout);
	while ((ret = snd_pcm_drain(pcm)) == -EAGAIN || ret == -EINTR) {
		struct timespec pause = { .tv_nsec = 1000000 };

		/* Nonblocking DRAIN may return EAGAIN even once SETUP is reached. */
		if (snd_pcm_state(pcm) == SND_PCM_STATE_SETUP) {
			ret = 0;
			break;
		}
		if (seconds() - began > duration + 5) {
			snd_pcm_sframes_t delay = 0;

			snd_pcm_delay(pcm, &delay);
			fprintf(stderr, "FAIL: drain deadline, state=%s delay=%ld avail=%ld\n",
				snd_pcm_state_name(snd_pcm_state(pcm)), delay,
				snd_pcm_avail_update(pcm));
			goto fail;
		}
		nanosleep(&pause, NULL);
	}
	if (ret < 0) {
		fprintf(stderr, "drain: %s\n", snd_strerror(ret));
		goto fail;
	}
	elapsed = seconds() - began;
	snd_pcm_close(pcm);
	if (elapsed < duration * .95 || elapsed > duration * 1.05 + .1) {
		fprintf(stderr, "FAIL: duration %.3f seconds, expected %u\n", elapsed, duration);
		return 1;
	}
	printf("PASS: %lu frames, %.3f seconds, no underrun\n", sent, elapsed);
	return 0;
fail:
	snd_pcm_drop(pcm);
	snd_pcm_close(pcm);
	return 1;
}
