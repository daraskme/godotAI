/**************************************************************************/
/*  ai_bridge_plugin.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/io/stream_peer_tcp.h"
#include "core/io/tcp_server.h"
#include "editor/docks/editor_dock.h"
#include "editor/plugins/editor_plugin.h"

class Button;
class CheckButton;
class Label;
class LineEdit;
class RichTextLabel;

class AIBridgeDock : public EditorDock {
	GDCLASS(AIBridgeDock, EditorDock);

	Label *status_label = nullptr;
	LineEdit *url_edit = nullptr;
	CheckButton *enable_button = nullptr;
	Button *copy_button = nullptr;
	Button *clear_button = nullptr;
	RichTextLabel *activity_log = nullptr;

	void _copy_command();

protected:
	void _notification(int p_what);

public:
	void set_status(bool p_running, const String &p_url, const String &p_error);
	void add_activity(const String &p_tool, const String &p_summary, bool p_error);
	CheckButton *get_enable_button() const { return enable_button; }

	AIBridgeDock();
};

class AIBridgePlugin : public EditorPlugin {
	GDCLASS(AIBridgePlugin, EditorPlugin);

	struct Client {
		Ref<StreamPeerTCP> peer;
		Vector<uint8_t> buffer;
		uint64_t connected_msec = 0;
	};

	Ref<TCPServer> server;
	Vector<Client> clients;
	AIBridgeDock *dock = nullptr;
	bool start_attempted = false;
	int port = 0;
	String bind_host;

	void _start();
	void _stop();
	void _poll();
	void _toggled(bool p_enabled);
	// Returns true when the request was complete and a response has been sent.
	bool _try_handle(Client &p_client);
	void _send_response(Client &p_client, int p_code, const String &p_status, const String &p_body, const String &p_content_type = "application/json");
	Variant _handle_rpc(const Dictionary &p_message);
	Dictionary _rpc_error(const Variant &p_id, int p_code, const String &p_message);

protected:
	void _notification(int p_what);

public:
	virtual String get_plugin_name() const override { return "AIBridge"; }

	AIBridgePlugin();
	~AIBridgePlugin();
};
