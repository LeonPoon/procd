/*
 * Copyright (C) 2013 Felix Fietkau <nbd@openwrt.org>
 * Copyright (C) 2013 John Crispin <blogic@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <sys/utsname.h>
#ifdef linux
#include <sys/sysinfo.h>
#endif
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>

#include <json-c/json_tokener.h>
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>

#include "procd.h"
#include "sysupgrade.h"
#include "watchdog.h"

static struct blob_buf b;
static int notify;
static struct ubus_context *_ctx;

static int system_board(struct ubus_context *ctx, struct ubus_object *obj,
                 struct ubus_request_data *req, const char *method,
                 struct blob_attr *msg)
{
	void *c;
	char line[256];
	char *key, *val, *next;
	struct utsname utsname;
	FILE *f;

	blob_buf_init(&b, 0);

	if (uname(&utsname) >= 0)
	{
		blobmsg_add_string(&b, "kernel", utsname.release);
		blobmsg_add_string(&b, "hostname", utsname.nodename);
	}

	if ((f = fopen("/proc/cpuinfo", "r")) != NULL)
	{
		while(fgets(line, sizeof(line), f))
		{
			key = strtok(line, "\t:");
			val = strtok(NULL, "\t\n");

			if (!key || !val)
				continue;

			if (!strcasecmp(key, "system type") ||
			    !strcasecmp(key, "processor") ||
			    !strcasecmp(key, "cpu") ||
			    !strcasecmp(key, "model name"))
			{
				strtoul(val + 2, &key, 0);

				if (key == (val + 2) || *key != 0)
				{
					blobmsg_add_string(&b, "system", val + 2);
					break;
				}
			}
		}

		fclose(f);
	}

	if ((f = fopen("/tmp/sysinfo/model", "r")) != NULL ||
	    (f = fopen("/proc/device-tree/model", "r")) != NULL)
	{
		if (fgets(line, sizeof(line), f))
		{
			val = strtok(line, "\t\n");

			if (val)
				blobmsg_add_string(&b, "model", val);
		}

		fclose(f);
	}
	else if ((f = fopen("/proc/cpuinfo", "r")) != NULL)
	{
		while(fgets(line, sizeof(line), f))
		{
			key = strtok(line, "\t:");
			val = strtok(NULL, "\t\n");

			if (!key || !val)
				continue;

			if (!strcasecmp(key, "machine") ||
			    !strcasecmp(key, "hardware"))
			{
				blobmsg_add_string(&b, "model", val + 2);
				break;
			}
		}

		fclose(f);
	}

	if ((f = fopen("/tmp/sysinfo/board_name", "r")) != NULL)
	{
		if (fgets(line, sizeof(line), f))
		{
			val = strtok(line, "\t\n");

			if (val)
				blobmsg_add_string(&b, "board_name", val);
		}

		fclose(f);
	}
	else if ((f = fopen("/proc/device-tree/compatible", "r")) != NULL)
	{
		if (fgets(line, sizeof(line), f))
		{
			val = strtok(line, "\t\n");

			if (val)
			{
				next = val;
				while ((next = strchr(next, ',')) != NULL)
				{
					*next = '-';
					next++;
				}

				blobmsg_add_string(&b, "board_name", val);
			}
		}

		fclose(f);
	}

	if ((f = fopen("/etc/openwrt_release", "r")) != NULL)
	{
		c = blobmsg_open_table(&b, "release");

		while (fgets(line, sizeof(line), f))
		{
			char *dest;
			char ch;

			key = line;
			val = strchr(line, '=');
			if (!val)
				continue;

			*(val++) = 0;

			if (!strcasecmp(key, "DISTRIB_ID"))
				key = "distribution";
			else if (!strcasecmp(key, "DISTRIB_RELEASE"))
				key = "version";
			else if (!strcasecmp(key, "DISTRIB_REVISION"))
				key = "revision";
			else if (!strcasecmp(key, "DISTRIB_CODENAME"))
				key = "codename";
			else if (!strcasecmp(key, "DISTRIB_TARGET"))
				key = "target";
			else if (!strcasecmp(key, "DISTRIB_DESCRIPTION"))
				key = "description";
			else
				continue;

			dest = blobmsg_alloc_string_buffer(&b, key, strlen(val));
			if (!dest) {
				ERROR("Failed to allocate blob.\n");
				continue;
			}

			while (val && (ch = *(val++)) != 0) {
				switch (ch) {
				case '\'':
				case '"':
					next = strchr(val, ch);
					if (next)
						*next = 0;

					strcpy(dest, val);

					if (next)
						val = next + 1;

					dest += strlen(dest);
					break;
				case '\\':
					*(dest++) = *(val++);
					break;
				}
			}
			blobmsg_add_string_buffer(&b);
		}

		blobmsg_close_array(&b, c);

		fclose(f);
	}

	ubus_send_reply(ctx, req, b.head);

	return UBUS_STATUS_OK;
}

static int system_info(struct ubus_context *ctx, struct ubus_object *obj,
                struct ubus_request_data *req, const char *method,
                struct blob_attr *msg)
{
	time_t now;
	struct tm *tm;
#ifdef linux
	struct sysinfo info;
	void *c;
	char line[256];
	char *key, *val;
	unsigned long long available, cached;
	FILE *f;

	if (sysinfo(&info))
		return UBUS_STATUS_UNKNOWN_ERROR;

	if ((f = fopen("/proc/meminfo", "r")) == NULL)
		return UBUS_STATUS_UNKNOWN_ERROR;

	/* if linux < 3.14 MemAvailable is not in meminfo */
	available = 0;
	cached = 0;

	while (fgets(line, sizeof(line), f))
	{
		key = strtok(line, " :");
		val = strtok(NULL, " ");

		if (!key || !val)
			continue;

		if (!strcasecmp(key, "MemAvailable"))
			available = 1024 * atoll(val);
		else if (!strcasecmp(key, "Cached"))
			cached = 1024 * atoll(val);
	}

	fclose(f);
#endif

	now = time(NULL);

	if (!(tm = localtime(&now)))
		return UBUS_STATUS_UNKNOWN_ERROR;

	blob_buf_init(&b, 0);

	blobmsg_add_u32(&b, "localtime", now + tm->tm_gmtoff);

#ifdef linux
	blobmsg_add_u32(&b, "uptime",    info.uptime);

	c = blobmsg_open_array(&b, "load");
	blobmsg_add_u32(&b, NULL, info.loads[0]);
	blobmsg_add_u32(&b, NULL, info.loads[1]);
	blobmsg_add_u32(&b, NULL, info.loads[2]);
	blobmsg_close_array(&b, c);

	c = blobmsg_open_table(&b, "memory");
	blobmsg_add_u64(&b, "total",
			(uint64_t)info.mem_unit * (uint64_t)info.totalram);
	blobmsg_add_u64(&b, "free",
			(uint64_t)info.mem_unit * (uint64_t)info.freeram);
	blobmsg_add_u64(&b, "shared",
			(uint64_t)info.mem_unit * (uint64_t)info.sharedram);
	blobmsg_add_u64(&b, "buffered",
			(uint64_t)info.mem_unit * (uint64_t)info.bufferram);
	blobmsg_add_u64(&b, "available", available);
	blobmsg_add_u64(&b, "cached", cached);
	blobmsg_close_table(&b, c);

	c = blobmsg_open_table(&b, "swap");
	blobmsg_add_u64(&b, "total",
			(uint64_t)info.mem_unit * (uint64_t)info.totalswap);
	blobmsg_add_u64(&b, "free",
			(uint64_t)info.mem_unit * (uint64_t)info.freeswap);
	blobmsg_close_table(&b, c);
#endif

	ubus_send_reply(ctx, req, b.head);

	return UBUS_STATUS_OK;
}

static int system_reboot(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	procd_shutdown(RB_AUTOBOOT);
	return 0;
}

enum {
	WDT_FREQUENCY,
	WDT_TIMEOUT,
	WDT_MAGICCLOSE,
	WDT_STOP,
	__WDT_MAX
};

static const struct blobmsg_policy watchdog_policy[__WDT_MAX] = {
	[WDT_FREQUENCY] = { .name = "frequency", .type = BLOBMSG_TYPE_INT32 },
	[WDT_TIMEOUT] = { .name = "timeout", .type = BLOBMSG_TYPE_INT32 },
	[WDT_MAGICCLOSE] = { .name = "magicclose", .type = BLOBMSG_TYPE_BOOL },
	[WDT_STOP] = { .name = "stop", .type = BLOBMSG_TYPE_BOOL },
};

static int watchdog_set(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__WDT_MAX];
	const char *status;

	if (!msg)
		return UBUS_STATUS_INVALID_ARGUMENT;

	blobmsg_parse(watchdog_policy, __WDT_MAX, tb, blob_data(msg), blob_len(msg));
	if (tb[WDT_FREQUENCY]) {
		unsigned int timeout = tb[WDT_TIMEOUT] ? blobmsg_get_u32(tb[WDT_TIMEOUT]) :
						watchdog_timeout(0);
		unsigned int freq = blobmsg_get_u32(tb[WDT_FREQUENCY]);

		if (freq) {
			if (freq > timeout / 2)
				freq = timeout / 2;
			watchdog_frequency(freq);
		}
	}

	if (tb[WDT_TIMEOUT]) {
		unsigned int timeout = blobmsg_get_u32(tb[WDT_TIMEOUT]);
		unsigned int frequency = watchdog_frequency(0);

		if (timeout <= frequency)
			timeout = frequency * 2;
		 watchdog_timeout(timeout);
	}

	if (tb[WDT_MAGICCLOSE])
		watchdog_set_magicclose(blobmsg_get_bool(tb[WDT_MAGICCLOSE]));

	if (tb[WDT_STOP])
		watchdog_set_stopped(blobmsg_get_bool(tb[WDT_STOP]));

	if (watchdog_fd() == NULL)
		status = "offline";
	else if (watchdog_get_stopped())
		status = "stopped";
	else
		status = "running";

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "status", status);
	blobmsg_add_u32(&b, "timeout", watchdog_timeout(0));
	blobmsg_add_u32(&b, "frequency", watchdog_frequency(0));
	blobmsg_add_u8(&b, "magicclose", watchdog_get_magicclose());
	ubus_send_reply(ctx, req, b.head);

	return 0;
}

enum {
	SIGNAL_PID,
	SIGNAL_NUM,
	__SIGNAL_MAX
};

static const struct blobmsg_policy signal_policy[__SIGNAL_MAX] = {
	[SIGNAL_PID] = { .name = "pid", .type = BLOBMSG_TYPE_INT32 },
	[SIGNAL_NUM] = { .name = "signum", .type = BLOBMSG_TYPE_INT32 },
};

static int proc_signal(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct blob_attr *tb[__SIGNAL_MAX];

	if (!msg)
		return UBUS_STATUS_INVALID_ARGUMENT;

	blobmsg_parse(signal_policy, __SIGNAL_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[SIGNAL_PID || !tb[SIGNAL_NUM]])
		return UBUS_STATUS_INVALID_ARGUMENT;

	kill(blobmsg_get_u32(tb[SIGNAL_PID]), blobmsg_get_u32(tb[SIGNAL_NUM]));

	return 0;
}

/**
 * validate_firmware_image_call - perform validation & store result in global b
 *
 * @file: firmware image path
 */
static int validate_firmware_image_call(const char *file)
{
	const char *path = "/usr/libexec/validate_firmware_image";
	json_object *jsobj = NULL;
	json_tokener *tok;
	char buf[64];
	ssize_t len;
	int fds[2];
	int err;
	int fd;

	if (pipe(fds))
		return -errno;

	switch (fork()) {
	case -1:
		return -errno;
	case 0:
		/* Set stdin & stderr to /dev/null */
		fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, 0);
			dup2(fd, 2);
			close(fd);
		}

		/* Set stdout to the shared pipe */
		dup2(fds[1], 1);
		close(fds[0]);
		close(fds[1]);

		execl(path, path, file, NULL);
		exit(errno);
	}

	/* Parent process */

	tok = json_tokener_new();
	if (!tok) {
		close(fds[0]);
		close(fds[1]);
		return -ENOMEM;
	}

	blob_buf_init(&b, 0);
	while ((len = read(fds[0], buf, sizeof(buf)))) {
		jsobj = json_tokener_parse_ex(tok, buf, len);

		if (json_tokener_get_error(tok) == json_tokener_success)
			break;
		else if (json_tokener_get_error(tok) == json_tokener_continue)
			continue;
		else
			fprintf(stderr, "Failed to parse JSON: %d\n",
				json_tokener_get_error(tok));
	}

	close(fds[0]);
	close(fds[1]);

	err = -ENOENT;
	if (jsobj) {
		if (json_object_get_type(jsobj) == json_type_object) {
			blobmsg_add_object(&b, jsobj);
			err = 0;
		}

		json_object_put(jsobj);
	}

	json_tokener_free(tok);

	return err;
}

enum {
	VALIDATE_FIRMWARE_IMAGE_PATH,
	__VALIDATE_FIRMWARE_IMAGE_MAX,
};

static const struct blobmsg_policy validate_firmware_image_policy[__VALIDATE_FIRMWARE_IMAGE_MAX] = {
	[VALIDATE_FIRMWARE_IMAGE_PATH] = { .name = "path", .type = BLOBMSG_TYPE_STRING },
};

static int validate_firmware_image(struct ubus_context *ctx,
				   struct ubus_object *obj,
				   struct ubus_request_data *req,
				   const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__VALIDATE_FIRMWARE_IMAGE_MAX];

	if (!msg)
		return UBUS_STATUS_INVALID_ARGUMENT;

	blobmsg_parse(validate_firmware_image_policy, __VALIDATE_FIRMWARE_IMAGE_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[VALIDATE_FIRMWARE_IMAGE_PATH])
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (validate_firmware_image_call(blobmsg_get_string(tb[VALIDATE_FIRMWARE_IMAGE_PATH])))
		return UBUS_STATUS_UNKNOWN_ERROR;

	ubus_send_reply(ctx, req, b.head);

	return UBUS_STATUS_OK;
}

enum {
	SYSUPGRADE_PATH,
	SYSUPGRADE_FORCE,
	SYSUPGRADE_BACKUP,
	SYSUPGRADE_PREFIX,
	SYSUPGRADE_COMMAND,
	SYSUPGRADE_OPTIONS,
	__SYSUPGRADE_MAX
};

static const struct blobmsg_policy sysupgrade_policy[__SYSUPGRADE_MAX] = {
	[SYSUPGRADE_PATH] = { .name = "path", .type = BLOBMSG_TYPE_STRING },
	[SYSUPGRADE_FORCE] = { .name = "force", .type = BLOBMSG_TYPE_BOOL },
	[SYSUPGRADE_BACKUP] = { .name = "backup", .type = BLOBMSG_TYPE_STRING },
	[SYSUPGRADE_PREFIX] = { .name = "prefix", .type = BLOBMSG_TYPE_STRING },
	[SYSUPGRADE_COMMAND] = { .name = "command", .type = BLOBMSG_TYPE_STRING },
	[SYSUPGRADE_OPTIONS] = { .name = "options", .type = BLOBMSG_TYPE_TABLE },
};

static void sysupgrade_error(struct ubus_context *ctx,
			     struct ubus_request_data *req,
			     const char *message)
{
	void *c;

	blob_buf_init(&b, 0);

	c = blobmsg_open_table(&b, "error");
	blobmsg_add_string(&b, "message", message);
	blobmsg_close_table(&b, c);

	ubus_send_reply(ctx, req, b.head);
}

static int sysupgrade(struct ubus_context *ctx, struct ubus_object *obj,
		      struct ubus_request_data *req, const char *method,
		      struct blob_attr *msg)
{
	enum {
		VALIDATION_VALID,
		VALIDATION_FORCEABLE,
		VALIDATION_ALLOW_BACKUP,
		__VALIDATION_MAX
	};
	static const struct blobmsg_policy validation_policy[__VALIDATION_MAX] = {
		[VALIDATION_VALID] = { .name = "valid", .type = BLOBMSG_TYPE_BOOL },
		[VALIDATION_FORCEABLE] = { .name = "forceable", .type = BLOBMSG_TYPE_BOOL },
		[VALIDATION_ALLOW_BACKUP] = { .name = "allow_backup", .type = BLOBMSG_TYPE_BOOL },
	};
	struct blob_attr *validation[__VALIDATION_MAX];
	struct blob_attr *tb[__SYSUPGRADE_MAX];
	bool valid, forceable, allow_backup;

	if (!msg)
		return UBUS_STATUS_INVALID_ARGUMENT;

	blobmsg_parse(sysupgrade_policy, __SYSUPGRADE_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[SYSUPGRADE_PATH] || !tb[SYSUPGRADE_PREFIX])
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (validate_firmware_image_call(blobmsg_get_string(tb[SYSUPGRADE_PATH]))) {
		sysupgrade_error(ctx, req, "Firmware image couldn't be validated");
		return UBUS_STATUS_UNKNOWN_ERROR;
	}

	blobmsg_parse(validation_policy, __VALIDATION_MAX, validation, blob_data(b.head), blob_len(b.head));

	valid = validation[VALIDATION_VALID] && blobmsg_get_bool(validation[VALIDATION_VALID]);
	forceable = validation[VALIDATION_FORCEABLE] && blobmsg_get_bool(validation[VALIDATION_FORCEABLE]);
	allow_backup = validation[VALIDATION_ALLOW_BACKUP] && blobmsg_get_bool(validation[VALIDATION_ALLOW_BACKUP]);

	if (!valid) {
		if (!forceable) {
			sysupgrade_error(ctx, req, "Firmware image is broken and cannot be installed");
			return UBUS_STATUS_NOT_SUPPORTED;
		} else if (!tb[SYSUPGRADE_FORCE] || !blobmsg_get_bool(tb[SYSUPGRADE_FORCE])) {
			sysupgrade_error(ctx, req, "Firmware image is invalid");
			return UBUS_STATUS_NOT_SUPPORTED;
		}
	} else if (!allow_backup && tb[SYSUPGRADE_BACKUP]) {
		sysupgrade_error(ctx, req, "Firmware image doesn't allow preserving a backup");
		return UBUS_STATUS_NOT_SUPPORTED;
	}

	sysupgrade_exec_upgraded(blobmsg_get_string(tb[SYSUPGRADE_PREFIX]),
				 blobmsg_get_string(tb[SYSUPGRADE_PATH]),
				 tb[SYSUPGRADE_BACKUP] ? blobmsg_get_string(tb[SYSUPGRADE_BACKUP]) : NULL,
				 tb[SYSUPGRADE_COMMAND] ? blobmsg_get_string(tb[SYSUPGRADE_COMMAND]) : NULL,
				 tb[SYSUPGRADE_OPTIONS]);

	/* sysupgrade_exec_upgraded() will never return unless something has gone wrong */
	return UBUS_STATUS_UNKNOWN_ERROR;
}

static void
procd_subscribe_cb(struct ubus_context *ctx, struct ubus_object *obj)
{
	notify = obj->has_subscribers;
}


static const struct ubus_method system_methods[] = {
	UBUS_METHOD_NOARG("board", system_board),
	UBUS_METHOD_NOARG("info",  system_info),
	UBUS_METHOD_NOARG("reboot", system_reboot),
	UBUS_METHOD("watchdog", watchdog_set, watchdog_policy),
	UBUS_METHOD("signal", proc_signal, signal_policy),
	UBUS_METHOD("validate_firmware_image", validate_firmware_image, validate_firmware_image_policy),
	UBUS_METHOD("sysupgrade", sysupgrade, sysupgrade_policy),
};

static struct ubus_object_type system_object_type =
	UBUS_OBJECT_TYPE("system", system_methods);

static struct ubus_object system_object = {
	.name = "system",
	.type = &system_object_type,
	.methods = system_methods,
	.n_methods = ARRAY_SIZE(system_methods),
	.subscribe_cb = procd_subscribe_cb,
};

void
procd_bcast_event(char *event, struct blob_attr *msg)
{
	int ret;

	if (!notify)
		return;

	ret = ubus_notify(_ctx, &system_object, event, msg, -1);
	if (ret)
		fprintf(stderr, "Failed to notify log: %s\n", ubus_strerror(ret));
}

void ubus_init_system(struct ubus_context *ctx)
{
	int ret;

	_ctx = ctx;
	ret = ubus_add_object(ctx, &system_object);
	if (ret)
		ERROR("Failed to add object: %s\n", ubus_strerror(ret));
}
