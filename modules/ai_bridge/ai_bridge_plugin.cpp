/**************************************************************************/
/*  ai_bridge_plugin.cpp                                                  */
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

#include "ai_bridge_plugin.h"

#include "ai_bridge_tools.h"

#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/version.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/rich_text_label.h"
#include "servers/display/display_server.h"

static const char *MCP_PROTOCOL_VERSION = "2025-06-18";
static const int MAX_REQUEST_BYTES = 16 * 1024 * 1024;
static const uint64_t CLIENT_TIMEOUT_MSEC = 30000;

// AIBridgeDock

void AIBridgeDock::_copy_command() {
	DisplayServer::get_singleton()->clipboard_set("claude mcp add --transport http godot " + url_edit->get_text());
}

void AIBridgeDock::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		copy_button->set_button_icon(get_editor_theme_icon(SNAME("ActionCopy")));
		clear_button->set_button_icon(get_editor_theme_icon(SNAME("Clear")));
	}
}

void AIBridgeDock::set_status(bool p_running, const String &p_url, const String &p_error) {
	if (p_running) {
		status_label->set_text(TTR("MCP server running"));
		status_label->set_modulate(Color(0.5, 1.0, 0.6));
	} else if (!p_error.is_empty()) {
		status_label->set_text(TTR("MCP server error:") + " " + p_error);
		status_label->set_modulate(Color(1.0, 0.5, 0.5));
	} else {
		status_label->set_text(TTR("MCP server stopped"));
		status_label->set_modulate(Color(1, 1, 1, 0.6));
	}
	url_edit->set_text(p_url);
	enable_button->set_pressed_no_signal(p_running);
}

void AIBridgeDock::add_activity(const String &p_tool, const String &p_summary, bool p_error) {
	const String time = Time::get_singleton()->get_time_string_from_system();
	activity_log->push_color(Color(1, 1, 1, 0.5));
	activity_log->add_text(time + " ");
	activity_log->pop();
	activity_log->push_color(p_error ? Color(1.0, 0.45, 0.45) : Color(0.55, 0.8, 1.0));
	activity_log->push_bold();
	activity_log->add_text(p_tool);
	activity_log->pop();
	activity_log->pop();
	if (!p_summary.is_empty()) {
		activity_log->add_text(" " + p_summary);
	}
	activity_log->add_newline();
}

AIBridgeDock::AIBridgeDock() {
	set_name(TTRC("AI"));
	set_icon_name("Script");
	set_layout_key("AIBridge");
	set_default_slot(EditorDock::DOCK_SLOT_RIGHT_BL);

	VBoxContainer *vb = memnew(VBoxContainer);
	add_child(vb);

	HBoxContainer *top = memnew(HBoxContainer);
	vb->add_child(top);
	status_label = memnew(Label);
	status_label->set_h_size_flags(SIZE_EXPAND_FILL);
	status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	top->add_child(status_label);
	enable_button = memnew(CheckButton);
	enable_button->set_tooltip_text(TTR("Enable the MCP server so AI agents can operate this editor."));
	top->add_child(enable_button);

	HBoxContainer *url_hb = memnew(HBoxContainer);
	vb->add_child(url_hb);
	url_edit = memnew(LineEdit);
	url_edit->set_editable(false);
	url_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	url_hb->add_child(url_edit);
	copy_button = memnew(Button);
	copy_button->set_flat(true);
	copy_button->set_tooltip_text(TTR("Copy the \"claude mcp add\" command for this server."));
	copy_button->connect(SceneStringName(pressed), callable_mp(this, &AIBridgeDock::_copy_command));
	url_hb->add_child(copy_button);

	HBoxContainer *log_hb = memnew(HBoxContainer);
	vb->add_child(log_hb);
	Label *log_label = memnew(Label(TTR("AI activity")));
	log_label->set_h_size_flags(SIZE_EXPAND_FILL);
	log_hb->add_child(log_label);
	clear_button = memnew(Button);
	clear_button->set_flat(true);
	clear_button->set_tooltip_text(TTR("Clear activity log."));
	log_hb->add_child(clear_button);

	activity_log = memnew(RichTextLabel);
	activity_log->set_v_size_flags(SIZE_EXPAND_FILL);
	activity_log->set_scroll_follow(true);
	activity_log->set_selection_enabled(true);
	activity_log->set_context_menu_enabled(true);
	vb->add_child(activity_log);
	clear_button->connect(SceneStringName(pressed), callable_mp(activity_log, &RichTextLabel::clear));
}

// AIBridgePlugin

void AIBridgePlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			dock = memnew(AIBridgeDock);
			dock->get_enable_button()->connect(SceneStringName(toggled), callable_mp(this, &AIBridgePlugin::_toggled));
			add_dock(dock);
			set_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_stop();
			if (dock) {
				remove_dock(dock);
				dock->queue_free();
				dock = nullptr;
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (!start_attempted && EditorNode::get_singleton()->is_editor_ready()) {
				start_attempted = true;
				if ((bool)EDITOR_GET("ai_bridge/enabled")) {
					_start();
				} else {
					dock->set_status(false, String(), String());
				}
			}
			_poll();
		} break;
	}
}

void AIBridgePlugin::_toggled(bool p_enabled) {
	EditorSettings::get_singleton()->set_setting("ai_bridge/enabled", p_enabled);
	EditorSettings::get_singleton()->save();
	if (p_enabled) {
		_start();
	} else {
		_stop();
	}
}

void AIBridgePlugin::_start() {
	_stop();
	bind_host = EDITOR_GET("ai_bridge/bind_host");
	port = EDITOR_GET("ai_bridge/port");
	const char *env_port = getenv("GODOTAI_MCP_PORT");
	if (env_port && String(env_port).is_valid_int()) {
		port = String(env_port).to_int();
	}
	server.instantiate();
	const String url = vformat("http://%s:%d/mcp", bind_host, port);
	Error err = server->listen(port, IPAddress(bind_host));
	if (err != OK) {
		server.unref();
		const String msg = vformat("Failed to listen on %s:%d (%s)", bind_host, port, error_names[err]);
		EditorNode::get_log()->add_message("--- AI bridge: " + msg + " ---", EditorLog::MSG_TYPE_EDITOR);
		dock->set_status(false, url, msg);
		return;
	}
	EditorNode::get_log()->add_message("--- AI bridge MCP server listening on " + url + " ---", EditorLog::MSG_TYPE_EDITOR);
	dock->set_status(true, url, String());
}

void AIBridgePlugin::_stop() {
	for (Client &client : clients) {
		client.peer->disconnect_from_host();
	}
	clients.clear();
	if (server.is_valid()) {
		server->stop();
		server.unref();
	}
	if (dock) {
		dock->set_status(false, vformat("http://%s:%d/mcp", bind_host, port), String());
	}
}

void AIBridgePlugin::_poll() {
	if (server.is_null()) {
		return;
	}
	while (server->is_connection_available()) {
		Client client;
		client.peer = server->take_connection();
		client.connected_msec = OS::get_singleton()->get_ticks_msec();
		clients.push_back(client);
	}
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	for (int i = clients.size() - 1; i >= 0; i--) {
		Client &client = clients.write[i];
		client.peer->poll();
		const StreamPeerTCP::Status status = client.peer->get_status();
		if (status != StreamPeerTCP::STATUS_CONNECTED && status != StreamPeerTCP::STATUS_CONNECTING) {
			clients.remove_at(i);
			continue;
		}
		bool done = false;
		int available = client.peer->get_available_bytes();
		if (available > 0) {
			const int offset = client.buffer.size();
			client.buffer.resize(offset + available);
			int received = 0;
			client.peer->get_partial_data(client.buffer.ptrw() + offset, available, received);
			client.buffer.resize(offset + received);
			if (client.buffer.size() > MAX_REQUEST_BYTES) {
				_send_response(client, 413, "Payload Too Large", "");
				done = true;
			} else {
				done = _try_handle(client);
			}
		}
		if (!done && now - client.connected_msec > CLIENT_TIMEOUT_MSEC) {
			done = true;
		}
		if (done) {
			client.peer->disconnect_from_host();
			clients.remove_at(i);
		}
	}
}

static int _find_header_end(const Vector<uint8_t> &p_buf) {
	for (int i = 0; i + 3 < p_buf.size(); i++) {
		if (p_buf[i] == '\r' && p_buf[i + 1] == '\n' && p_buf[i + 2] == '\r' && p_buf[i + 3] == '\n') {
			return i;
		}
	}
	return -1;
}

static bool _is_local_origin(const String &p_origin) {
	if (p_origin.is_empty() || p_origin == "null") {
		return true;
	}
	for (const String &prefix : { String("http://localhost"), String("http://127.0.0.1"), String("https://localhost"), String("https://127.0.0.1"), String("http://[::1]") }) {
		if (p_origin == prefix || p_origin.begins_with(prefix + ":") || p_origin.begins_with(prefix + "/")) {
			return true;
		}
	}
	return false;
}

bool AIBridgePlugin::_try_handle(Client &p_client) {
	const int header_end = _find_header_end(p_client.buffer);
	if (header_end < 0) {
		return false;
	}
	const String header_text = String::utf8((const char *)p_client.buffer.ptr(), header_end);
	const Vector<String> lines = header_text.split("\r\n");
	const Vector<String> request_line = lines[0].split(" ");
	if (request_line.size() < 2) {
		_send_response(p_client, 400, "Bad Request", "");
		return true;
	}
	const String method = request_line[0];
	const String path = request_line[1].get_slice("?", 0);
	HashMap<String, String> headers;
	for (int i = 1; i < lines.size(); i++) {
		const int colon = lines[i].find_char(':');
		if (colon > 0) {
			headers[lines[i].substr(0, colon).strip_edges().to_lower()] = lines[i].substr(colon + 1).strip_edges();
		}
	}
	const int content_length = headers.has("content-length") ? headers["content-length"].to_int() : 0;
	const int body_start = header_end + 4;
	if (p_client.buffer.size() - body_start < content_length) {
		return false;
	}

	if (!_is_local_origin(headers.has("origin") ? headers["origin"] : String())) {
		_send_response(p_client, 403, "Forbidden", "{\"error\":\"Origin not allowed\"}");
		return true;
	}
	if (path != "/mcp" && path != "/") {
		_send_response(p_client, 404, "Not Found", "{\"error\":\"Use POST /mcp\"}");
		return true;
	}
	if (method == "GET") {
		// No server-initiated streams: tell clients SSE is not offered.
		_send_response(p_client, 405, "Method Not Allowed", "");
		return true;
	}
	if (method == "DELETE") {
		_send_response(p_client, 200, "OK", "");
		return true;
	}
	if (method != "POST") {
		_send_response(p_client, 405, "Method Not Allowed", "");
		return true;
	}

	const String body = String::utf8((const char *)p_client.buffer.ptr() + body_start, content_length);
	Ref<JSON> json;
	json.instantiate();
	if (json->parse(body) != OK) {
		_send_response(p_client, 400, "Bad Request", JSON::stringify(_rpc_error(Variant(), -32700, "Parse error: " + json->get_error_message())));
		return true;
	}
	const Variant request = json->get_data();
	Variant response;
	if (request.get_type() == Variant::ARRAY) {
		Array out;
		const Array batch = request;
		for (const Variant &item : batch) {
			if (item.get_type() == Variant::DICTIONARY) {
				Variant r = _handle_rpc(item);
				if (r.get_type() != Variant::NIL) {
					out.push_back(r);
				}
			}
		}
		if (!out.is_empty()) {
			response = out;
		}
	} else if (request.get_type() == Variant::DICTIONARY) {
		response = _handle_rpc(request);
	} else {
		response = _rpc_error(Variant(), -32600, "Invalid Request");
	}

	if (response.get_type() == Variant::NIL) {
		_send_response(p_client, 202, "Accepted", "");
	} else {
		_send_response(p_client, 200, "OK", JSON::stringify(response, "", false));
	}
	return true;
}

void AIBridgePlugin::_send_response(Client &p_client, int p_code, const String &p_status, const String &p_body, const String &p_content_type) {
	const CharString body = p_body.utf8();
	String head = vformat("HTTP/1.1 %d %s\r\n", p_code, p_status);
	if (body.length() > 0) {
		head += "Content-Type: " + p_content_type + "; charset=utf-8\r\n";
	}
	if (p_code == 405) {
		head += "Allow: POST, DELETE\r\n";
	}
	head += vformat("Content-Length: %d\r\nConnection: close\r\n\r\n", body.length());
	const CharString head_utf8 = head.utf8();
	p_client.peer->set_no_delay(true);
	p_client.peer->put_data((const uint8_t *)head_utf8.get_data(), head_utf8.length());
	if (body.length() > 0) {
		p_client.peer->put_data((const uint8_t *)body.get_data(), body.length());
	}
}

Dictionary AIBridgePlugin::_rpc_error(const Variant &p_id, int p_code, const String &p_message) {
	Dictionary err;
	err["code"] = p_code;
	err["message"] = p_message;
	Dictionary msg;
	msg["jsonrpc"] = "2.0";
	msg["id"] = p_id;
	msg["error"] = err;
	return msg;
}

Variant AIBridgePlugin::_handle_rpc(const Dictionary &p_message) {
	if (!p_message.has("method")) {
		// A response or malformed message; nothing to answer.
		return Variant();
	}
	Variant id = p_message.get("id", Variant());
	if (id.get_type() == Variant::FLOAT && Math::is_equal_approx((double)id, Math::round((double)id))) {
		id = (int64_t)(double)id;
	}
	const bool is_notification = !p_message.has("id");
	const String method = p_message["method"];
	const Dictionary params = p_message.get("params", Dictionary());

	Variant result;
	if (method == "initialize") {
		Dictionary res;
		res["protocolVersion"] = params.get("protocolVersion", MCP_PROTOCOL_VERSION);
		Dictionary caps;
		caps["tools"] = Dictionary();
		res["capabilities"] = caps;
		Dictionary info;
		info["name"] = "godotai-editor";
		info["version"] = GODOT_VERSION_FULL_CONFIG;
		res["serverInfo"] = info;
		res["instructions"] = "You are connected to a running GodotAI (Godot fork) editor. "
							  "Start with editor_get_state and scene_get_tree. All scene edits go through the editor's undo history, "
							  "so the human can undo them with Ctrl+Z. Property values accept JSON numbers/bools/strings, "
							  "Godot literals such as \"Vector2(10, 20)\" or \"Color(1, 0, 0)\", arrays like [10, 20] for vectors, "
							  "\"res://...\" paths for resources, and \"new:ClassName\" or {\"type\": \"ClassName\", ...props} to create resources. "
							  "After running the game with project_run, use debugger_get_errors and log_get to check results.";
		result = res;
	} else if (method == "ping") {
		result = Dictionary();
	} else if (method == "tools/list") {
		Dictionary res;
		res["tools"] = AIBridgeTools::get_tool_definitions();
		result = res;
	} else if (method == "tools/call") {
		const String name = params.get("name", "");
		const Dictionary args = params.get("arguments", Dictionary());
		if (!AIBridgeTools::has_tool(name)) {
			return is_notification ? Variant() : Variant(_rpc_error(id, -32602, "Unknown tool: " + name));
		}
		Dictionary res = AIBridgeTools::call_tool(name, args);
		if (dock) {
			String summary;
			const bool is_error = res.get("isError", false);
			if (is_error) {
				const Array content = res.get("content", Array());
				if (!content.is_empty()) {
					summary = String(Dictionary(content[0]).get("text", "")).left(200);
				}
			} else {
				summary = JSON::stringify(args, "", false).left(160);
			}
			dock->add_activity(name, summary, is_error);
		}
		result = res;
	} else if (method.begins_with("notifications/")) {
		return Variant();
	} else {
		return is_notification ? Variant() : Variant(_rpc_error(id, -32601, "Method not found: " + method));
	}

	if (is_notification) {
		return Variant();
	}
	Dictionary msg;
	msg["jsonrpc"] = "2.0";
	msg["id"] = id;
	msg["result"] = result;
	return msg;
}

AIBridgePlugin::AIBridgePlugin() {
	EDITOR_DEF("ai_bridge/enabled", true);
	EDITOR_DEF("ai_bridge/bind_host", "127.0.0.1");
	EDITOR_DEF("ai_bridge/port", 6010);
	EditorSettings::get_singleton()->add_property_hint(PropertyInfo(Variant::INT, "ai_bridge/port", PROPERTY_HINT_RANGE, "1,65535,1"));
}

AIBridgePlugin::~AIBridgePlugin() {
	_stop();
}
