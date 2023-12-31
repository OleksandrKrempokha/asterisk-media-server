/*
 * Trismedia -- An open source telephony toolkit.
 *
 * Copyright (C) 2007 - 2008, Russell Bryant
 *
 * Russell Bryant <russell@digium.com>
 *
 * See http://www.trismedia.org for more information about
 * the Trismedia project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*!
 * \file
 * \brief Jack Application
 *
 * \author Russell Bryant <russell@digium.com>
 *
 * This is an application to connect an Trismedia channel to an input
 * and output jack port so that the audio can be processed through
 * another application, or to play audio from another application.
 *
 * \arg http://www.jackaudio.org/
 *
 * \note To install libresample, check it out of the following repository:
 * <code>$ svn co http://svn.digium.com/svn/thirdparty/libresample/trunk</code>
 *
 * \ingroup applications
 */

/*** MODULEINFO
	<depend>jack</depend>
	<depend>resample</depend>
 ***/

#include "trismedia.h"

TRISMEDIA_FILE_VERSION(__FILE__, "$Revision: 164202 $")

#include <limits.h>

#include <jack/jack.h>
#include <jack/ringbuffer.h>

#include <libresample.h>

#include "trismedia/module.h"
#include "trismedia/channel.h"
#include "trismedia/strings.h"
#include "trismedia/lock.h"
#include "trismedia/app.h"
#include "trismedia/pbx.h"
#include "trismedia/audiohook.h"

#define RESAMPLE_QUALITY 1

#define RINGBUFFER_SIZE 16384

/*! \brief Common options between the Jack() app and JACK_HOOK() function */
#define COMMON_OPTIONS \
"    s(<name>) - Connect to the specified jack server name.\n" \
"    i(<name>) - Connect the output port that gets created to the specified\n" \
"                jack input port.\n" \
"    o(<name>) - Connect the input port that gets created to the specified\n" \
"                jack output port.\n" \
"    n         - Do not automatically start the JACK server if it is not already\n" \
"                running.\n" \
"    c(<name>) - By default, Trismedia will use the channel name for the jack client\n" \
"                name.  Use this option to specify a custom client name.\n"
/*** DOCUMENTATION
	<application name="JACK" language="en_US">
		<synopsis>
			Jack Audio Connection Kit
		</synopsis>
		<syntax>
			<parameter name="options" required="false">
				<optionlist>
					<option name="s">
						<argument name="name" required="true">
							<para>Connect to the specified jack server name</para>
						</argument>
					</option>
					<option name="i">
						<argument name="name" required="true">
							<para>Connect the output port that gets created to the specified jack input port</para>
						</argument>
					</option>
					<option name="o">
						<argument name="name" required="true">
							<para>Connect the input port that gets created to the specified jack output port</para>
						</argument>
					</option>
					<option name="c">
						<argument name="name" required="true">
							<para>By default, Trismedia will use the channel name for the jack client name.</para>
							<para>Use this option to specify a custom client name.</para>
						</argument>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>When executing this application, two jack ports will be created; 
			one input and one output. Other applications can be hooked up to 
			these ports to access audio coming from, or being send to the channel.</para>
		</description>
	</application>
 ***/
static char *jack_app = "JACK";

struct jack_data {
	TRIS_DECLARE_STRING_FIELDS(
		TRIS_STRING_FIELD(server_name);
		TRIS_STRING_FIELD(client_name);
		TRIS_STRING_FIELD(connect_input_port);
		TRIS_STRING_FIELD(connect_output_port);
	);
	jack_client_t *client;
	jack_port_t *input_port;
	jack_port_t *output_port;
	jack_ringbuffer_t *input_rb;
	jack_ringbuffer_t *output_rb;
	void *output_resampler;
	double output_resample_factor;
	void *input_resampler;
	double input_resample_factor;
	unsigned int stop:1;
	unsigned int has_audiohook:1;
	unsigned int no_start_server:1;
	/*! Only used with JACK_HOOK */
	struct tris_audiohook audiohook;
};

static const struct {
	jack_status_t status;
	const char *str;
} jack_status_table[] = {
	{ JackFailure,        "Failure" },
	{ JackInvalidOption,  "Invalid Option" },
	{ JackNameNotUnique,  "Name Not Unique" },
	{ JackServerStarted,  "Server Started" },
	{ JackServerFailed,   "Server Failed" },
	{ JackServerError,    "Server Error" },
	{ JackNoSuchClient,   "No Such Client" },
	{ JackLoadFailure,    "Load Failure" },
	{ JackInitFailure,    "Init Failure" },
	{ JackShmFailure,     "Shared Memory Access Failure" },
	{ JackVersionError,   "Version Mismatch" },
};

static const char *jack_status_to_str(jack_status_t status)
{
	int i;

	for (i = 0; i < ARRAY_LEN(jack_status_table); i++) {
		if (jack_status_table[i].status == status)
			return jack_status_table[i].str;
	}

	return "Unknown Error";
}

static void log_jack_status(const char *prefix, jack_status_t status)
{
	struct tris_str *str = tris_str_alloca(512);
	int i, first = 0;

	for (i = 0; i < (sizeof(status) * 8); i++) {
		if (!(status & (1 << i)))
			continue;

		if (!first) {
			tris_str_set(&str, 0, "%s", jack_status_to_str((1 << i)));
			first = 1;
		} else
			tris_str_append(&str, 0, ", %s", jack_status_to_str((1 << i)));
	}
	
	tris_log(LOG_NOTICE, "%s: %s\n", prefix, tris_str_buffer(str));
}

static int alloc_resampler(struct jack_data *jack_data, int input)
{
	double from_srate, to_srate, jack_srate;
	void **resampler;
	double *resample_factor;

	if (input && jack_data->input_resampler)
		return 0;

	if (!input && jack_data->output_resampler)
		return 0;

	jack_srate = jack_get_sample_rate(jack_data->client);

	/* XXX Hard coded 8 kHz */

	to_srate = input ? 8000.0 : jack_srate; 
	from_srate = input ? jack_srate : 8000.0;

	resample_factor = input ? &jack_data->input_resample_factor : 
		&jack_data->output_resample_factor;

	if (from_srate == to_srate) {
		/* Awesome!  The jack sample rate is the same as ours.
		 * Resampling isn't needed. */
		*resample_factor = 1.0;
		return 0;
	}

	*resample_factor = to_srate / from_srate;

	resampler = input ? &jack_data->input_resampler :
		&jack_data->output_resampler;

	if (!(*resampler = resample_open(RESAMPLE_QUALITY, 
		*resample_factor, *resample_factor))) {
		tris_log(LOG_ERROR, "Failed to open %s resampler\n", 
			input ? "input" : "output");
		return -1;
	}

	return 0;
}

/*!
 * \brief Handle jack input port
 *
 * Read nframes number of samples from the input buffer, resample it
 * if necessary, and write it into the appropriate ringbuffer. 
 */
static void handle_input(void *buf, jack_nframes_t nframes, 
	struct jack_data *jack_data)
{
	short s_buf[nframes];
	float *in_buf = buf;
	size_t res;
	int i;
	size_t write_len = sizeof(s_buf);

	if (jack_data->input_resampler) {
		int total_in_buf_used = 0;
		int total_out_buf_used = 0;
		float f_buf[nframes + 1];

		memset(f_buf, 0, sizeof(f_buf));

		while (total_in_buf_used < nframes) {
			int in_buf_used;
			int out_buf_used;

			out_buf_used = resample_process(jack_data->input_resampler,
				jack_data->input_resample_factor,
				&in_buf[total_in_buf_used], nframes - total_in_buf_used,
				0, &in_buf_used,
				&f_buf[total_out_buf_used], ARRAY_LEN(f_buf) - total_out_buf_used);

			if (out_buf_used < 0)
				break;

			total_out_buf_used += out_buf_used;
			total_in_buf_used += in_buf_used;
	
			if (total_out_buf_used == ARRAY_LEN(f_buf)) {
				tris_log(LOG_ERROR, "Output buffer filled ... need to increase its size, "
					"nframes '%d', total_out_buf_used '%d'\n", nframes, total_out_buf_used);
				break;
			}
		}

		for (i = 0; i < total_out_buf_used; i++)
			s_buf[i] = f_buf[i] * (SHRT_MAX / 1.0);
		
		write_len = total_out_buf_used * sizeof(int16_t);
	} else {
		/* No resampling needed */

		for (i = 0; i < nframes; i++)
			s_buf[i] = in_buf[i] * (SHRT_MAX / 1.0);
	}

	res = jack_ringbuffer_write(jack_data->input_rb, (const char *) s_buf, write_len);
	if (res != write_len) {
		tris_debug(2, "Tried to write %d bytes to the ringbuffer, but only wrote %d\n",
			(int) sizeof(s_buf), (int) res);
	}
}

/*!
 * \brief Handle jack output port
 *
 * Read nframes number of samples from the ringbuffer and write it out to the
 * output port buffer.
 */
static void handle_output(void *buf, jack_nframes_t nframes, 
	struct jack_data *jack_data)
{
	size_t res, len;

	len = nframes * sizeof(float);

	res = jack_ringbuffer_read(jack_data->output_rb, buf, len);

	if (len != res) {
		tris_debug(2, "Wanted %d bytes to send to the output port, "
			"but only got %d\n", (int) len, (int) res);
	}
}

static int jack_process(jack_nframes_t nframes, void *arg)
{
	struct jack_data *jack_data = arg;
	void *input_port_buf, *output_port_buf;

	if (!jack_data->input_resample_factor)
		alloc_resampler(jack_data, 1);

	input_port_buf = jack_port_get_buffer(jack_data->input_port, nframes);
	handle_input(input_port_buf, nframes, jack_data);

	output_port_buf = jack_port_get_buffer(jack_data->output_port, nframes);
	handle_output(output_port_buf, nframes, jack_data);

	return 0;
}

static void jack_shutdown(void *arg)
{
	struct jack_data *jack_data = arg;

	jack_data->stop = 1;
}

static struct jack_data *destroy_jack_data(struct jack_data *jack_data)
{
	if (jack_data->input_port) {
		jack_port_unregister(jack_data->client, jack_data->input_port);
		jack_data->input_port = NULL;
	}

	if (jack_data->output_port) {
		jack_port_unregister(jack_data->client, jack_data->output_port);
		jack_data->output_port = NULL;
	}

	if (jack_data->client) {
		jack_client_close(jack_data->client);
		jack_data->client = NULL;
	}

	if (jack_data->input_rb) {
		jack_ringbuffer_free(jack_data->input_rb);
		jack_data->input_rb = NULL;
	}

	if (jack_data->output_rb) {
		jack_ringbuffer_free(jack_data->output_rb);
		jack_data->output_rb = NULL;
	}

	if (jack_data->output_resampler) {
		resample_close(jack_data->output_resampler);
		jack_data->output_resampler = NULL;
	}
	
	if (jack_data->input_resampler) {
		resample_close(jack_data->input_resampler);
		jack_data->input_resampler = NULL;
	}

	if (jack_data->has_audiohook)
		tris_audiohook_destroy(&jack_data->audiohook);

	tris_string_field_free_memory(jack_data);

	tris_free(jack_data);

	return NULL;
}

static int init_jack_data(struct tris_channel *chan, struct jack_data *jack_data)
{
	const char *client_name;
	jack_status_t status = 0;
	jack_options_t jack_options = JackNullOption;

	if (!tris_strlen_zero(jack_data->client_name)) {
		client_name = jack_data->client_name;
	} else {
		tris_channel_lock(chan);
		client_name = tris_strdupa(chan->name);
		tris_channel_unlock(chan);
	}

	if (!(jack_data->output_rb = jack_ringbuffer_create(RINGBUFFER_SIZE)))
		return -1;

	if (!(jack_data->input_rb = jack_ringbuffer_create(RINGBUFFER_SIZE)))
		return -1;

	if (jack_data->no_start_server)
		jack_options |= JackNoStartServer;

	if (!tris_strlen_zero(jack_data->server_name)) {
		jack_options |= JackServerName;
		jack_data->client = jack_client_open(client_name, jack_options, &status,
			jack_data->server_name);
	} else {
		jack_data->client = jack_client_open(client_name, jack_options, &status);
	}

	if (status)
		log_jack_status("Client Open Status", status);

	if (!jack_data->client)
		return -1;

	jack_data->input_port = jack_port_register(jack_data->client, "input",
		JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsTerminal, 0);
	if (!jack_data->input_port) {
		tris_log(LOG_ERROR, "Failed to create input port for jack port\n");
		return -1;
	}

	jack_data->output_port = jack_port_register(jack_data->client, "output",
		JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);
	if (!jack_data->output_port) {
		tris_log(LOG_ERROR, "Failed to create output port for jack port\n");
		return -1;
	}

	if (jack_set_process_callback(jack_data->client, jack_process, jack_data)) {
		tris_log(LOG_ERROR, "Failed to register process callback with jack client\n");
		return -1;
	}

	jack_on_shutdown(jack_data->client, jack_shutdown, jack_data);

	if (jack_activate(jack_data->client)) {
		tris_log(LOG_ERROR, "Unable to activate jack client\n");
		return -1;
	}

	while (!tris_strlen_zero(jack_data->connect_input_port)) {
		const char **ports;
		int i;

		ports = jack_get_ports(jack_data->client, jack_data->connect_input_port,
			NULL, JackPortIsInput);

		if (!ports) {
			tris_log(LOG_ERROR, "No input port matching '%s' was found\n",
				jack_data->connect_input_port);
			break;
		}

		for (i = 0; ports[i]; i++) {
			tris_debug(1, "Found port '%s' that matched specified input port '%s'\n",
				ports[i], jack_data->connect_input_port);
		}

		if (jack_connect(jack_data->client, jack_port_name(jack_data->output_port), ports[0])) {
			tris_log(LOG_ERROR, "Failed to connect '%s' to '%s'\n", ports[0],
				jack_port_name(jack_data->output_port));
		} else {
			tris_debug(1, "Connected '%s' to '%s'\n", ports[0],
				jack_port_name(jack_data->output_port));
		}

		free((void *) ports);

		break;
	}

	while (!tris_strlen_zero(jack_data->connect_output_port)) {
		const char **ports;
		int i;

		ports = jack_get_ports(jack_data->client, jack_data->connect_output_port,
			NULL, JackPortIsOutput);

		if (!ports) {
			tris_log(LOG_ERROR, "No output port matching '%s' was found\n",
				jack_data->connect_output_port);
			break;
		}

		for (i = 0; ports[i]; i++) {
			tris_debug(1, "Found port '%s' that matched specified output port '%s'\n",
				ports[i], jack_data->connect_output_port);
		}

		if (jack_connect(jack_data->client, ports[0], jack_port_name(jack_data->input_port))) {
			tris_log(LOG_ERROR, "Failed to connect '%s' to '%s'\n", ports[0],
				jack_port_name(jack_data->input_port));
		} else {
			tris_debug(1, "Connected '%s' to '%s'\n", ports[0],
				jack_port_name(jack_data->input_port));
		}

		free((void *) ports);

		break;
	}

	return 0;
}

static int queue_voice_frame(struct jack_data *jack_data, struct tris_frame *f)
{
	float f_buf[f->samples * 8];
	size_t f_buf_used = 0;
	int i;
	int16_t *s_buf = f->data.ptr;
	size_t res;

	memset(f_buf, 0, sizeof(f_buf));

	if (!jack_data->output_resample_factor)
		alloc_resampler(jack_data, 0);

	if (jack_data->output_resampler) {
		float in_buf[f->samples];
		int total_in_buf_used = 0;
		int total_out_buf_used = 0;

		memset(in_buf, 0, sizeof(in_buf));

		for (i = 0; i < f->samples; i++)
			in_buf[i] = s_buf[i] * (1.0 / SHRT_MAX);

		while (total_in_buf_used < ARRAY_LEN(in_buf)) {
			int in_buf_used;
			int out_buf_used;

			out_buf_used = resample_process(jack_data->output_resampler, 
				jack_data->output_resample_factor,
				&in_buf[total_in_buf_used], ARRAY_LEN(in_buf) - total_in_buf_used, 
				0, &in_buf_used, 
				&f_buf[total_out_buf_used], ARRAY_LEN(f_buf) - total_out_buf_used);

			if (out_buf_used < 0)
				break;

			total_out_buf_used += out_buf_used;
			total_in_buf_used += in_buf_used;

			if (total_out_buf_used == ARRAY_LEN(f_buf)) {
				tris_log(LOG_ERROR, "Output buffer filled ... need to increase its size\n");
				break;
			}
		}

		f_buf_used = total_out_buf_used;
		if (f_buf_used > ARRAY_LEN(f_buf))
			f_buf_used = ARRAY_LEN(f_buf);
	} else {
		/* No resampling needed */

		for (i = 0; i < f->samples; i++)
			f_buf[i] = s_buf[i] * (1.0 / SHRT_MAX);

		f_buf_used = f->samples;
	}

	res = jack_ringbuffer_write(jack_data->output_rb, (const char *) f_buf, f_buf_used * sizeof(float));
	if (res != (f_buf_used * sizeof(float))) {
		tris_debug(2, "Tried to write %d bytes to the ringbuffer, but only wrote %d\n",
			(int) (f_buf_used * sizeof(float)), (int) res);
	}

	return 0;
}

/*!
 * \brief handle jack audio
 *
 * \param[in]  chan The Trismedia channel to write the frames to if no output frame
 *             is provided.
 * \param[in]  jack_data This is the jack_data struct that contains the input
 *             ringbuffer that audio will be read from.
 * \param[out] out_frame If this argument is non-NULL, then assuming there is
 *             enough data avilable in the ringbuffer, the audio in this frame
 *             will get replaced with audio from the input buffer.  If there is
 *             not enough data available to read at this time, then the frame
 *             data gets zeroed out.
 *
 * Read data from the input ringbuffer, which is the properly resampled audio
 * that was read from the jack input port.  Write it to the channel in 20 ms frames,
 * or fill up an output frame instead if one is provided.
 *
 * \return Nothing.
 */
static void handle_jack_audio(struct tris_channel *chan, struct jack_data *jack_data,
	struct tris_frame *out_frame)
{	
	short buf[160];
	struct tris_frame f = {
		.frametype = TRIS_FRAME_VOICE,
		.subclass = TRIS_FORMAT_SLINEAR,
		.src = "JACK",
		.data.ptr = buf,
		.datalen = sizeof(buf),
		.samples = ARRAY_LEN(buf),
	};

	for (;;) {
		size_t res, read_len;
		char *read_buf;

		read_len = out_frame ? out_frame->datalen : sizeof(buf);
		read_buf = out_frame ? out_frame->data.ptr : buf;

		res = jack_ringbuffer_read_space(jack_data->input_rb);

		if (res < read_len) {
			/* Not enough data ready for another frame, move on ... */
			if (out_frame) {
				tris_debug(1, "Sending an empty frame for the JACK_HOOK\n");
				memset(out_frame->data.ptr, 0, out_frame->datalen);
			}
			break;
		}

		res = jack_ringbuffer_read(jack_data->input_rb, (char *) read_buf, read_len);

		if (res < read_len) {
			tris_log(LOG_ERROR, "Error reading from ringbuffer, even though it said there was enough data\n");
			break;
		}

		if (out_frame) {
			/* If an output frame was provided, then we just want to fill up the
			 * buffer in that frame and return. */
			break;
		}

		tris_write(chan, &f);
	}
}

enum {
	OPT_SERVER_NAME =    (1 << 0),
	OPT_INPUT_PORT =     (1 << 1),
	OPT_OUTPUT_PORT =    (1 << 2),
	OPT_NOSTART_SERVER = (1 << 3),
	OPT_CLIENT_NAME =    (1 << 4),
};

enum {
	OPT_ARG_SERVER_NAME,
	OPT_ARG_INPUT_PORT,
	OPT_ARG_OUTPUT_PORT,
	OPT_ARG_CLIENT_NAME,

	/* Must be the last element */
	OPT_ARG_ARRAY_SIZE,
};

TRIS_APP_OPTIONS(jack_exec_options, BEGIN_OPTIONS
	TRIS_APP_OPTION_ARG('s', OPT_SERVER_NAME, OPT_ARG_SERVER_NAME),
	TRIS_APP_OPTION_ARG('i', OPT_INPUT_PORT, OPT_ARG_INPUT_PORT),
	TRIS_APP_OPTION_ARG('o', OPT_OUTPUT_PORT, OPT_ARG_OUTPUT_PORT),
	TRIS_APP_OPTION('n', OPT_NOSTART_SERVER),
	TRIS_APP_OPTION_ARG('c', OPT_CLIENT_NAME, OPT_ARG_CLIENT_NAME),
END_OPTIONS );

static struct jack_data *jack_data_alloc(void)
{
	struct jack_data *jack_data;

	if (!(jack_data = tris_calloc(1, sizeof(*jack_data))))
		return NULL;
	
	if (tris_string_field_init(jack_data, 32)) {
		tris_free(jack_data);
		return NULL;
	}

	return jack_data;
}

/*!
 * \note This must be done before calling init_jack_data().
 */
static int handle_options(struct jack_data *jack_data, const char *__options_str)
{
	struct tris_flags options = { 0, };
	char *option_args[OPT_ARG_ARRAY_SIZE];
	char *options_str;

	options_str = tris_strdupa(__options_str);

	tris_app_parse_options(jack_exec_options, &options, option_args, options_str);

	if (tris_test_flag(&options, OPT_SERVER_NAME)) {
		if (!tris_strlen_zero(option_args[OPT_ARG_SERVER_NAME]))
			tris_string_field_set(jack_data, server_name, option_args[OPT_ARG_SERVER_NAME]);
		else {
			tris_log(LOG_ERROR, "A server name must be provided with the s() option\n");
			return -1;
		}
	}

	if (tris_test_flag(&options, OPT_CLIENT_NAME)) {
		if (!tris_strlen_zero(option_args[OPT_ARG_CLIENT_NAME]))
			tris_string_field_set(jack_data, client_name, option_args[OPT_ARG_CLIENT_NAME]);
		else {
			tris_log(LOG_ERROR, "A client name must be provided with the c() option\n");
			return -1;
		}
	}

	if (tris_test_flag(&options, OPT_INPUT_PORT)) {
		if (!tris_strlen_zero(option_args[OPT_ARG_INPUT_PORT]))
			tris_string_field_set(jack_data, connect_input_port, option_args[OPT_ARG_INPUT_PORT]);
		else {
			tris_log(LOG_ERROR, "A name must be provided with the i() option\n");
			return -1;
		}
	}

	if (tris_test_flag(&options, OPT_OUTPUT_PORT)) {
		if (!tris_strlen_zero(option_args[OPT_ARG_OUTPUT_PORT]))
			tris_string_field_set(jack_data, connect_output_port, option_args[OPT_ARG_OUTPUT_PORT]);
		else {
			tris_log(LOG_ERROR, "A name must be provided with the o() option\n");
			return -1;
		}
	}

	jack_data->no_start_server = tris_test_flag(&options, OPT_NOSTART_SERVER) ? 1 : 0;

	return 0;
}

static int jack_exec(struct tris_channel *chan, void *data)
{
	struct jack_data *jack_data;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(options);
	);

	if (!(jack_data = jack_data_alloc()))
		return -1;

	args.options = data;

	if (!tris_strlen_zero(args.options) && handle_options(jack_data, args.options)) {
		destroy_jack_data(jack_data);
		return -1;
	}

	if (init_jack_data(chan, jack_data)) {
		destroy_jack_data(jack_data);
		return -1;
	}

	if (tris_set_read_format(chan, TRIS_FORMAT_SLINEAR)) {
		destroy_jack_data(jack_data);
		return -1;
	}

	if (tris_set_write_format(chan, TRIS_FORMAT_SLINEAR)) {
		destroy_jack_data(jack_data);
		return -1;
	}

	while (!jack_data->stop) {
		struct tris_frame *f;

		tris_waitfor(chan, -1);

		f = tris_read(chan);
		if (!f) {
			jack_data->stop = 1;
			continue;
		}

		switch (f->frametype) {
		case TRIS_FRAME_CONTROL:
			if (f->subclass == TRIS_CONTROL_HANGUP)
				jack_data->stop = 1;
			break;
		case TRIS_FRAME_VOICE:
			queue_voice_frame(jack_data, f);
		default:
			break;
		}

		tris_frfree(f);

		handle_jack_audio(chan, jack_data, NULL);
	}

	jack_data = destroy_jack_data(jack_data);

	return 0;
}

static void jack_hook_ds_destroy(void *data)
{
	struct jack_data *jack_data = data;

	destroy_jack_data(jack_data);
}

static const struct tris_datastore_info jack_hook_ds_info = {
	.type = "JACK_HOOK",
	.destroy = jack_hook_ds_destroy,
};

static int jack_hook_callback(struct tris_audiohook *audiohook, struct tris_channel *chan, 
	struct tris_frame *frame, enum tris_audiohook_direction direction)
{
	struct tris_datastore *datastore;
	struct jack_data *jack_data;

	if (audiohook->status == TRIS_AUDIOHOOK_STATUS_DONE)
		return 0;

	if (direction != TRIS_AUDIOHOOK_DIRECTION_READ)
		return 0;

	if (frame->frametype != TRIS_FRAME_VOICE)
		return 0;

	if (frame->subclass != TRIS_FORMAT_SLINEAR) {
		tris_log(LOG_WARNING, "Expected frame in SLINEAR for the audiohook, but got format %d\n",
			frame->subclass);
		return 0;
	}

	tris_channel_lock(chan);

	if (!(datastore = tris_channel_datastore_find(chan, &jack_hook_ds_info, NULL))) {
		tris_log(LOG_ERROR, "JACK_HOOK datastore not found for '%s'\n", chan->name);
		tris_channel_unlock(chan);
		return -1;
	}

	jack_data = datastore->data;

	queue_voice_frame(jack_data, frame);

	handle_jack_audio(chan, jack_data, frame);

	tris_channel_unlock(chan);

	return 0;
}

static int enable_jack_hook(struct tris_channel *chan, char *data)
{
	struct tris_datastore *datastore;
	struct jack_data *jack_data = NULL;
	TRIS_DECLARE_APP_ARGS(args,
		TRIS_APP_ARG(mode);
		TRIS_APP_ARG(options);
	);

	TRIS_STANDARD_APP_ARGS(args, data);

	tris_channel_lock(chan);

	if ((datastore = tris_channel_datastore_find(chan, &jack_hook_ds_info, NULL))) {
		tris_log(LOG_ERROR, "JACK_HOOK already enabled for '%s'\n", chan->name);
		goto return_error;
	}

	if (tris_strlen_zero(args.mode) || strcasecmp(args.mode, "manipulate")) {
		tris_log(LOG_ERROR, "'%s' is not a supported mode.  Only manipulate is supported.\n", 
			S_OR(args.mode, "<none>"));
		goto return_error;
	}

	if (!(jack_data = jack_data_alloc()))
		goto return_error;

	if (!tris_strlen_zero(args.options) && handle_options(jack_data, args.options))
		goto return_error;

	if (init_jack_data(chan, jack_data))
		goto return_error;

	if (!(datastore = tris_datastore_alloc(&jack_hook_ds_info, NULL)))
		goto return_error;

	jack_data->has_audiohook = 1;
	tris_audiohook_init(&jack_data->audiohook, TRIS_AUDIOHOOK_TYPE_MANIPULATE, "JACK_HOOK");
	jack_data->audiohook.manipulate_callback = jack_hook_callback;

	datastore->data = jack_data;

	if (tris_audiohook_attach(chan, &jack_data->audiohook))
		goto return_error;

	if (tris_channel_datastore_add(chan, datastore))
		goto return_error;

	tris_channel_unlock(chan);

	return 0;

return_error:
	tris_channel_unlock(chan);

	if (jack_data)
		destroy_jack_data(jack_data);

	return -1;
}

static int disable_jack_hook(struct tris_channel *chan)
{
	struct tris_datastore *datastore;
	struct jack_data *jack_data;

	tris_channel_lock(chan);

	if (!(datastore = tris_channel_datastore_find(chan, &jack_hook_ds_info, NULL))) {
		tris_channel_unlock(chan);
		tris_log(LOG_WARNING, "No JACK_HOOK found to disable\n");
		return -1;
	}

	tris_channel_datastore_remove(chan, datastore);

	jack_data = datastore->data;
	tris_audiohook_detach(&jack_data->audiohook);

	/* Keep the channel locked while we destroy the datastore, so that we can
	 * ensure that all of the jack stuff is stopped just in case another frame
	 * tries to come through the audiohook callback. */
	tris_datastore_free(datastore);

	tris_channel_unlock(chan);

	return 0;
}

static int jack_hook_write(struct tris_channel *chan, const char *cmd, char *data, 
	const char *value)
{
	int res;

	if (!strcasecmp(value, "on"))
		res = enable_jack_hook(chan, data);
	else if (!strcasecmp(value, "off"))
		res = disable_jack_hook(chan);
	else {
		tris_log(LOG_ERROR, "'%s' is not a valid value for JACK_HOOK()\n", value);	
		res = -1;
	}

	return res;
}

static struct tris_custom_function jack_hook_function = {
	.name = "JACK_HOOK",
	.synopsis = "Enable a jack hook on a channel",
	.syntax = "JACK_HOOK(<mode>,[options])",
	.desc =
	"   The JACK_HOOK allows turning on or off jack connectivity to this channel.\n"
	"When the JACK_HOOK is turned on, jack ports will get created that allow\n"
	"access to the audio stream for this channel.  The mode specifies which mode\n"
	"this hook should run in.  A mode must be specified when turning the JACK_HOOK.\n"
	"on.  However, all arguments are optional when turning it off.\n"
	"\n"
	"   Valid modes are:\n"
#if 0
	/* XXX TODO */
	"    spy -        Create a read-only audio hook.  Only an output jack port will\n"
	"                 get created.\n"
	"    whisper -    Create a write-only audio hook.  Only an input jack port will\n"
	"                 get created.\n"
#endif
	"    manipulate - Create a read/write audio hook.  Both an input and an output\n"
	"                 jack port will get created.  Audio from the channel will be\n"
	"                 sent out the output port and will be replaced by the audio\n"
	"                 coming in on the input port as it gets passed on.\n"
	"\n"
	"   Valid options are:\n"
	COMMON_OPTIONS
	"\n"
	" Examples:\n"
	"   To turn on the JACK_HOOK,\n"
	"     Set(JACK_HOOK(manipulate,i(pure_data_0:input0)o(pure_data_0:output0))=on)\n"
	"   To turn off the JACK_HOOK,\n"
	"     Set(JACK_HOOK()=off)\n"
	"",
	.write = jack_hook_write,
};

static int unload_module(void)
{
	int res;

	res = tris_unregister_application(jack_app);
	res |= tris_custom_function_unregister(&jack_hook_function);

	return res;
}

static int load_module(void)
{
	if (tris_register_application_xml(jack_app, jack_exec)) {
		return TRIS_MODULE_LOAD_DECLINE;
	}

	if (tris_custom_function_register(&jack_hook_function)) {
		tris_unregister_application(jack_app);
		return TRIS_MODULE_LOAD_DECLINE;
	}

	return TRIS_MODULE_LOAD_SUCCESS;
}

TRIS_MODULE_INFO_STANDARD(TRISMEDIA_GPL_KEY, "JACK Interface");
