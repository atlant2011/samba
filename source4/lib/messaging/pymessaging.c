/* 
   Unix SMB/CIFS implementation.
   Copyright © Jelmer Vernooij <jelmer@samba.org> 2008

   Based on the equivalent for EJS:
   Copyright © Andrew Tridgell <tridge@samba.org> 2005
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <Python.h>
#include "includes.h"
#include "scripting/python/modules.h"
#include "libcli/util/pyerrors.h"
#include "librpc/rpc/pyrpc_util.h"
#include "librpc/ndr/libndr.h"
#include "lib/messaging/messaging.h"
#include "lib/events/events.h"
#include "cluster/cluster.h"
#include "param/param.h"
#include "param/pyparam.h"
#include "librpc/rpc/dcerpc.h"
#include "librpc/gen_ndr/server_id.h"

void initmessaging(void);

extern PyTypeObject imessaging_Type;

static bool server_id_from_py(PyObject *object, struct server_id *server_id)
{
	if (!PyTuple_Check(object)) {
		PyErr_SetString(PyExc_ValueError, "Expected tuple");
		return false;
	}

	if (PyTuple_Size(object) == 3) {
		return PyArg_ParseTuple(object, "iii", &server_id->pid, &server_id->task_id, &server_id->vnn);
	} else {
		int pid, task_id;
		if (!PyArg_ParseTuple(object, "ii", &pid, &task_id))
			return false;
		*server_id = cluster_id(pid, task_id);
		return true;
	}
}

typedef struct {
	PyObject_HEAD
	TALLOC_CTX *mem_ctx;
	struct imessaging_context *msg_ctx;
} imessaging_Object;

static PyObject *py_imessaging_connect(PyTypeObject *self, PyObject *args, PyObject *kwargs)
{
	struct tevent_context *ev;
	const char *kwnames[] = { "own_id", "messaging_path", NULL };
	PyObject *own_id = Py_None;
	const char *imessaging_path = NULL;
	imessaging_Object *ret;

	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|Oz:connect", 
		discard_const_p(char *, kwnames), &own_id, &imessaging_path)) {
		return NULL;
	}

	ret = PyObject_New(imessaging_Object, &imessaging_Type);
	if (ret == NULL)
		return NULL;

	ret->mem_ctx = talloc_new(NULL);

	ev = s4_event_context_init(ret->mem_ctx);

	if (imessaging_path == NULL) {
		imessaging_path = lpcfg_imessaging_path(ret->mem_ctx,
								   py_default_loadparm_context(ret->mem_ctx));
	} else {
		imessaging_path = talloc_strdup(ret->mem_ctx, imessaging_path);
	}

	if (own_id != Py_None) {
		struct server_id server_id;

		if (!server_id_from_py(own_id, &server_id)) 
			return NULL;

		ret->msg_ctx = imessaging_init(ret->mem_ctx,
					       imessaging_path,
					       server_id,
					       ev, true);
	} else {
		ret->msg_ctx = imessaging_client_init(ret->mem_ctx,
						      imessaging_path,
						      ev);
	}

	if (ret->msg_ctx == NULL) {
		PyErr_SetString(PyExc_RuntimeError, "imessaging_connect unable to create a messaging context");
		talloc_free(ret->mem_ctx);
		return NULL;
	}

	return (PyObject *)ret;
}

static void py_imessaging_dealloc(PyObject *self)
{
	imessaging_Object *iface = (imessaging_Object *)self;
	talloc_free(iface->msg_ctx);
	self->ob_type->tp_free(self);
}

static PyObject *py_imessaging_send(PyObject *self, PyObject *args, PyObject *kwargs)
{
	imessaging_Object *iface = (imessaging_Object *)self;
	uint32_t msg_type;
	DATA_BLOB data;
	PyObject *target;
	NTSTATUS status;
	struct server_id server;
	const char *kwnames[] = { "target", "msg_type", "data", NULL };
	int length;

	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Ois#:send", 
		discard_const_p(char *, kwnames), &target, &msg_type, &data.data, &length)) {

		return NULL;
	}

	data.length = length;

	if (!server_id_from_py(target, &server))
		return NULL;

	status = imessaging_send(iface->msg_ctx, server, msg_type, &data);
	if (NT_STATUS_IS_ERR(status)) {
		PyErr_SetNTSTATUS(status);
		return NULL;
	}

	Py_RETURN_NONE;
}

static void py_msg_callback_wrapper(struct imessaging_context *msg, void *private_data,
			       uint32_t msg_type, 
			       struct server_id server_id, DATA_BLOB *data)
{
	PyObject *callback = (PyObject *)private_data;

	PyObject_CallFunction(callback, discard_const_p(char, "i(iii)s#"), msg_type,
			      server_id.pid, server_id.task_id, server_id.vnn,
			      data->data, data->length);
}

static PyObject *py_imessaging_register(PyObject *self, PyObject *args, PyObject *kwargs)
{
	imessaging_Object *iface = (imessaging_Object *)self;
	int msg_type = -1;
	PyObject *callback;
	NTSTATUS status;
	const char *kwnames[] = { "callback", "msg_type", NULL };
	
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|i:register", 
		discard_const_p(char *, kwnames), &callback, &msg_type)) {
		return NULL;
	}

	Py_INCREF(callback);

	if (msg_type == -1) {
		uint32_t msg_type32 = msg_type;
		status = imessaging_register_tmp(iface->msg_ctx, callback,
						py_msg_callback_wrapper, &msg_type32);
		msg_type = msg_type32;
	} else {
		status = imessaging_register(iface->msg_ctx, callback,
				    msg_type, py_msg_callback_wrapper);
	}
	if (NT_STATUS_IS_ERR(status)) {
		PyErr_SetNTSTATUS(status);
		return NULL;
	}

	return PyLong_FromLong(msg_type);
}

static PyObject *py_imessaging_deregister(PyObject *self, PyObject *args, PyObject *kwargs)
{
	imessaging_Object *iface = (imessaging_Object *)self;
	int msg_type = -1;
	PyObject *callback;
	const char *kwnames[] = { "callback", "msg_type", NULL };

	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|i:deregister",
		discard_const_p(char *, kwnames), &callback, &msg_type)) {
		return NULL;
	}

	imessaging_deregister(iface->msg_ctx, msg_type, callback);

	Py_DECREF(callback);

	Py_RETURN_NONE;
}

static PyMethodDef py_imessaging_methods[] = {
	{ "send", (PyCFunction)py_imessaging_send, METH_VARARGS|METH_KEYWORDS,
		"S.send(target, msg_type, data) -> None\nSend a message" },
	{ "register", (PyCFunction)py_imessaging_register, METH_VARARGS|METH_KEYWORDS,
		"S.register(callback, msg_type=None) -> msg_type\nRegister a message handler" },
	{ "deregister", (PyCFunction)py_imessaging_deregister, METH_VARARGS|METH_KEYWORDS,
		"S.deregister(callback, msg_type) -> None\nDeregister a message handler" },
	{ NULL, NULL, 0, NULL }
};

static PyObject *py_imessaging_server_id(PyObject *obj, void *closure)
{
	imessaging_Object *iface = (imessaging_Object *)obj;
	struct server_id server_id = imessaging_get_server_id(iface->msg_ctx);

	return Py_BuildValue("(iii)", server_id.pid, server_id.task_id,
			     server_id.vnn);
}

static PyGetSetDef py_imessaging_getset[] = {
	{ discard_const_p(char, "server_id"), py_imessaging_server_id, NULL,
	  discard_const_p(char, "local server id") },
	{ NULL },
};


PyTypeObject imessaging_Type = {
	PyObject_HEAD_INIT(NULL) 0,
	.tp_name = "messaging.Messaging",
	.tp_basicsize = sizeof(imessaging_Object),
	.tp_flags = Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE,
	.tp_new = py_imessaging_connect,
	.tp_dealloc = py_imessaging_dealloc,
	.tp_methods = py_imessaging_methods,
	.tp_getset = py_imessaging_getset,
	.tp_doc = "Messaging(own_id=None, imessaging_path=None)\n" \
		  "Create a new object that can be used to communicate with the peers in the specified messaging path.\n" \
		  "If no path is specified, the default path from smb.conf will be used."
};

void initmessaging(void)
{
	PyObject *mod;

	if (PyType_Ready(&imessaging_Type) < 0)
		return;

	mod = Py_InitModule3("messaging", NULL, "Internal RPC");
	if (mod == NULL)
		return;

	Py_INCREF((PyObject *)&imessaging_Type);
	PyModule_AddObject(mod, "Messaging", (PyObject *)&imessaging_Type);
}
