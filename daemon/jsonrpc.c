/* Code for JSON_RPC API */
/* eg: { "method" : "dev-echo", "params" : [ "hello", "Arabella!" ], "id" : "1" } */
#include "json.h"
#include "jsonrpc.h"
#include "lightningd.h"
#include "log.h"
#include <ccan/array_size/array_size.h>
#include <ccan/err/err.h>
#include <ccan/io/io.h>
#include <ccan/str/hex/hex.h>
#include <ccan/tal/str/str.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

struct json_output {
	struct list_node list;
	const char *json;
};

static void finish_jcon(struct io_conn *conn, struct json_connection *jcon)
{
	log_debug(jcon->log, "Closing (%s)", strerror(errno));
	if (jcon->current) {
		log_unusual(jcon->log, "Abandoning current command");
		jcon->current->jcon = NULL;
	}
}

static void json_help(struct command *cmd,
		      const char *buffer, const jsmntok_t *params);

static const struct json_command help_command = {
	"help",
	json_help,
	"describe commands",
	"[<command>] if specified gives details about a single command."
};

static void json_echo(struct command *cmd,
		      const char *buffer, const jsmntok_t *params)
{
	struct json_result *response = new_json_result(cmd);

	json_object_start(response, NULL);
	json_add_num(response, "num", params->size);
	json_add_literal(response, "echo",
			 json_tok_contents(buffer, params),
			 json_tok_len(params));
	json_object_end(response);
	command_success(cmd, response);
}

static const struct json_command echo_command = {
	"dev-echo",
	json_echo,
	"echo parameters",
	"Simple echo test for developers"
};

static void json_stop(struct command *cmd,
		      const char *buffer, const jsmntok_t *params)
{
	struct json_result *response = new_json_result(cmd);

	/* This can't have closed yet! */
	cmd->jcon->stop = true;
	json_add_string(response, NULL, "Shutting down");
	command_success(cmd, response);
}

static const struct json_command stop_command = {
	"stop",
	json_stop,
	"Shutdown the lightningd process",
	"What part of shutdown wasn't clear?"
};

struct log_info {
	enum log_level level;
	struct json_result *response;
	unsigned int num_skipped;
};

static void add_skipped(struct log_info *info)
{
	if (info->num_skipped) {
		json_array_start(info->response, NULL);
		json_add_string(info->response, "type", "SKIPPED");
		json_add_num(info->response, "num_skipped", info->num_skipped);
		json_array_end(info->response);
		info->num_skipped = 0;
	}
}

static void json_add_time(struct json_result *result, const char *fieldname,
			  struct timespec ts)
{
	char timebuf[100];

	sprintf(timebuf, "%lu.%09u",
		(unsigned long)ts.tv_sec,
		(unsigned)ts.tv_nsec);
	json_add_string(result, fieldname, timebuf);
}

static void log_to_json(unsigned int skipped,
			struct timerel diff,
			enum log_level level,
			const char *prefix,
			const char *log,
			struct log_info *info)
{
	info->num_skipped += skipped;

	if (level < info->level) {
		info->num_skipped++;
		return;
	}

	add_skipped(info);

	json_array_start(info->response, NULL);
	json_add_string(info->response, "type",
			level == LOG_BROKEN ? "BROKEN"
			: level == LOG_UNUSUAL ? "UNUSUAL"
			: level == LOG_INFORM ? "INFO"
			: level == LOG_DBG ? "DEBUG"
			: level == LOG_IO ? "IO"
			: "UNKNOWN");
	json_add_time(info->response, "time", diff.ts);
	json_add_string(info->response, "source", prefix);
	if (level == LOG_IO) {
		if (log[0])
			json_add_string(info->response, "direction", "IN");
		else
			json_add_string(info->response, "direction", "OUT");

		json_add_hex(info->response, "data", log+1, tal_count(log)-1);
	} else
		json_add_string(info->response, "log", log);

	json_array_end(info->response);
}

static void json_getlog(struct command *cmd,
			const char *buffer, const jsmntok_t *params)
{
	struct log_info info;
	struct log_record *lr = cmd->dstate->log_record;
	jsmntok_t *level;

	json_get_params(buffer, params, "?level", &level, NULL);

	info.num_skipped = 0;

	if (!level)
		info.level = LOG_INFORM;
	else if (json_tok_streq(buffer, level, "io"))
		info.level = LOG_IO;
	else if (json_tok_streq(buffer, level, "debug"))
		info.level = LOG_DBG;
	else if (json_tok_streq(buffer, level, "info"))
		info.level = LOG_INFORM;
	else if (json_tok_streq(buffer, level, "unusual"))
		info.level = LOG_UNUSUAL;
	else {
		command_fail(cmd, "Invalid level param");
		return;
	}

	info.response = new_json_result(cmd);
	json_object_start(info.response, NULL);
	json_add_time(info.response, "creation_time", log_init_time(lr)->ts);
	json_add_num(info.response, "bytes_used", (unsigned int)log_used(lr));
	json_add_num(info.response, "bytes_max", (unsigned int)log_max_mem(lr));
	json_object_start(info.response, "log");
	log_each_line(lr, log_to_json, &info);
	json_object_end(info.response);
	json_object_end(info.response);
	command_success(cmd, info.response);
}

static const struct json_command getlog_command = {
	"getlog",
	json_getlog,
	"Get logs, with optional level: [io|debug|info|unusual]",
	"Returns log array"
};

static void json_rhash(struct command *cmd,
		       const char *buffer, const jsmntok_t *params)
{
	struct json_result *response = new_json_result(cmd);
	jsmntok_t *secrettok;
	struct sha256 secret;

	if (!json_get_params(buffer, params,
			     "secret", &secrettok,
			     NULL)) {
		command_fail(cmd, "Need secret");
		return;
	}

	if (!hex_decode(buffer + secrettok->start,
			secrettok->end - secrettok->start,
			&secret, sizeof(secret))) {
		command_fail(cmd, "'%.*s' is not a valid 32-byte hex value",
			     (int)(secrettok->end - secrettok->start),
			     buffer + secrettok->start);
		return;
	}

	/* Hash in place. */
	sha256(&secret, &secret, sizeof(secret));
	json_object_start(response, NULL);
	json_add_hex(response, "rhash", &secret, sizeof(secret));
	json_object_end(response);
	command_success(cmd, response);
}

static const struct json_command rhash_command = {
	"dev-rhash",
	json_rhash,
	"SHA256 of {secret}",
	"Returns a hash value"
};

static void json_crash(struct command *cmd,
		       const char *buffer, const jsmntok_t *params)
{
	fatal("Crash at user request");
}

static const struct json_command crash_command = {
	"dev-crash",
	json_crash,
	"Call fatal().",
	"Simple crash test for developers"
};

static const struct json_command *cmdlist[] = {
	&help_command,
	&stop_command,
	&getlog_command,
	&connect_command,
	&getpeers_command,
	&newhtlc_command,
	&fulfillhtlc_command,
	&failhtlc_command,
	&commit_command,
	&close_command,
	&newaddr_command,
	/* Developer/debugging options. */
	&echo_command,
	&rhash_command,
	&mocktime_command,
	&crash_command,
	&disconnect_command,
};

static void json_help(struct command *cmd,
		      const char *buffer, const jsmntok_t *params)
{
	unsigned int i;
	struct json_result *response = new_json_result(cmd);

	json_array_start(response, NULL);
	for (i = 0; i < ARRAY_SIZE(cmdlist); i++) {
		json_add_object(response,
				"command", JSMN_STRING,
				cmdlist[i]->name,
				"description", JSMN_STRING,
				cmdlist[i]->description,
				NULL);
	}
	json_array_end(response);
	command_success(cmd, response);
}

static const struct json_command *find_cmd(const char *buffer,
					   const jsmntok_t *tok)
{
	unsigned int i;

	/* cmdlist[i]->name can be NULL in test code. */
	for (i = 0; i < ARRAY_SIZE(cmdlist); i++)
		if (cmdlist[i]->name
		    && json_tok_streq(buffer, tok, cmdlist[i]->name))
			return cmdlist[i];
	return NULL;
}

static void json_result(struct json_connection *jcon,
			const char *id, const char *res, const char *err)
{
	struct json_output *out = tal(jcon, struct json_output);

	out->json = tal_fmt(out,
			    "{ \"result\" : %s,"
			    " \"error\" : %s,"
			    " \"id\" : %s }\n",
			    res, err, id);

	/* Queue for writing, and wake writer (and maybe reader). */
	list_add_tail(&jcon->output, &out->list);
	io_wake(jcon);
}

void command_success(struct command *cmd, struct json_result *result)
{
	struct json_connection *jcon = cmd->jcon;

	if (!jcon) {
		log_unusual(cmd->dstate->base_log,
			    "Command returned result after jcon close");
		tal_free(cmd);
		return;
	}
	assert(jcon->current == cmd);
	json_result(jcon, cmd->id, json_result_string(result), "null");
	jcon->current = tal_free(cmd);
}

void command_fail(struct command *cmd, const char *fmt, ...)
{
	char *quote, *error;
	struct json_connection *jcon = cmd->jcon;
	va_list ap;

	if (!jcon) {
		log_unusual(cmd->dstate->base_log,
			    "Command failed after jcon close");
		tal_free(cmd);
		return;
	}

	va_start(ap, fmt);
	error = tal_vfmt(cmd, fmt, ap);
	va_end(ap);

	/* Remove " */
	while ((quote = strchr(error, '"')) != NULL)
		*quote = '\'';

	/* Now surround in quotes. */
	quote = tal_fmt(cmd, "\"%s\"", error);

	assert(jcon->current == cmd);
	json_result(jcon, cmd->id, "null", quote);
	jcon->current = tal_free(cmd);
}

static void json_command_malformed(struct json_connection *jcon,
				   const char *id,
				   const char *error)
{
	return json_result(jcon, id, "null", error);
}

static void parse_request(struct json_connection *jcon, const jsmntok_t tok[])
{
	const jsmntok_t *method, *id, *params;
	const struct json_command *cmd;

	assert(!jcon->current);
	if (tok[0].type != JSMN_OBJECT) {
		json_command_malformed(jcon, "null",
				       "Expected {} for json command");
		return;
	}

	method = json_get_member(jcon->buffer, tok, "method");
	params = json_get_member(jcon->buffer, tok, "params");
	id = json_get_member(jcon->buffer, tok, "id");

	if (!id) {
		json_command_malformed(jcon, "null", "No id");
		return;
	}
	if (id->type != JSMN_STRING && id->type != JSMN_PRIMITIVE) {
		json_command_malformed(jcon, "null",
				       "Expected string/primitive for id");
		return;
	}

	/* This is a convenient tal parent for durarion of command
	 * (which may outlive the conn!). */
	jcon->current = tal(jcon->dstate, struct command);
	jcon->current->jcon = jcon;
	jcon->current->dstate = jcon->dstate;
	jcon->current->id = tal_strndup(jcon->current,
					json_tok_contents(jcon->buffer, id),
					json_tok_len(id));

	if (!method || !params) {
		command_fail(jcon->current, method ? "No params" : "No method");
		return;
	}

	if (method->type != JSMN_STRING) {
		command_fail(jcon->current, "Expected string for method");
		return;
	}

	cmd = find_cmd(jcon->buffer, method);
	if (!cmd) {
		command_fail(jcon->current,
			     "Unknown command '%.*s'",
			     (int)(method->end - method->start),
			     jcon->buffer + method->start);
		return;
	}

	if (params->type != JSMN_ARRAY && params->type != JSMN_OBJECT) {
		command_fail(jcon->current,
			     "Expected array or object for params");
		return;
	}

	cmd->dispatch(jcon->current, jcon->buffer, params);
}

static struct io_plan *write_json(struct io_conn *conn,
				  struct json_connection *jcon)
{
	struct json_output *out;
	
	out = list_pop(&jcon->output, struct json_output, list);
	if (!out) {
		if (jcon->stop) {
			log_unusual(jcon->log, "JSON-RPC shutdown");
			/* Return us to toplevel lightningd.c */
			io_break(jcon->dstate);
			return io_close(conn);
		}

		/* Reader can go again now. */
		io_wake(jcon);
		return io_out_wait(conn, jcon, write_json, jcon);
	}

	jcon->outbuf = tal_steal(jcon, out->json);
	tal_free(out);

	log_io(jcon->log, false, jcon->outbuf, strlen(jcon->outbuf));
	return io_write(conn,
			jcon->outbuf, strlen(jcon->outbuf), write_json, jcon);
}

static struct io_plan *read_json(struct io_conn *conn,
				 struct json_connection *jcon)
{
	jsmntok_t *toks;
	bool valid;

	log_io(jcon->log, true, jcon->buffer + jcon->used, jcon->len_read);

	/* Resize larger if we're full. */
	jcon->used += jcon->len_read;
	if (jcon->used == tal_count(jcon->buffer))
		tal_resize(&jcon->buffer, jcon->used * 2);

again:
	toks = json_parse_input(jcon->buffer, jcon->used, &valid);
	if (!toks) {
		if (!valid) {
			log_unusual(jcon->dstate->base_log,
				    "Invalid token in json input: '%.*s'",
				    (int)jcon->used, jcon->buffer);
			return io_close(conn);
		}
		/* We need more. */
		goto read_more;
	}

	/* Empty buffer? (eg. just whitespace). */
	if (tal_count(toks) == 1) {
		jcon->used = 0;
		goto read_more;
	}

	parse_request(jcon, toks);

	/* Remove first {}. */
	memmove(jcon->buffer, jcon->buffer + toks[0].end,
		tal_count(jcon->buffer) - toks[0].end);
	jcon->used -= toks[0].end;
	tal_free(toks);

	/* Need to wait for command to finish? */
	if (jcon->current) {
		jcon->len_read = 0;
		return io_wait(conn, jcon, read_json, jcon);
	}

	/* See if we can parse the rest. */
	goto again;

read_more:
	tal_free(toks);
	return io_read_partial(conn, jcon->buffer + jcon->used,
			       tal_count(jcon->buffer) - jcon->used,
			       &jcon->len_read, read_json, jcon);
}

static struct io_plan *jcon_connected(struct io_conn *conn,
				      struct lightningd_state *dstate)
{
	struct json_connection *jcon;

	jcon = tal(dstate, struct json_connection);
	jcon->dstate = dstate;
	jcon->used = 0;
	jcon->buffer = tal_arr(jcon, char, 64);
	jcon->stop = false;
	jcon->current = NULL;
	jcon->log = new_log(jcon, dstate->log_record, "%sjcon fd %i:",
			    log_prefix(dstate->base_log), io_conn_fd(conn));
	list_head_init(&jcon->output);

	io_set_finish(conn, finish_jcon, jcon);

	return io_duplex(conn,
			 io_read_partial(conn, jcon->buffer,
					 tal_count(jcon->buffer),
					 &jcon->len_read, read_json, jcon),
			 write_json(conn, jcon));
}

static struct io_plan *incoming_jcon_connected(struct io_conn *conn,
					       struct lightningd_state *dstate)
{
	log_info(dstate->base_log, "Connected json input");
	return jcon_connected(conn, dstate);
}

void setup_jsonrpc(struct lightningd_state *dstate, const char *rpc_filename)
{
	struct sockaddr_un addr;
	int fd, old_umask;

	if (streq(rpc_filename, ""))
		return;

	if (streq(rpc_filename, "/dev/tty")) {
		fd = open(rpc_filename, O_RDWR);
		if (fd == -1)
			err(1, "Opening %s", rpc_filename);
		io_new_conn(dstate, fd, jcon_connected, dstate);
		return;
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (strlen(rpc_filename) + 1 > sizeof(addr.sun_path))
		errx(1, "rpc filename '%s' too long", rpc_filename);
	strcpy(addr.sun_path, rpc_filename);
	addr.sun_family = AF_UNIX;

	/* Of course, this is racy! */
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
		errx(1, "rpc filename '%s' in use", rpc_filename);
	unlink(rpc_filename);

	/* This file is only rw by us! */
	old_umask = umask(0177);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)))
		err(1, "Binding rpc socket to '%s'", rpc_filename);
	umask(old_umask);

	if (listen(fd, 1) != 0)
		err(1, "Listening on '%s'", rpc_filename);

	io_new_listener(dstate, fd, incoming_jcon_connected, dstate);
}
