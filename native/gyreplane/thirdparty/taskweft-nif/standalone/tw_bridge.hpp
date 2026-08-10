// Plan-memory bridge utilities.
// Pure C++20 port of plan_memory/bridge.py helpers. No Godot dependency.
// Stateless: inputs arrive as JSON strings, outputs are JSON strings.
#pragma once
#include "tw_json.hpp"
#include "tw_loader.hpp"
#include "tw_rebac.hpp"
#include "tw_value.hpp"

#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace TwBridge {

// ---- Relation keyword map -------------------------------------------------
// Maps first word of a matched verb phrase → RelationshipType name.

inline const std::unordered_map<std::string, std::string> &relation_keywords() {
	static const std::unordered_map<std::string, std::string> kw = {
		{"owns",       "OWNS"},
		{"controls",   "CONTROLS"},
		{"delegated",  "DELEGATED_TO"},
		{"delegates",  "DELEGATED_TO"},
		{"capable",    "HAS_CAPABILITY"},
		{"capability", "HAS_CAPABILITY"},
		{"member",     "IS_MEMBER_OF"},
		{"belongs",    "IS_MEMBER_OF"},
		{"supervises", "SUPERVISOR_OF"},
		{"supervisor", "SUPERVISOR_OF"},
		{"partner",    "PARTNER_OF"},
	};
	return kw;
}

// ---- binding_content -------------------------------------------------------
// Canonical text for a state/goal variable binding: "var arg val"

inline std::string binding_content(const std::string &p_var,
		const std::string &p_arg,
		const std::string &p_val) {
	return p_var + " " + p_arg + " " + p_val;
}

// ---- parse_relation_edges --------------------------------------------------
// Parse relation sentences from facts_json into a TwReBACGraph JSON string.
// facts_json: [{content, trust_score?, ...}]
// Uses the same regex pattern as bridge.py _RE_RELATION.

inline std::string parse_relation_edges(const std::string &p_facts_json,
		double p_trust_threshold = 0.5) {
	TwValue root = TwLoader::parse_json_str(p_facts_json);
	if (!root.is_array()) {
		TwReBAC::TwReBACGraph empty;
		return TwReBAC::graph_to_json(empty);
	}

	// Regex: (subject) (verb) (object)[.?!$]
	// Mirrors Python _RE_RELATION
	static const std::regex RE_REL(
		R"((\w[\w\s]*?)\s+(owns|controls|delegated to|delegates to|has capability|)"
		R"(is member of|belongs to|supervises|partner of)\s+([\w][\w\s]*?)(?:\.|$))",
		std::regex::icase);

	TwReBAC::TwReBACGraph graph;
	const auto &kw = relation_keywords();

	for (const TwValue &item : root.as_array()) {
		if (!item.is_dict()) { continue; }
		const TwValue::Dict &d = item.as_dict();

		// Trust gate
		auto tit = d.find("trust_score");
		if (tit != d.end() && tit->second.is_number()) {
			if (tit->second.as_number() < p_trust_threshold) {
				continue;
			}
		}

		auto cit = d.find("content");
		if (cit == d.end() || !cit->second.is_string()) { continue; }
		const std::string &content = cit->second.as_string();

		std::sregex_iterator it(content.begin(), content.end(), RE_REL);
		std::sregex_iterator end;
		for (; it != end; ++it) {
			const std::smatch &m = *it;
			std::string subj = m[1].str();
			std::string verb = m[2].str();
			std::string obj  = m[3].str();

			// Trim trailing whitespace
			while (!subj.empty() && std::isspace(static_cast<unsigned char>(subj.back()))) {
				subj.pop_back();
			}
			while (!obj.empty() && std::isspace(static_cast<unsigned char>(obj.back()))) {
				obj.pop_back();
			}

			// First word of verb → rel key
			std::string verb_lower = verb;
			for (char &c : verb_lower) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			std::string first_word;
			for (char c : verb_lower) {
				if (c == ' ') { break; }
				first_word += c;
			}

			auto kit = kw.find(first_word);
			if (kit == kw.end()) { continue; }

			TwReBAC::RelationType rel = TwReBAC::parse_rel(kit->second);
			graph.add_edge(subj, obj, rel);
		}
	}

	return TwReBAC::graph_to_json(graph);
}

// ---- extract_state_entities ------------------------------------------------
// Returns inner dict keys from state_json, excluding private/rigid vars.
// state_json: {"var_name": {"arg": "val", ...}, ...}

inline std::vector<std::string> extract_state_entities(const std::string &p_state_json) {
	TwValue root = TwLoader::parse_json_str(p_state_json);
	if (!root.is_dict()) {
		return {};
	}

	std::unordered_set<std::string> seen;
	std::vector<std::string> entities;

	for (const auto &[var_name, bindings] : root.as_dict()) {
		// Skip private / internal / rigid
		if (var_name.empty()) { continue; }
		if (var_name[0] == '_') { continue; }
		if (var_name == "__name__") { continue; }
		if (var_name == "rigid") { continue; }
		if (!bindings.is_dict()) { continue; }

		for (const auto &[arg, _val] : bindings.as_dict()) {
			if (arg.rfind("rigid", 0) == 0) { continue; }
			if (!seen.count(arg)) {
				seen.insert(arg);
				entities.push_back(arg);
			}
		}
	}
	return entities;
}

// ---- plan_result_contents --------------------------------------------------
// Returns JSON array of {content, category, tags} for storing a plan result.
// plan_json: [[action, arg, ...], ...]
// domain: string
// entities_json: ["alice", "bob"]

inline std::string plan_result_contents(const std::string &p_plan_json,
		const std::string &p_domain,
		const std::string &p_entities_json) {
	TwValue plan = TwLoader::parse_json_str(p_plan_json);
	TwValue ents = TwLoader::parse_json_str(p_entities_json);

	std::vector<std::string> entity_names;
	if (ents.is_array()) {
		for (const TwValue &e : ents.as_array()) {
			if (e.is_string()) {
				entity_names.push_back(e.as_string());
			}
		}
	}

	size_t step_count = plan.is_array() ? plan.as_array().size() : 0;

	// Entity string (up to 5)
	std::ostringstream entity_oss;
	for (size_t i = 0; i < entity_names.size() && i < 5; ++i) {
		if (i) { entity_oss << ", "; }
		entity_oss << entity_names[i];
	}

	// Summary fact
	std::string summary = "Plan for " + p_domain + ": " +
			std::to_string(step_count) + " steps involving " + entity_oss.str() + ".";

	TwValue::Array results;

	TwValue::Dict sum_dict;
	sum_dict["content"]  = TwValue(summary);
	sum_dict["category"] = TwValue(std::string("planning"));
	sum_dict["tags"]     = TwValue(p_domain);
	results.push_back(TwValue(std::move(sum_dict)));

	// Per-step facts (up to 20)
	if (plan.is_array()) {
		const auto &steps = plan.as_array();
		size_t cap = steps.size() < 20 ? steps.size() : 20;
		for (size_t i = 0; i < cap; ++i) {
			const TwValue &step = steps[i];
			std::string action_name;
			std::string args_str;
			if (step.is_array() && !step.as_array().empty()) {
				action_name = step.as_array()[0].as_string();
				for (size_t j = 1; j < step.as_array().size(); ++j) {
					if (j > 1) { args_str += ", "; }
					args_str += step.as_array()[j].as_string();
				}
			}
			std::string content = "Plan step " + std::to_string(i + 1) + ": " +
					action_name + "(" + args_str + ") in " + p_domain + ".";

			TwValue::Dict step_dict;
			step_dict["content"]  = TwValue(std::move(content));
			step_dict["category"] = TwValue(std::string("planning"));
			step_dict["tags"]     = TwValue(p_domain);
			results.push_back(TwValue(std::move(step_dict)));
		}
	}

	return TwJson::to_json(TwValue(std::move(results)));
}

// ---- state_bindings_contents -----------------------------------------------
// Returns JSON array of {content, category, tags} for all (var, arg, val) triples.

inline std::string state_bindings_contents(const std::string &p_state_json,
		const std::string &p_domain,
		const std::string &p_category) {
	TwValue root = TwLoader::parse_json_str(p_state_json);
	if (!root.is_dict()) {
		return "[]";
	}

	TwValue::Array results;

	for (const auto &[var_name, bindings] : root.as_dict()) {
		if (var_name.empty() || var_name[0] == '_') { continue; }
		if (var_name == "__name__" || var_name == "rigid") { continue; }
		if (!bindings.is_dict()) { continue; }

		for (const auto &[arg, val] : bindings.as_dict()) {
			std::string content = binding_content(var_name, arg, val.as_string());
			TwValue::Dict d;
			d["content"]  = TwValue(std::move(content));
			d["category"] = TwValue(p_category);
			d["tags"]     = TwValue(p_domain);
			results.push_back(TwValue(std::move(d)));
		}
	}

	return TwJson::to_json(TwValue(std::move(results)));
}

} // namespace TwBridge
