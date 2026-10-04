/**************************************************************************/
/*  ai_bridge_tools.cpp                                                   */
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

#include "ai_bridge_tools.h"

#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "core/object/editor_language.h"
#include "core/object/script_language.h"
#include "core/string/print_string.h"
#include "core/variant/variant_utility.h"
#include "core/version.h"
#include "editor/debugger/editor_debugger_node.h"
#include "editor/debugger/script_editor_debugger.h"
#include "editor/doc/editor_help.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/packed_scene.h"

namespace {

typedef Variant (*ToolFunc)(const Dictionary &p_args, String &r_error);

struct ToolDef {
	const char *name;
	const char *description;
	ToolFunc func;
	Dictionary (*schema)();
};

// Schema helpers.

Dictionary _prop(const String &p_type, const String &p_description) {
	Dictionary d;
	d["type"] = p_type;
	d["description"] = p_description;
	return d;
}

Dictionary _object_schema(const Dictionary &p_props = Dictionary(), const Array &p_required = Array()) {
	Dictionary d;
	d["type"] = "object";
	d["properties"] = p_props;
	if (!p_required.is_empty()) {
		d["required"] = p_required;
	}
	return d;
}

// Common helpers.

Node *_scene_root() {
	return EditorInterface::get_singleton()->get_edited_scene_root();
}

Node *_find_node(const String &p_path, String &r_error) {
	Node *root = _scene_root();
	if (!root) {
		r_error = "No scene is open in the editor. Use scene_open or scene_new first.";
		return nullptr;
	}
	String path = p_path.strip_edges();
	if (path.is_empty() || path == "." || path == String(root->get_name())) {
		return root;
	}
	if (path.begins_with(String(root->get_name()) + "/")) {
		path = path.substr(String(root->get_name()).length() + 1);
	}
	Node *node = root->get_node_or_null(NodePath(path));
	if (!node) {
		r_error = "Node not found: " + p_path + " (paths are relative to the scene root, use \".\" for the root)";
	}
	return node;
}

String _node_path(Node *p_node) {
	Node *root = _scene_root();
	if (!root || p_node == root) {
		return ".";
	}
	return String(root->get_path_to(p_node));
}

bool _check_res_path(const String &p_path, String &r_error) {
	if (!p_path.begins_with("res://")) {
		r_error = "Path must start with res://: " + p_path;
		return false;
	}
	if (p_path.contains("..")) {
		r_error = "Path must not contain '..': " + p_path;
		return false;
	}
	return true;
}

Variant _variant_to_json(const Variant &p_value, int p_depth = 0, int p_max_depth = 4) {
	switch (p_value.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
		case Variant::STRING:
			return p_value;
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
			return String(p_value);
		case Variant::OBJECT: {
			Object *obj = p_value;
			if (!obj) {
				return Variant();
			}
			Resource *res = Object::cast_to<Resource>(obj);
			if (res && !res->get_path().is_empty() && !res->get_path().contains("::")) {
				return res->get_path();
			}
			return "<" + obj->get_class() + ">";
		}
		case Variant::ARRAY: {
			if (p_depth > p_max_depth) {
				return VariantUtilityFunctions::var_to_str(p_value).left(500);
			}
			Array in = p_value;
			Array out;
			for (int i = 0; i < MIN(in.size(), 200); i++) {
				out.push_back(_variant_to_json(in[i], p_depth + 1, p_max_depth));
			}
			return out;
		}
		case Variant::DICTIONARY: {
			if (p_depth > p_max_depth) {
				return VariantUtilityFunctions::var_to_str(p_value).left(500);
			}
			Dictionary in = p_value;
			Dictionary out;
			for (const KeyValue<Variant, Variant> &kv : in) {
				out[String(kv.key)] = _variant_to_json(kv.value, p_depth + 1, p_max_depth);
			}
			return out;
		}
		case Variant::PACKED_STRING_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
			return _variant_to_json(Array(p_value), p_depth, p_max_depth);
		default:
			return VariantUtilityFunctions::var_to_str(p_value);
	}
}

bool _get_property_info(Object *p_object, const String &p_name, PropertyInfo &r_info) {
	List<PropertyInfo> plist;
	p_object->get_property_list(&plist);
	for (const PropertyInfo &pi : plist) {
		if (pi.name == p_name) {
			r_info = pi;
			return true;
		}
	}
	return false;
}

Variant _json_to_variant(const Variant &p_value, const PropertyInfo &p_info, String &r_error);

Object *_create_object(const String &p_class, String &r_error) {
	if (ScriptServer::is_global_class(p_class)) {
		const String path = ScriptServer::get_global_class_path(p_class);
		Ref<Script> scr = ResourceLoader::load(path);
		Object *obj = ClassDB::instantiate(ScriptServer::get_global_class_native_base(p_class));
		if (obj && scr.is_valid()) {
			obj->set_script(scr);
		}
		return obj;
	}
	if (!ClassDB::class_exists(p_class) || !ClassDB::can_instantiate(p_class)) {
		r_error = "Cannot instantiate class: " + p_class;
		return nullptr;
	}
	return ClassDB::instantiate(p_class);
}

bool _apply_properties(Object *p_object, const Dictionary &p_props, String &r_error) {
	for (const KeyValue<Variant, Variant> &kv : p_props) {
		const String name = kv.key;
		if (name == "type") {
			continue;
		}
		PropertyInfo info;
		if (!_get_property_info(p_object, name, info)) {
			r_error = "Unknown property '" + name + "' on " + p_object->get_class();
			return false;
		}
		Variant v = _json_to_variant(kv.value, info, r_error);
		if (!r_error.is_empty()) {
			return false;
		}
		p_object->set(name, v);
	}
	return true;
}

Variant _json_to_variant(const Variant &p_value, const PropertyInfo &p_info, String &r_error) {
	const Variant::Type target = p_info.type;
	if (target == Variant::NIL) {
		if (p_value.get_type() == Variant::STRING) {
			Variant parsed = VariantUtilityFunctions::str_to_var(p_value);
			return parsed.get_type() == Variant::NIL ? p_value : parsed;
		}
		return p_value;
	}
	if (target == Variant::OBJECT) {
		if (p_value.get_type() == Variant::NIL) {
			return Variant();
		}
		if (p_value.get_type() == Variant::DICTIONARY) {
			Dictionary d = p_value;
			String cls = d.get("type", p_info.class_name);
			Object *obj = _create_object(cls, r_error);
			if (!obj) {
				return Variant();
			}
			Ref<RefCounted> holder = Object::cast_to<RefCounted>(obj);
			if (!_apply_properties(obj, d, r_error)) {
				if (holder.is_null()) {
					memdelete(obj);
				}
				return Variant();
			}
			return obj;
		}
		const String s = p_value;
		if (s.is_empty() || s == "null") {
			return Variant();
		}
		if (s.begins_with("new:")) {
			Object *obj = _create_object(s.substr(4), r_error);
			return obj ? Variant(obj) : Variant();
		}
		if (s.begins_with("res://") || s.begins_with("uid://")) {
			Ref<Resource> res = ResourceLoader::load(s);
			if (res.is_null()) {
				r_error = "Failed to load resource: " + s;
				return Variant();
			}
			return res;
		}
		r_error = "Object properties need \"res://path\", \"new:ClassName\" or {\"type\": \"ClassName\", ...}";
		return Variant();
	}
	if (p_value.get_type() == target) {
		return p_value;
	}
	if (p_value.get_type() == Variant::STRING && target != Variant::STRING && target != Variant::STRING_NAME && target != Variant::NODE_PATH) {
		const String s = p_value;
		Variant parsed = VariantUtilityFunctions::str_to_var(s);
		if (parsed.get_type() == target) {
			return parsed;
		}
		if (parsed.get_type() != Variant::NIL) {
			return VariantUtilityFunctions::type_convert(parsed, target);
		}
		if (p_info.hint == PROPERTY_HINT_ENUM && (target == Variant::INT)) {
			const Vector<String> options = p_info.hint_string.split(",");
			int64_t current = 0;
			for (const String &opt : options) {
				const String label = opt.get_slicec(':', 0).strip_edges();
				if (opt.contains_char(':')) {
					current = opt.get_slicec(':', 1).to_int();
				}
				if (label.to_lower() == s.to_lower()) {
					return current;
				}
				current++;
			}
		}
		r_error = "Could not parse value '" + s + "' as " + Variant::get_type_name(target) + ". Use Godot literal syntax, e.g. Vector2(1, 2).";
		return Variant();
	}
	if (p_value.get_type() == Variant::ARRAY) {
		const Array arr = p_value;
		switch (target) {
			case Variant::VECTOR2:
			case Variant::VECTOR2I:
			case Variant::VECTOR3:
			case Variant::VECTOR3I:
			case Variant::VECTOR4:
			case Variant::VECTOR4I:
			case Variant::COLOR:
			case Variant::RECT2:
			case Variant::RECT2I:
			case Variant::QUATERNION: {
				Vector<Variant> args;
				Vector<const Variant *> argptrs;
				args.resize(arr.size());
				for (int i = 0; i < arr.size(); i++) {
					args.write[i] = arr[i];
				}
				for (int i = 0; i < args.size(); i++) {
					argptrs.push_back(&args[i]);
				}
				Variant ret;
				Callable::CallError ce;
				Variant::construct(target, ret, argptrs.ptrw(), argptrs.size(), ce);
				if (ce.error != Callable::CallError::CALL_OK) {
					r_error = "Wrong number of components for " + Variant::get_type_name(target);
					return Variant();
				}
				return ret;
			}
			default:
				break;
		}
	}
	return VariantUtilityFunctions::type_convert(p_value, target);
}

Dictionary _describe_node(Node *p_node, int p_depth, int p_max_depth, bool p_props) {
	Node *root = _scene_root();
	Dictionary d;
	d["name"] = p_node->get_name();
	d["type"] = p_node->get_class();
	d["path"] = _node_path(p_node);
	Ref<Script> scr = p_node->get_script();
	if (scr.is_valid()) {
		d["script"] = scr->get_path();
		if (scr->get_global_name() != StringName()) {
			d["class_name"] = scr->get_global_name();
		}
	}
	const bool is_instance = p_node != root && !p_node->get_scene_file_path().is_empty();
	if (is_instance) {
		d["instance_of"] = p_node->get_scene_file_path();
	}
	if (p_props) {
		Dictionary props;
		List<PropertyInfo> plist;
		p_node->get_property_list(&plist);
		for (const PropertyInfo &pi : plist) {
			if (!(pi.usage & PROPERTY_USAGE_STORAGE) || pi.name == "script" || pi.name.begins_with("metadata/")) {
				continue;
			}
			const Variant v = p_node->get(pi.name);
			bool valid = false;
			const Variant def = ClassDB::class_get_default_property_value(p_node->get_class_name(), pi.name, &valid);
			if (valid && v == def) {
				continue;
			}
			props[pi.name] = _variant_to_json(v);
		}
		d["properties"] = props;
	}
	if (p_max_depth >= 0 && p_depth >= p_max_depth) {
		if (p_node->get_child_count() > 0) {
			d["child_count"] = p_node->get_child_count();
		}
		return d;
	}
	Array children;
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *child = p_node->get_child(i);
		if (child->get_owner() != root && !(is_instance && root->is_editable_instance(p_node))) {
			continue;
		}
		children.push_back(_describe_node(child, p_depth + 1, p_max_depth, p_props));
	}
	if (!children.is_empty()) {
		d["children"] = children;
	}
	return d;
}

// Tools.

Variant tool_editor_get_state(const Dictionary &p_args, String &r_error) {
	Dictionary d;
	d["engine"] = String("GodotAI ") + GODOT_VERSION_FULL_CONFIG;
	d["project_name"] = GLOBAL_GET("application/config/name");
	d["project_path"] = ProjectSettings::get_singleton()->get_resource_path();
	d["main_scene"] = GLOBAL_GET("application/run/main_scene");
	Node *root = _scene_root();
	d["edited_scene"] = root ? Variant(root->get_scene_file_path()) : Variant();
	d["open_scenes"] = EditorInterface::get_singleton()->get_open_scenes();
	Array selected;
	for (Node *n : EditorNode::get_singleton()->get_editor_selection()->get_top_selected_node_list()) {
		selected.push_back(_node_path(n));
	}
	d["selected_nodes"] = selected;
	d["is_playing"] = EditorInterface::get_singleton()->is_playing_scene();
	d["has_unsaved_changes"] = root ? EditorNode::get_singleton()->is_scene_unsaved(EditorNode::get_editor_data().get_edited_scene()) : false;
	return d;
}

Variant tool_scene_get_tree(const Dictionary &p_args, String &r_error) {
	Node *node = _find_node(p_args.get("root", "."), r_error);
	if (!node) {
		return Variant();
	}
	return _describe_node(node, 0, p_args.get("max_depth", -1), p_args.get("include_properties", false));
}

Variant tool_scene_open(const Dictionary &p_args, String &r_error) {
	const String path = p_args.get("path", "");
	if (!_check_res_path(path, r_error)) {
		return Variant();
	}
	if (!FileAccess::exists(path)) {
		r_error = "Scene not found: " + path;
		return Variant();
	}
	EditorInterface::get_singleton()->open_scene_from_path(path);
	return "Opened " + path;
}

Variant tool_scene_new(const Dictionary &p_args, String &r_error) {
	const String path = p_args.get("path", "");
	const String root_type = p_args.get("root_type", "Node2D");
	if (!_check_res_path(path, r_error)) {
		return Variant();
	}
	if (FileAccess::exists(path) && !(bool)p_args.get("overwrite", false)) {
		r_error = "File already exists: " + path + " (pass overwrite=true to replace it)";
		return Variant();
	}
	Object *obj = _create_object(root_type, r_error);
	Node *root = Object::cast_to<Node>(obj);
	if (!root) {
		if (obj && !Object::cast_to<RefCounted>(obj)) {
			memdelete(obj);
		}
		if (r_error.is_empty()) {
			r_error = root_type + " is not a Node type";
		}
		return Variant();
	}
	root->set_name(p_args.get("root_name", path.get_file().get_basename().to_pascal_case()));
	DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(path.get_base_dir()));
	Ref<PackedScene> ps;
	ps.instantiate();
	ps->pack(root);
	memdelete(root);
	Error err = ResourceSaver::save(ps, path);
	if (err != OK) {
		r_error = "Failed to save scene: " + String(error_names[err]);
		return Variant();
	}
	EditorFileSystem::get_singleton()->update_file(path);
	EditorInterface::get_singleton()->open_scene_from_path(path);
	if ((bool)p_args.get("set_as_main", false)) {
		ProjectSettings::get_singleton()->set_setting("application/run/main_scene", path);
		ProjectSettings::get_singleton()->save();
	}
	return "Created and opened " + path;
}

Variant tool_scene_save(const Dictionary &p_args, String &r_error) {
	if (!_scene_root()) {
		r_error = "No scene is open.";
		return Variant();
	}
	if (_scene_root()->get_scene_file_path().is_empty()) {
		r_error = "Scene has never been saved; create scenes with scene_new.";
		return Variant();
	}
	Error err = EditorInterface::get_singleton()->save_scene();
	if (err != OK) {
		r_error = "Save failed: " + String(error_names[err]);
		return Variant();
	}
	return "Saved " + _scene_root()->get_scene_file_path();
}

Variant tool_node_add(const Dictionary &p_args, String &r_error) {
	Node *parent = _find_node(p_args.get("parent", "."), r_error);
	if (!parent) {
		return Variant();
	}
	Node *root = _scene_root();
	const String type = p_args.get("type", "");
	Node *child = nullptr;
	if (type.begins_with("res://") || type.begins_with("uid://")) {
		Ref<PackedScene> ps = ResourceLoader::load(type);
		if (ps.is_null()) {
			r_error = "Failed to load scene: " + type;
			return Variant();
		}
		child = ps->instantiate(PackedScene::GEN_EDIT_STATE_INSTANCE);
		if (!child) {
			r_error = "Failed to instantiate scene: " + type;
			return Variant();
		}
		child->set_scene_file_path(ResourceLoader::path_remap(type));
	} else {
		Object *obj = _create_object(type, r_error);
		child = Object::cast_to<Node>(obj);
		if (!child) {
			if (obj && !Object::cast_to<RefCounted>(obj)) {
				memdelete(obj);
			}
			if (r_error.is_empty()) {
				r_error = type + " is not a Node type";
			}
			return Variant();
		}
	}
	String name = p_args.get("name", "");
	if (name.is_empty()) {
		name = type.begins_with("res://") ? type.get_file().get_basename().to_pascal_case() : type;
	}
	child->set_name(parent->prevalidate_child_name(child, name));
	if (p_args.has("properties")) {
		if (!_apply_properties(child, p_args["properties"], r_error)) {
			memdelete(child);
			return Variant();
		}
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action_for_history("AI: Add " + String(child->get_name()), EditorNode::get_editor_data().get_current_edited_scene_history_id());
	ur->add_do_method(parent, "add_child", child, true);
	ur->add_do_method(child, "set_owner", root);
	ur->add_do_reference(child);
	ur->add_undo_method(parent, "remove_child", child);
	ur->commit_action();
	return _describe_node(child, 0, 0, false);
}

void _collect_owned(Node *p_node, Node *p_owner, List<Node *> &r_list) {
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *c = p_node->get_child(i);
		if (c->get_owner() == p_owner) {
			r_list.push_back(c);
		}
		_collect_owned(c, p_owner, r_list);
	}
}

Variant tool_node_remove(const Dictionary &p_args, String &r_error) {
	Node *node = _find_node(p_args.get("path", ""), r_error);
	if (!node) {
		return Variant();
	}
	Node *root = _scene_root();
	if (node == root) {
		r_error = "Cannot remove the scene root.";
		return Variant();
	}
	Node *parent = node->get_parent();
	List<Node *> owned;
	_collect_owned(node, root, owned);
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action_for_history("AI: Remove " + String(node->get_name()), EditorNode::get_editor_data().get_current_edited_scene_history_id());
	ur->add_do_method(parent, "remove_child", node);
	ur->add_undo_method(parent, "add_child", node, true);
	ur->add_undo_method(parent, "move_child", node, node->get_index(false));
	ur->add_undo_method(node, "set_owner", node->get_owner());
	for (Node *n : owned) {
		ur->add_undo_method(n, "set_owner", root);
	}
	ur->add_undo_reference(node);
	ur->commit_action();
	return "Removed " + String(p_args.get("path", ""));
}

Variant tool_node_set_properties(const Dictionary &p_args, String &r_error) {
	Node *node = _find_node(p_args.get("path", ""), r_error);
	if (!node) {
		return Variant();
	}
	const Dictionary props = p_args.get("properties", Dictionary());
	HashMap<String, Variant> values;
	for (const KeyValue<Variant, Variant> &kv : props) {
		const String name = kv.key;
		PropertyInfo info;
		if (!_get_property_info(node, name, info)) {
			r_error = "Unknown property '" + name + "' on " + node->get_class() + ". Use class_get_info to list properties.";
			return Variant();
		}
		Variant v = _json_to_variant(kv.value, info, r_error);
		if (!r_error.is_empty()) {
			return Variant();
		}
		values[name] = v;
	}
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action("AI: Set properties on " + String(node->get_name()), UndoRedo::MERGE_DISABLE, node);
	for (const KeyValue<String, Variant> &kv : values) {
		ur->add_do_property(node, kv.key, kv.value);
		ur->add_undo_property(node, kv.key, node->get(kv.key));
	}
	ur->commit_action();
	Dictionary result;
	for (const KeyValue<String, Variant> &kv : values) {
		result[kv.key] = _variant_to_json(node->get(kv.key));
	}
	return result;
}

Variant tool_node_get_properties(const Dictionary &p_args, String &r_error) {
	Node *node = _find_node(p_args.get("path", ""), r_error);
	if (!node) {
		return Variant();
	}
	const String filter = p_args.get("filter", "");
	Array out;
	List<PropertyInfo> plist;
	node->get_property_list(&plist);
	for (const PropertyInfo &pi : plist) {
		if (!(pi.usage & PROPERTY_USAGE_EDITOR) || pi.type == Variant::NIL) {
			continue;
		}
		if (!filter.is_empty() && !pi.name.containsn(filter)) {
			continue;
		}
		Dictionary d;
		d["name"] = pi.name;
		d["type"] = pi.type == Variant::OBJECT && !pi.class_name.is_empty() ? String(pi.class_name) : Variant::get_type_name(pi.type);
		d["value"] = _variant_to_json(node->get(pi.name));
		if (pi.hint == PROPERTY_HINT_ENUM) {
			d["enum"] = pi.hint_string;
		}
		out.push_back(d);
	}
	return out;
}

Variant tool_node_reparent(const Dictionary &p_args, String &r_error) {
	Node *node = _find_node(p_args.get("path", ""), r_error);
	if (!node) {
		return Variant();
	}
	Node *new_parent = _find_node(p_args.get("new_parent", ""), r_error);
	if (!new_parent) {
		return Variant();
	}
	Node *root = _scene_root();
	if (node == root || new_parent == node || node->is_ancestor_of(new_parent)) {
		r_error = "Invalid reparent target.";
		return Variant();
	}
	Node *old_parent = node->get_parent();
	List<Node *> owned;
	_collect_owned(node, root, owned);
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action_for_history("AI: Reparent " + String(node->get_name()), EditorNode::get_editor_data().get_current_edited_scene_history_id());
	ur->add_do_method(node, "reparent", new_parent, true);
	ur->add_do_method(node, "set_owner", root);
	for (Node *n : owned) {
		ur->add_do_method(n, "set_owner", root);
	}
	ur->add_undo_method(node, "reparent", old_parent, true);
	ur->add_undo_method(old_parent, "move_child", node, node->get_index(false));
	ur->add_undo_method(node, "set_owner", root);
	for (Node *n : owned) {
		ur->add_undo_method(n, "set_owner", root);
	}
	ur->commit_action();
	return "Moved to " + _node_path(new_parent) + "/" + String(node->get_name());
}

Variant tool_node_select(const Dictionary &p_args, String &r_error) {
	Node *node = _find_node(p_args.get("path", ""), r_error);
	if (!node) {
		return Variant();
	}
	EditorInterface::get_singleton()->edit_node(node);
	return "Selected " + _node_path(node);
}

Variant tool_file_read(const Dictionary &p_args, String &r_error) {
	const String path = p_args.get("path", "");
	if (!_check_res_path(path, r_error)) {
		return Variant();
	}
	Error err;
	String content = FileAccess::get_file_as_string(path, &err);
	if (err != OK) {
		r_error = "Cannot read " + path + ": " + String(error_names[err]);
		return Variant();
	}
	return content;
}

Variant tool_file_write(const Dictionary &p_args, String &r_error) {
	const String path = p_args.get("path", "");
	if (!_check_res_path(path, r_error)) {
		return Variant();
	}
	if (path.begins_with("res://.godot/")) {
		r_error = "Refusing to write into res://.godot/ (editor cache).";
		return Variant();
	}
	const String content = p_args.get("content", "");
	DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(path.get_base_dir()));
	Error err;
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE, &err);
	if (f.is_null()) {
		r_error = "Cannot write " + path + ": " + String(error_names[err]);
		return Variant();
	}
	f->store_string(content);
	f->close();

	Dictionary result;
	result["path"] = path;
	result["bytes"] = content.utf8().length();
	// Hot-reload scripts already loaded by the editor so the change is visible immediately.
	if (ResourceCache::has(path)) {
		Ref<Script> scr = ResourceCache::get_ref(path);
		if (scr.is_valid()) {
			scr->set_source_code(content);
			scr->reload(true);
		}
	}
	ScriptLanguage *lang = ScriptServer::get_language_for_extension(path.get_extension());
	if (lang && lang->get_editor_language()) {
		List<EditorLanguage::ScriptError> errors;
		lang->get_editor_language()->validate(content, path, &errors, nullptr, nullptr, nullptr);
		Array errs;
		for (const EditorLanguage::ScriptError &e : errors) {
			errs.push_back(vformat("%d:%d: %s", e.start_line, e.start_column, e.message));
		}
		result["script_errors"] = errs;
	}
	EditorFileSystem::get_singleton()->update_file(path);
	return result;
}

void _list_dir(const String &p_dir, bool p_recursive, Array &r_out) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}
	da->list_dir_begin();
	for (String f = da->get_next(); !f.is_empty() && r_out.size() < 5000; f = da->get_next()) {
		if (f.begins_with(".")) {
			continue;
		}
		const String full = p_dir.path_join(f);
		if (da->current_is_dir()) {
			r_out.push_back(full + "/");
			if (p_recursive) {
				_list_dir(full, true, r_out);
			}
		} else if (!f.ends_with(".import") && !f.ends_with(".uid")) {
			r_out.push_back(full);
		}
	}
	da->list_dir_end();
}

Variant tool_file_list(const Dictionary &p_args, String &r_error) {
	const String path = p_args.get("path", "res://");
	if (!_check_res_path(path, r_error)) {
		return Variant();
	}
	Array out;
	_list_dir(path, p_args.get("recursive", true), out);
	return out;
}

Variant tool_script_validate(const Dictionary &p_args, String &r_error) {
	String path = p_args.get("path", "res://ai_validate.gd");
	String code = p_args.get("code", "");
	if (!p_args.has("code")) {
		Error err;
		code = FileAccess::get_file_as_string(path, &err);
		if (err != OK) {
			r_error = "Cannot read " + path;
			return Variant();
		}
	}
	ScriptLanguage *lang = ScriptServer::get_language_for_extension(path.get_extension());
	if (!lang || !lang->get_editor_language()) {
		r_error = "No script language for extension: " + path.get_extension();
		return Variant();
	}
	List<EditorLanguage::ScriptError> errors;
	List<EditorLanguage::Warning> warnings;
	const bool valid = lang->get_editor_language()->validate(code, path, &errors, &warnings, nullptr, nullptr);
	Dictionary d;
	d["valid"] = valid;
	Array errs;
	for (const EditorLanguage::ScriptError &e : errors) {
		Dictionary ed;
		ed["line"] = e.start_line;
		ed["column"] = e.start_column;
		ed["message"] = e.message;
		errs.push_back(ed);
	}
	d["errors"] = errs;
	Array warns;
	for (const EditorLanguage::Warning &w : warnings) {
		Dictionary wd;
		wd["line"] = w.start_line;
		wd["code"] = w.string_code;
		wd["message"] = w.message;
		warns.push_back(wd);
	}
	d["warnings"] = warns;
	return d;
}

Variant tool_node_attach_script(const Dictionary &p_args, String &r_error) {
	Node *node = _find_node(p_args.get("path", ""), r_error);
	if (!node) {
		return Variant();
	}
	const String script_path = p_args.get("script", "");
	Variant script_value;
	if (!script_path.is_empty()) {
		Ref<Script> scr = ResourceLoader::load(script_path, "Script", ResourceFormatLoader::CACHE_MODE_REPLACE);
		if (scr.is_null()) {
			r_error = "Failed to load script: " + script_path;
			return Variant();
		}
		script_value = scr;
	}
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action("AI: Attach script to " + String(node->get_name()), UndoRedo::MERGE_DISABLE, node);
	ur->add_do_method(node, "set_script", script_value);
	ur->add_undo_method(node, "set_script", node->get_script());
	ur->commit_action();
	return script_path.is_empty() ? String("Detached script") : "Attached " + script_path;
}

Variant tool_project_run(const Dictionary &p_args, String &r_error) {
	const String scene = p_args.get("scene", "main");
	if ((bool)p_args.get("clear_errors", true)) {
		for (int i = 0; EditorDebuggerNode::get_singleton()->get_debugger(i); i++) {
			EditorDebuggerNode::get_singleton()->get_debugger(i)->clear_errors_list();
		}
	}
	if (scene == "main") {
		if (String(GLOBAL_GET("application/run/main_scene")).is_empty()) {
			r_error = "No main scene set. Use scene_new with set_as_main=true or project_set_setting application/run/main_scene.";
			return Variant();
		}
		EditorInterface::get_singleton()->play_main_scene();
	} else if (scene == "current") {
		EditorInterface::get_singleton()->play_current_scene();
	} else {
		if (!_check_res_path(scene, r_error)) {
			return Variant();
		}
		EditorInterface::get_singleton()->play_custom_scene(scene);
	}
	return "Started " + scene + ". Poll debugger_get_errors / log_get to inspect results, then project_stop.";
}

Variant tool_project_stop(const Dictionary &p_args, String &r_error) {
	EditorInterface::get_singleton()->stop_playing_scene();
	return "Stopped";
}

Variant tool_project_set_setting(const Dictionary &p_args, String &r_error) {
	const String name = p_args.get("name", "");
	if (name.is_empty()) {
		r_error = "name is required";
		return Variant();
	}
	Variant value = p_args.get("value", Variant());
	if (ProjectSettings::get_singleton()->has_setting(name)) {
		PropertyInfo info;
		info.type = ProjectSettings::get_singleton()->get_setting(name).get_type();
		value = _json_to_variant(value, info, r_error);
		if (!r_error.is_empty()) {
			return Variant();
		}
	}
	ProjectSettings::get_singleton()->set_setting(name, value);
	ProjectSettings::get_singleton()->save();
	return name + " = " + VariantUtilityFunctions::var_to_str(value);
}

Variant tool_log_get(const Dictionary &p_args, String &r_error) {
	Array msgs = EditorNode::get_log()->get_recent_messages(p_args.get("max", 100));
	if ((bool)p_args.get("clear", false)) {
		EditorNode::get_log()->clear();
	}
	return msgs;
}

Variant tool_debugger_get_errors(const Dictionary &p_args, String &r_error) {
	const bool project_only = p_args.get("project_only", true);
	Array out;
	bool paused = false;
	for (int i = 0; EditorDebuggerNode::get_singleton()->get_debugger(i); i++) {
		ScriptEditorDebugger *dbg = EditorDebuggerNode::get_singleton()->get_debugger(i);
		paused = paused || dbg->is_breaked();
		for (const Variant &e : dbg->get_runtime_errors()) {
			const Dictionary err = e;
			if (project_only && !String(err["source_file"]).begins_with("res://") && Array(err["callstack"]).is_empty()) {
				continue;
			}
			out.push_back(err);
		}
	}
	Dictionary d;
	d["is_playing"] = EditorInterface::get_singleton()->is_playing_scene();
	d["paused_in_debugger"] = paused;
	d["errors"] = out;
	return d;
}

Variant tool_class_get_info(const Dictionary &p_args, String &r_error) {
	const StringName cls = String(p_args.get("class", ""));
	if (!ClassDB::class_exists(cls)) {
		r_error = "Unknown class: " + String(cls);
		return Variant();
	}
	const bool inherited = p_args.get("include_inherited", false);
	Dictionary d;
	d["class"] = cls;
	Array chain;
	for (StringName c = ClassDB::get_parent_class(cls); c != StringName(); c = ClassDB::get_parent_class(c)) {
		chain.push_back(c);
	}
	d["inherits"] = chain;
	DocTools *docs = EditorHelp::get_doc_data();
	if (docs && docs->class_list.has(cls)) {
		d["description"] = docs->class_list[cls].brief_description.strip_edges();
	}
	Array props;
	List<PropertyInfo> plist;
	ClassDB::get_property_list(cls, &plist, !inherited);
	for (const PropertyInfo &pi : plist) {
		if (!(pi.usage & PROPERTY_USAGE_EDITOR) || pi.type == Variant::NIL) {
			continue;
		}
		props.push_back(pi.name + ": " + (pi.type == Variant::OBJECT && !pi.class_name.is_empty() ? String(pi.class_name) : Variant::get_type_name(pi.type)) + (pi.hint == PROPERTY_HINT_ENUM ? " [" + pi.hint_string + "]" : String()));
	}
	d["properties"] = props;
	Array methods;
	List<MethodInfo> mlist;
	ClassDB::get_method_list(cls, &mlist, !inherited);
	for (const MethodInfo &mi : mlist) {
		if (mi.name.begins_with("_")) {
			continue;
		}
		String sig = mi.name + "(";
		for (int i = 0; i < mi.arguments.size(); i++) {
			sig += (i > 0 ? ", " : "") + mi.arguments[i].name + ": " + Variant::get_type_name(mi.arguments[i].type);
		}
		sig += ") -> " + (mi.return_val.type == Variant::NIL ? String("void") : Variant::get_type_name(mi.return_val.type));
		methods.push_back(sig);
	}
	d["methods"] = methods;
	Array signals;
	List<MethodInfo> slist;
	ClassDB::get_signal_list(cls, &slist, !inherited);
	for (const MethodInfo &mi : slist) {
		signals.push_back(mi.name);
	}
	d["signals"] = signals;
	return d;
}

Variant tool_editor_undo(const Dictionary &p_args, String &r_error) {
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	const bool redo = p_args.get("redo", false);
	const bool ok = redo ? ur->redo() : ur->undo();
	if (!ok) {
		r_error = redo ? "Nothing to redo." : "Nothing to undo.";
		return Variant();
	}
	return redo ? "Redone" : "Undone";
}

// Collect print() output emitted while an editor script runs.
struct PrintCapture {
	PrintHandlerList handler;
	String output;
	static void func(void *p_user, const String &p_string, bool p_error, bool p_rich) {
		PrintCapture *self = static_cast<PrintCapture *>(p_user);
		self->output += (p_error ? "ERROR: " : "") + p_string + "\n";
	}
};

Variant tool_editor_run_script(const Dictionary &p_args, String &r_error) {
	String code = p_args.get("code", "");
	if (!code.contains("func run(")) {
		String body;
		for (const String &line : code.split("\n")) {
			body += "\t" + line + "\n";
		}
		code = "func run():\n" + body;
	}
	if (!code.begins_with("extends") && !code.contains("\nextends ")) {
		code = "@tool\nextends RefCounted\n" + code;
	} else if (!code.contains("@tool")) {
		code = "@tool\n" + code;
	}
	ScriptLanguage *lang = ScriptServer::get_language_for_extension("gd");
	if (!lang) {
		r_error = "GDScript is not available.";
		return Variant();
	}
	List<EditorLanguage::ScriptError> errors;
	if (lang->get_editor_language() && !lang->get_editor_language()->validate(code, "res://__ai_run_script.gd", &errors, nullptr, nullptr, nullptr)) {
		r_error = "Script errors:";
		for (const EditorLanguage::ScriptError &e : errors) {
			r_error += vformat("\n%d:%d: %s", e.start_line, e.start_column, e.message);
		}
		r_error += "\n--- code ---\n" + code;
		return Variant();
	}
	Ref<Script> scr = Object::cast_to<Script>(ClassDB::instantiate("GDScript"));
	scr->set_source_code(code);
	if (scr->reload() != OK) {
		r_error = "Failed to compile script.";
		return Variant();
	}
	const StringName base = scr->get_instance_base_type();
	if (!ClassDB::is_parent_class(base, "RefCounted")) {
		r_error = "Editor scripts must extend RefCounted (or EditorScript).";
		return Variant();
	}
	Ref<RefCounted> obj = Object::cast_to<RefCounted>(ClassDB::instantiate(base));
	obj->set_script(scr);

	PrintCapture capture;
	capture.handler.printfunc = PrintCapture::func;
	capture.handler.userdata = &capture;
	add_print_handler(&capture.handler);
	Variant ret = obj->call("run");
	remove_print_handler(&capture.handler);

	Dictionary d;
	d["result"] = _variant_to_json(ret);
	d["output"] = capture.output;
	return d;
}

Variant tool_editor_screenshot(const Dictionary &p_args, String &r_error) {
	const String target = p_args.get("target", "editor");
	Viewport *vp = nullptr;
	if (target == "2d") {
		vp = EditorInterface::get_singleton()->get_editor_viewport_2d();
	} else if (target == "3d") {
		vp = EditorInterface::get_singleton()->get_editor_viewport_3d(0);
	} else {
		vp = EditorNode::get_singleton()->get_viewport();
	}
	if (!vp || vp->get_texture().is_null()) {
		r_error = "Viewport not available (the editor may be running headless).";
		return Variant();
	}
	Ref<Image> img = vp->get_texture()->get_image();
	if (img.is_null() || img->is_empty()) {
		r_error = "Could not capture image (the editor may be running headless).";
		return Variant();
	}
	const int max_width = p_args.get("max_width", 1280);
	if (max_width > 0 && img->get_width() > max_width) {
		img->resize(max_width, img->get_height() * max_width / img->get_width());
	}
	const Vector<uint8_t> png = img->save_png_to_buffer();
	Dictionary content;
	content["type"] = "image";
	content["mimeType"] = "image/png";
	content["data"] = CryptoCore::b64_encode_str(png.ptr(), png.size());
	Dictionary marker;
	marker["__mcp_content"] = content;
	return marker;
}

// Schemas.

Dictionary s_empty() {
	return _object_schema();
}
Dictionary s_tree() {
	Dictionary p;
	p["root"] = _prop("string", "Node path to start from (default \".\" = scene root).");
	p["max_depth"] = _prop("integer", "Maximum depth (-1 = unlimited).");
	p["include_properties"] = _prop("boolean", "Include non-default stored properties for every node.");
	return _object_schema(p);
}
Dictionary s_path_only() {
	Dictionary p;
	p["path"] = _prop("string", "res:// path.");
	return _object_schema(p, { "path" });
}
Dictionary s_node_path() {
	Dictionary p;
	p["path"] = _prop("string", "Node path relative to the scene root (\".\" = root).");
	return _object_schema(p, { "path" });
}
Dictionary s_scene_new() {
	Dictionary p;
	p["path"] = _prop("string", "res:// path for the new .tscn file.");
	p["root_type"] = _prop("string", "Root node class (Node2D, Node3D, Control, or a class_name). Default Node2D.");
	p["root_name"] = _prop("string", "Root node name (default: derived from file name).");
	p["set_as_main"] = _prop("boolean", "Also set as the project's main scene.");
	p["overwrite"] = _prop("boolean", "Replace an existing file.");
	return _object_schema(p, { "path" });
}
Dictionary s_node_add() {
	Dictionary p;
	p["parent"] = _prop("string", "Parent node path (default \".\").");
	p["type"] = _prop("string", "Node class (e.g. Sprite2D, CharacterBody2D), a script class_name, or a res://*.tscn scene to instance.");
	p["name"] = _prop("string", "Node name.");
	p["properties"] = _prop("object", "Initial property values, e.g. {\"position\": \"Vector2(100, 50)\"}.");
	return _object_schema(p, { "type" });
}
Dictionary s_set_props() {
	Dictionary p;
	p["path"] = _prop("string", "Node path.");
	p["properties"] = _prop("object", "Property name -> value. Values: JSON, Godot literals (\"Vector2(1, 2)\"), [x, y] arrays, \"res://...\", \"new:Class\", {\"type\": \"Class\", ...}.");
	return _object_schema(p, { "path", "properties" });
}
Dictionary s_get_props() {
	Dictionary p;
	p["path"] = _prop("string", "Node path.");
	p["filter"] = _prop("string", "Only properties whose name contains this text.");
	return _object_schema(p, { "path" });
}
Dictionary s_reparent() {
	Dictionary p;
	p["path"] = _prop("string", "Node to move.");
	p["new_parent"] = _prop("string", "New parent node path.");
	return _object_schema(p, { "path", "new_parent" });
}
Dictionary s_attach() {
	Dictionary p;
	p["path"] = _prop("string", "Node path.");
	p["script"] = _prop("string", "res:// script path, or empty to detach.");
	return _object_schema(p, { "path", "script" });
}
Dictionary s_file_write() {
	Dictionary p;
	p["path"] = _prop("string", "res:// path. Parent folders are created.");
	p["content"] = _prop("string", "Full file content.");
	return _object_schema(p, { "path", "content" });
}
Dictionary s_file_list() {
	Dictionary p;
	p["path"] = _prop("string", "res:// directory (default res://).");
	p["recursive"] = _prop("boolean", "Recurse into folders (default true).");
	return _object_schema(p);
}
Dictionary s_validate() {
	Dictionary p;
	p["path"] = _prop("string", "Script path (read from disk when code is omitted; its extension selects the language).");
	p["code"] = _prop("string", "Source to validate instead of the file on disk.");
	return _object_schema(p);
}
Dictionary s_run() {
	Dictionary p;
	p["scene"] = _prop("string", "\"main\" (default), \"current\", or a res:// scene path.");
	p["clear_errors"] = _prop("boolean", "Clear the debugger error list first (default true).");
	return _object_schema(p);
}
Dictionary s_setting() {
	Dictionary p;
	p["name"] = _prop("string", "Setting path, e.g. application/run/main_scene or input/jump.");
	p["value"] = _prop("string", "New value (JSON or Godot literal).");
	return _object_schema(p, { "name", "value" });
}
Dictionary s_log() {
	Dictionary p;
	p["max"] = _prop("integer", "Maximum number of recent messages (default 100).");
	p["clear"] = _prop("boolean", "Clear the Output panel after reading.");
	return _object_schema(p);
}
Dictionary s_errors() {
	Dictionary p;
	p["project_only"] = _prop("boolean", "Only errors from project scripts (res://) or with a script call stack (default true). Set false to include engine/driver errors.");
	return _object_schema(p);
}
Dictionary s_class() {
	Dictionary p;
	p["class"] = _prop("string", "Engine class name, e.g. CharacterBody2D.");
	p["include_inherited"] = _prop("boolean", "Include inherited members (default false).");
	return _object_schema(p, { "class" });
}
Dictionary s_undo() {
	Dictionary p;
	p["redo"] = _prop("boolean", "Redo instead of undo.");
	return _object_schema(p);
}
Dictionary s_run_script() {
	Dictionary p;
	p["code"] = _prop("string", "GDScript statements (wrapped into func run()) or a full script defining func run(). Has full editor access, e.g. EditorInterface. Return a value to receive it.");
	return _object_schema(p, { "code" });
}
Dictionary s_screenshot() {
	Dictionary p;
	p["target"] = _prop("string", "\"editor\" (whole window, default), \"2d\" or \"3d\" viewport.");
	p["max_width"] = _prop("integer", "Downscale to this width (default 1280, 0 = original).");
	return _object_schema(p);
}

const ToolDef TOOLS[] = {
	{ "editor_get_state", "Get project info, the edited scene, open scenes, selection and play state. Call this first.", tool_editor_get_state, s_empty },
	{ "scene_get_tree", "Get the node tree of the scene being edited (names, types, paths, scripts, optionally properties).", tool_scene_get_tree, s_tree },
	{ "scene_open", "Open a scene in the editor (it becomes the edited scene).", tool_scene_open, s_path_only },
	{ "scene_new", "Create a new scene file with a root node and open it.", tool_scene_new, s_scene_new },
	{ "scene_save", "Save the edited scene.", tool_scene_save, s_empty },
	{ "node_add", "Add a node (or instance a scene) to the edited scene. Undoable.", tool_node_add, s_node_add },
	{ "node_remove", "Remove a node from the edited scene. Undoable.", tool_node_remove, s_node_path },
	{ "node_set_properties", "Set one or more properties on a node. Undoable as a single action.", tool_node_set_properties, s_set_props },
	{ "node_get_properties", "List a node's editor-visible properties with types and current values.", tool_node_get_properties, s_get_props },
	{ "node_reparent", "Move a node under a different parent. Undoable.", tool_node_reparent, s_reparent },
	{ "node_attach_script", "Attach (or detach) a script to a node. Undoable.", tool_node_attach_script, s_attach },
	{ "node_select", "Select a node in the editor so the human sees it in the Inspector.", tool_node_select, s_node_path },
	{ "file_read", "Read a text file from the project.", tool_file_read, s_path_only },
	{ "file_write", "Write a text file (scripts, shaders, .tscn, ...). Loaded scripts hot-reload; script errors are returned.", tool_file_write, s_file_write },
	{ "file_list", "List project files (skips hidden folders, .import and .uid files).", tool_file_list, s_file_list },
	{ "script_validate", "Check a script for errors and warnings without running it.", tool_script_validate, s_validate },
	{ "project_run", "Run the game (main scene, current scene, or a given scene).", tool_project_run, s_run },
	{ "project_stop", "Stop the running game.", tool_project_stop, s_empty },
	{ "project_set_setting", "Set and save a ProjectSettings value.", tool_project_set_setting, s_setting },
	{ "log_get", "Read recent messages from the editor Output panel (includes the running game's print output).", tool_log_get, s_log },
	{ "debugger_get_errors", "Get runtime errors and warnings reported by the running (or last run) game, with source file/line and call stack. paused_in_debugger=true means the game stopped on an error; call project_stop.", tool_debugger_get_errors, s_errors },
	{ "class_get_info", "Describe an engine class: inheritance, brief docs, properties, methods, signals.", tool_class_get_info, s_class },
	{ "editor_undo", "Undo (or redo) the last editor action.", tool_editor_undo, s_undo },
	{ "editor_run_script", "Run GDScript inside the editor (like EditorScript) and return its result and print output. Use for anything the other tools cannot do.", tool_editor_run_script, s_run_script },
	{ "editor_screenshot", "Capture a PNG screenshot of the editor window or its 2D/3D viewport.", tool_editor_screenshot, s_screenshot },
};

const ToolDef *_find_tool(const String &p_name) {
	for (const ToolDef &t : TOOLS) {
		if (p_name == t.name) {
			return &t;
		}
	}
	return nullptr;
}

} // namespace

Array AIBridgeTools::get_tool_definitions() {
	Array out;
	for (const ToolDef &t : TOOLS) {
		Dictionary d;
		d["name"] = t.name;
		d["description"] = t.description;
		d["inputSchema"] = t.schema();
		out.push_back(d);
	}
	return out;
}

bool AIBridgeTools::has_tool(const String &p_name) {
	return _find_tool(p_name) != nullptr;
}

Dictionary AIBridgeTools::call_tool(const String &p_name, const Dictionary &p_args) {
	const ToolDef *tool = _find_tool(p_name);
	Dictionary result;
	Array content;
	String error;
	Variant value = tool ? tool->func(p_args, error) : Variant();
	if (!tool) {
		error = "Unknown tool: " + p_name;
	}
	if (!error.is_empty()) {
		Dictionary c;
		c["type"] = "text";
		c["text"] = error;
		content.push_back(c);
		result["isError"] = true;
	} else if (value.get_type() == Variant::DICTIONARY && Dictionary(value).has("__mcp_content")) {
		content.push_back(Dictionary(value)["__mcp_content"]);
		result["isError"] = false;
	} else {
		Dictionary c;
		c["type"] = "text";
		c["text"] = value.get_type() == Variant::STRING ? String(value) : JSON::stringify(_variant_to_json(value, 0, 64), "  ", false);
		content.push_back(c);
		result["isError"] = false;
	}
	result["content"] = content;
	return result;
}
