/*
 * Copyright (C) 2011-2014 Felix Fietkau <nbd@openwrt.org>
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
#include <arpa/inet.h>
#include <unistd.h>

#include "ubusd.h"

#include <libubox/blobmsg_json.h>

struct blob_buf b;
static struct ubus_msg_buf *retmsg;
static int *retmsg_data;
static struct avl_tree clients;

static struct blob_attr *attrbuf[UBUS_ATTR_MAX];

typedef int (*ubus_cmd_cb)(struct ubus_client *cl, struct ubus_msg_buf *ub, struct blob_attr **attr);

static const struct blob_attr_info ubus_policy[UBUS_ATTR_MAX] = {
	[UBUS_ATTR_SIGNATURE] = { .type = BLOB_ATTR_NESTED },
	[UBUS_ATTR_OBJTYPE] = { .type = BLOB_ATTR_INT32 },
	[UBUS_ATTR_OBJPATH] = { .type = BLOB_ATTR_STRING },
	[UBUS_ATTR_OBJID] = { .type = BLOB_ATTR_INT32 },
	[UBUS_ATTR_STATUS] = { .type = BLOB_ATTR_INT32 },
	[UBUS_ATTR_METHOD] = { .type = BLOB_ATTR_STRING },
	[UBUS_ATTR_USER] = { .type = BLOB_ATTR_STRING },
	[UBUS_ATTR_GROUP] = { .type = BLOB_ATTR_STRING },
};

struct blob_attr **ubus_parse_msg(struct blob_attr *msg)
{
	blob_parse(msg, attrbuf, ubus_policy, UBUS_ATTR_MAX);
	return attrbuf;
}

static void ubus_msg_close_fd(struct ubus_msg_buf *ub)
{
	if (ub->fd < 0)
		return;

	close(ub->fd);
	ub->fd = -1;
}

static void ubus_msg_init(struct ubus_msg_buf *ub, uint8_t type, uint16_t seq, uint32_t peer)
{
	ub->hdr.version = 0;
	ub->hdr.type = type;
	ub->hdr.seq = seq;
	ub->hdr.peer = peer;
}

static struct ubus_msg_buf *ubus_msg_from_blob(bool shared)
{
	return ubus_msg_new(b.head, blob_raw_len(b.head), shared);
}

static struct ubus_msg_buf *ubus_reply_from_blob(struct ubus_msg_buf *ub, bool shared)
{
	struct ubus_msg_buf *new;

	new = ubus_msg_from_blob(shared);
	if (!new)
		return NULL;

	ubus_msg_init(new, UBUS_MSG_DATA, ub->hdr.seq, ub->hdr.peer);
	return new;
}

void
ubus_proto_send_msg_from_blob(struct ubus_client *cl, struct ubus_msg_buf *ub,
			uint8_t type)
{
	ub = ubus_reply_from_blob(ub, true);
	if (!ub)
		return;

	ub->hdr.type = type;
	ubus_msg_send(cl, ub, true);
}

static bool ubusd_send_hello(struct ubus_client *cl)
{
	struct ubus_msg_buf *ub;

	blob_buf_init(&b, 0);
	ub = ubus_msg_from_blob(true);
	if (!ub)
		return false;

	ubus_msg_init(ub, UBUS_MSG_HELLO, 0, cl->id.id);
	ubus_msg_send(cl, ub, true);
	return true;
}

static int ubusd_send_pong(struct ubus_client *cl, struct ubus_msg_buf *ub, struct blob_attr **attr)
{
	ub->hdr.type = UBUS_MSG_DATA;
	ubus_msg_send(cl, ub, false);
	return 0;
}

static int ubusd_handle_remove_object(struct ubus_client *cl, struct ubus_msg_buf *ub, struct blob_attr **attr)
{
	struct ubus_object *obj;

	if (!attr[UBUS_ATTR_OBJID])
		return UBUS_STATUS_INVALID_ARGUMENT;

	obj = ubusd_find_object(blob_get_u32(attr[UBUS_ATTR_OBJID]));
	if (!obj)
		return UBUS_STATUS_NOT_FOUND;

	if (obj->client != cl)
		return UBUS_STATUS_PERMISSION_DENIED;

	blob_buf_init(&b, 0);
	blob_put_int32(&b, UBUS_ATTR_OBJID, obj->id.id);

	/* check if we're removing the object type as well */
	if (obj->type && obj->type->refcount == 1)
		blob_put_int32(&b, UBUS_ATTR_OBJTYPE, obj->type->id.id);

	ubusd_free_object(obj);
	ubus_proto_send_msg_from_blob(cl, ub, UBUS_MSG_DATA);

	return 0;
}

static const char *format_type(void *priv, struct blob_attr *attr)
{
	static const char * const attr_types[] = {
		[BLOBMSG_TYPE_INT8] = "\"Boolean\"",
		[BLOBMSG_TYPE_INT32] = "\"Integer\"",
		[BLOBMSG_TYPE_STRING] = "\"String\"",
		[BLOBMSG_TYPE_ARRAY] = "\"Array\"",
		[BLOBMSG_TYPE_TABLE] = "\"Table\"",
	};
	const char *type = NULL;
	int typeid;

	if (blob_id(attr) != BLOBMSG_TYPE_INT32)
		return NULL;

	typeid = blobmsg_get_u32(attr);
	if (typeid < ARRAY_SIZE(attr_types))
		type = attr_types[typeid];
	if (!type)
		type = "\"(unknown)\"";

	return type;
}

static int ubusd_handle_add_object(struct ubus_client *cl,
	struct ubus_msg_buf *ub, struct blob_attr **attr)
{
	struct ubus_object *obj;

	obj = ubusd_create_object(cl, attr);
	if (!obj)
		return UBUS_STATUS_INVALID_ARGUMENT;

	if(attr[UBUS_ATTR_SIGNATURE]){
			struct blob_attr *cur;
			char *s;
			int rem;

			blob_for_each_attr(cur, attr[UBUS_ATTR_SIGNATURE], rem) {
				s = blobmsg_format_json_with_cb(cur, false, format_type, NULL, -1);
				printf("\t%s\n", s);
				free(s);
			}
	}

	printf("-> [add] ubusd_handle_add_object: %s ->\n",
				(char*)obj->path.key);

	blob_buf_init(&b, 0);
	blob_put_int32(&b, UBUS_ATTR_OBJID, obj->id.id);
	if (attr[UBUS_ATTR_SIGNATURE])
		blob_put_int32(&b, UBUS_ATTR_OBJTYPE, obj->type->id.id);

	ubus_proto_send_msg_from_blob(cl, ub, UBUS_MSG_DATA);
	return 0;
}

static void ubusd_send_obj(struct ubus_client *cl, struct ubus_msg_buf *ub, struct ubus_object *obj)
{
	struct ubus_method *m;
	int cnt = 0;
	void *s;

	blob_buf_init(&b, 0);

	blob_put_string(&b, UBUS_ATTR_OBJPATH, obj->path.key);
	blob_put_int32(&b, UBUS_ATTR_OBJID, obj->id.id);
	blob_put_int32(&b, UBUS_ATTR_OBJTYPE, obj->type->id.id);

	s = blob_nest_start(&b, UBUS_ATTR_SIGNATURE);
	list_for_each_entry(m, &obj->type->methods, list) {
		if (!ubusd_acl_check(cl, obj->path.key, blobmsg_name(m->data), UBUS_ACL_ACCESS)) {
			blobmsg_add_blob(&b, m->data);
			cnt++;
		}
	}
	blob_nest_end(&b, s);

	if (cnt)
		ubus_proto_send_msg_from_blob(cl, ub, UBUS_MSG_DATA);
}

static int ubusd_handle_lookup(struct ubus_client *cl, struct ubus_msg_buf *ub, struct blob_attr **attr)
{
	struct ubus_object *obj;
	char *objpath;
	bool found = false;
	int len;

	if (!attr[UBUS_ATTR_OBJPATH]) {
		avl_for_each_element(&path, obj, path)
			ubusd_send_obj(cl, ub, obj);
		return 0;
	}

	objpath = blob_data(attr[UBUS_ATTR_OBJPATH]);
	len = strlen(objpath);
	if (objpath[len - 1] != '*') {
		obj = avl_find_element(&path, objpath, obj, path);
		if (!obj)
			return UBUS_STATUS_NOT_FOUND;

		ubusd_send_obj(cl, ub, obj);
		return 0;
	}

	objpath[--len] = 0;

	obj = avl_find_ge_element(&path, objpath, obj, path);
	if (!obj)
		return UBUS_STATUS_NOT_FOUND;

	while (!strncmp(objpath, obj->path.key, len)) {
		found = true;
		ubusd_send_obj(cl, ub, obj);
		if (obj == avl_last_element(&path, obj, path))
			break;
		obj = avl_next_element(obj, path);
	}

	if (!found)
		return UBUS_STATUS_NOT_FOUND;

	return 0;
}

static void
ubusd_forward_invoke(struct ubus_client *cl, struct ubus_object *obj,
		     const char *method, struct ubus_msg_buf *ub,
		     struct blob_attr *data)
{
	// printf("-> [consumer] ubusd_forward_invoke: %s,fd: %d, pid:%d, gid:%d, uid:%d, %s\n",
	//  							method, cl->sock.fd, cl->pid, cl->gid, cl->uid, data->data);

	blob_put_int32(&b, UBUS_ATTR_OBJID, obj->id.id);
	blob_put_string(&b, UBUS_ATTR_METHOD, method);
	if (cl->user)
		blob_put_string(&b, UBUS_ATTR_USER, cl->user);
	if (cl->group)
		blob_put_string(&b, UBUS_ATTR_GROUP, cl->group);
	if (data)
		blob_put(&b, UBUS_ATTR_DATA, blob_data(data), blob_len(data));

	ubus_proto_send_msg_from_blob(obj->client, ub, UBUS_MSG_INVOKE);
}

static int ubusd_handle_invoke(struct ubus_client *cl,
				struct ubus_msg_buf *ub, struct blob_attr **attr)
{
	struct ubus_object *obj = NULL;
	struct ubus_id *id;
	const char *method;

	if (!attr[UBUS_ATTR_METHOD] || !attr[UBUS_ATTR_OBJID])
		return UBUS_STATUS_INVALID_ARGUMENT;

	id = ubus_find_id(&objects, blob_get_u32(attr[UBUS_ATTR_OBJID]));
	if (!id)
		return UBUS_STATUS_NOT_FOUND;

	obj = container_of(id, struct ubus_object, id);

	printf("-> [invoke] ubusd_handle_invoke: %s->%s, pid:%d\n",
		(char*)obj->path.key,
		attr[UBUS_ATTR_METHOD]->data,
		cl->pid);

	method = blob_data(attr[UBUS_ATTR_METHOD]);

	if (ubusd_acl_check(cl, obj->path.key, method, UBUS_ACL_ACCESS))
		return UBUS_STATUS_NOT_FOUND;

	if (!obj->client)
		return obj->recv_msg(cl, ub, method, attr[UBUS_ATTR_DATA]);

	ub->hdr.peer = cl->id.id;
	blob_buf_init(&b, 0);

	ubusd_forward_invoke(cl, obj, method, ub, attr[UBUS_ATTR_DATA]);
	ubus_msg_free(ub);

	return -1;
}

static int ubusd_handle_notify(struct ubus_client *cl, struct ubus_msg_buf *ub, struct blob_attr **attr)
{
	struct ubus_object *obj = NULL;
	struct ubus_subscription *s;
	struct ubus_id *id;
	const char *method;
	bool no_reply = false;
	void *c;

	if (!attr[UBUS_ATTR_METHOD] || !attr[UBUS_ATTR_OBJID])
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (attr[UBUS_ATTR_NO_REPLY])
		no_reply = blob_get_int8(attr[UBUS_ATTR_NO_REPLY]);

	id = ubus_find_id(&objects, blob_get_u32(attr[UBUS_ATTR_OBJID]));
	if (!id)
		return UBUS_STATUS_NOT_FOUND;

	obj = container_of(id, struct ubus_object, id);
	if (obj->client != cl)
		return UBUS_STATUS_PERMISSION_DENIED;

	if (!no_reply) {
		blob_buf_init(&b, 0);
		blob_put_int32(&b, UBUS_ATTR_OBJID, id->id);
		c = blob_nest_start(&b, UBUS_ATTR_SUBSCRIBERS);
		list_for_each_entry(s, &obj->subscribers, list) {
			blob_put_int32(&b, 0, s->subscriber->id.id);
		}
		blob_nest_end(&b, c);
		blob_put_int32(&b, UBUS_ATTR_STATUS, 0);
		ubus_proto_send_msg_from_blob(cl, ub, UBUS_MSG_STATUS);
	}

	ub->hdr.peer = cl->id.id;
	method = blob_data(attr[UBUS_ATTR_METHOD]);
	list_for_each_entry(s, &obj->subscribers, list) {
		blob_buf_init(&b, 0);
		if (no_reply)
			blob_put_int8(&b, UBUS_ATTR_NO_REPLY, 1);
		ubusd_forward_invoke(cl, s->subscriber, method, ub, attr[UBUS_ATTR_DATA]);
	}
	ubus_msg_free(ub);

	return -1;
}

static struct ubus_client *ubusd_get_client_by_id(uint32_t id)
{
	struct ubus_id *clid;

	clid = ubus_find_id(&clients, id);
	if (!clid)
		return NULL;

	return container_of(clid, struct ubus_client, id);
}

static int ubusd_handle_response(struct ubus_client *cl, struct ubus_msg_buf *ub, struct blob_attr **attr)
{
	struct ubus_object *obj;

	if (!attr[UBUS_ATTR_OBJID] ||
	    (ub->hdr.type == UBUS_MSG_STATUS && !attr[UBUS_ATTR_STATUS]) ||
	    (ub->hdr.type == UBUS_MSG_DATA && !attr[UBUS_ATTR_DATA]))
		goto error;

	obj = ubusd_find_object(blob_get_u32(attr[UBUS_ATTR_OBJID]));
	if (!obj)
		goto error;

	if (cl != obj->client)
		goto error;

	cl = ubusd_get_client_by_id(ub->hdr.peer);
	if (!cl)
		goto error;

	ub->hdr.peer = blob_get_u32(attr[UBUS_ATTR_OBJID]);
	ubus_msg_send(cl, ub, true);
	return -1;

error:
	ubus_msg_free(ub);
	return -1;
}

static int ubusd_handle_add_watch(struct ubus_client *cl, struct ubus_msg_buf *ub, struct blob_attr **attr)
{
	struct ubus_object *obj, *target;

	if (!attr[UBUS_ATTR_OBJID] || !attr[UBUS_ATTR_TARGET])
		return UBUS_STATUS_INVALID_ARGUMENT;

	obj = ubusd_find_object(blob_get_u32(attr[UBUS_ATTR_OBJID]));
	if (!obj)
		return UBUS_STATUS_NOT_FOUND;

	if (cl != obj->client)
		return UBUS_STATUS_INVALID_ARGUMENT;

	target = ubusd_find_object(blob_get_u32(attr[UBUS_ATTR_TARGET]));
	if (!target)
		return UBUS_STATUS_NOT_FOUND;

	if (cl == target->client)
		return UBUS_STATUS_INVALID_ARGUMENT;

	if (!target->path.key) {
		if (strcmp(target->client->user, cl->user) && strcmp(target->client->group, cl->group))
			return UBUS_STATUS_NOT_FOUND;
	} else if (ubusd_acl_check(cl, target->path.key, NULL, UBUS_ACL_SUBSCRIBE)) {
		return UBUS_STATUS_NOT_FOUND;
	}

	ubus_subscribe(obj, target);
	return 0;
}

static int ubusd_handle_remove_watch(struct ubus_client *cl, struct ubus_msg_buf *ub, struct blob_attr **attr)
{
	struct ubus_object *obj;
	struct ubus_subscription *s;
	uint32_t id;

	if (!attr[UBUS_ATTR_OBJID] || !attr[UBUS_ATTR_TARGET])
		return UBUS_STATUS_INVALID_ARGUMENT;

	obj = ubusd_find_object(blob_get_u32(attr[UBUS_ATTR_OBJID]));
	if (!obj)
		return UBUS_STATUS_NOT_FOUND;

	if (cl != obj->client)
		return UBUS_STATUS_INVALID_ARGUMENT;

	id = blob_get_u32(attr[UBUS_ATTR_TARGET]);
	list_for_each_entry(s, &obj->target_list, target_list) {
		if (s->target->id.id != id)
			continue;

		ubus_unsubscribe(s);
		return 0;
	}

	return UBUS_STATUS_NOT_FOUND;
}

static const ubus_cmd_cb handlers[__UBUS_MSG_LAST] = {
	[UBUS_MSG_PING] = ubusd_send_pong,
	[UBUS_MSG_ADD_OBJECT] = ubusd_handle_add_object,
	[UBUS_MSG_REMOVE_OBJECT] = ubusd_handle_remove_object,
	[UBUS_MSG_LOOKUP] = ubusd_handle_lookup,
	[UBUS_MSG_INVOKE] = ubusd_handle_invoke,
	[UBUS_MSG_STATUS] = ubusd_handle_response,
	[UBUS_MSG_DATA] = ubusd_handle_response,
	[UBUS_MSG_SUBSCRIBE] = ubusd_handle_add_watch,
	[UBUS_MSG_UNSUBSCRIBE] = ubusd_handle_remove_watch,
	[UBUS_MSG_NOTIFY] = ubusd_handle_notify,
};

void ubusd_proto_receive_message(struct ubus_client *cl,
	struct ubus_msg_buf *ub)
{
	printf("-> [msg] %d, %d, %d, %d, len: %d\n", ub->hdr.version,
			ub->hdr.type, ub->hdr.seq, ub->hdr.peer, ub->len);

  // char *str;
	// str = blobmsg_format_json_indent(ub->data, true, 0);
  // printf("-> [msg] %s\n", str);

	ubus_cmd_cb cb = NULL;
	int ret;

	retmsg->hdr.seq = ub->hdr.seq;
	retmsg->hdr.peer = ub->hdr.peer;

	if (ub->hdr.type < __UBUS_MSG_LAST)
		cb = handlers[ub->hdr.type];

	if (ub->hdr.type != UBUS_MSG_STATUS)
		ubus_msg_close_fd(ub);

	if (cb)
		ret = cb(cl, ub, ubus_parse_msg(ub->data));
	else
		ret = UBUS_STATUS_INVALID_COMMAND;

	if (ret == -1)
		return;

	ubus_msg_free(ub);

	*retmsg_data = htonl(ret);
	ubus_msg_send(cl, retmsg, false);
}

//TODO: investigate the tricky ACL
struct ubus_client *ubusd_proto_new_client(int fd, uloop_fd_handler cb)
{
	//printf("-> ubusd_proto_new_client\n");

	struct ubus_client *cl;

	cl = calloc(1, sizeof(*cl));
	if (!cl)
		return NULL;

	if (ubusd_acl_init_client(cl, fd))
		goto free;

	INIT_LIST_HEAD(&cl->objects);
	cl->sock.fd = fd;
	cl->sock.cb = cb;
	cl->pending_msg_fd = -1;

	if (!ubus_alloc_id(&clients, &cl->id, 0))
		goto free;

	if (!ubusd_send_hello(cl))
		goto delete;

	return cl;

delete:
	ubus_free_id(&clients, &cl->id);
free:
	free(cl);
	return NULL;
}

void ubusd_proto_free_client(struct ubus_client *cl)
{
	struct ubus_object *obj;

	while (!list_empty(&cl->objects)) {
		obj = list_first_entry(&cl->objects, struct ubus_object, list);
		ubusd_free_object(obj);
	}

	ubus_free_id(&clients, &cl->id);
}

void ubus_notify_subscription(struct ubus_object *obj)
{
	bool active = !list_empty(&obj->subscribers);
	struct ubus_msg_buf *ub;

	blob_buf_init(&b, 0);
	blob_put_int32(&b, UBUS_ATTR_OBJID, obj->id.id);
	blob_put_int8(&b, UBUS_ATTR_ACTIVE, active);

	ub = ubus_msg_from_blob(false);
	if (!ub)
		return;

	ubus_msg_init(ub, UBUS_MSG_NOTIFY, ++obj->invoke_seq, 0);
	ubus_msg_send(obj->client, ub, true);
}

void ubus_notify_unsubscribe(struct ubus_subscription *s)
{
	struct ubus_msg_buf *ub;

	blob_buf_init(&b, 0);
	blob_put_int32(&b, UBUS_ATTR_OBJID, s->subscriber->id.id);
	blob_put_int32(&b, UBUS_ATTR_TARGET, s->target->id.id);

	ub = ubus_msg_from_blob(false);
	if (ub != NULL) {
		ubus_msg_init(ub, UBUS_MSG_UNSUBSCRIBE, ++s->subscriber->invoke_seq, 0);
		ubus_msg_send(s->subscriber->client, ub, true);
	}

	ubus_unsubscribe(s);
}

static void __constructor ubusd_proto_init(void)
{
	ubus_init_id_tree(&clients);

	blob_buf_init(&b, 0);
	blob_put_int32(&b, UBUS_ATTR_STATUS, 0);

	retmsg = ubus_msg_from_blob(false);
	if (!retmsg)
		exit(1);

	retmsg->hdr.type = UBUS_MSG_STATUS;
	retmsg_data = blob_data(blob_data(retmsg->data));
}
