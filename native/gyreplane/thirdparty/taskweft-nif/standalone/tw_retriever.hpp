// Hybrid keyword/HRR retrieval scoring.
// Pure C++20 port of holographic/retrieval.py. No Godot dependency.
// Stateless: all inputs arrive as JSON strings + byte blobs.
#pragma once
#include "tw_hrr.hpp"
#include "tw_json.hpp"
#include "tw_loader.hpp"
#include "tw_value.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace TwRetriever {

// ---- Tokenize ---------------------------------------------------------------

inline std::unordered_set<std::string> tokenize(const std::string &p_text) {
	static const std::string STRIP = ".,;:!?\"'()[]{}#@<>";
	std::unordered_set<std::string> tokens;
	std::istringstream ss(p_text);
	std::string word;
	while (ss >> word) {
		// Lowercase
		for (char &c : word) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		// Strip leading punctuation
		size_t start = 0;
		while (start < word.size() && STRIP.find(word[start]) != std::string::npos) {
			++start;
		}
		// Strip trailing punctuation
		size_t end = word.size();
		while (end > start && STRIP.find(word[end - 1]) != std::string::npos) {
			--end;
		}
		if (end > start) {
			tokens.insert(word.substr(start, end - start));
		}
	}
	return tokens;
}

// ---- Jaccard similarity -----------------------------------------------------

inline double jaccard(const std::unordered_set<std::string> &p_a,
		const std::unordered_set<std::string> &p_b) {
	if (p_a.empty() || p_b.empty()) {
		return 0.0;
	}
	size_t intersection = 0;
	for (const std::string &t : p_a) {
		if (p_b.count(t)) {
			++intersection;
		}
	}
	size_t union_size = p_a.size() + p_b.size() - intersection;
	return union_size > 0 ? static_cast<double>(intersection) / union_size : 0.0;
}

// ---- Temporal decay ---------------------------------------------------------
// Returns 0.5^(age_days / half_life_days). Returns 1.0 if disabled.

inline double temporal_decay(double p_age_days, double p_half_life_days) {
	if (p_half_life_days <= 0.0 || p_age_days < 0.0) {
		return 1.0;
	}
	return std::pow(0.5, p_age_days / p_half_life_days);
}

// ---- score_candidates -------------------------------------------------------
// candidates_json: [{fact_id, content, trust_score, tags, fts_rank,
//                    hrr_vector (bytes), age_days (optional)}]
// query_text: raw query string (for Jaccard)
// query_hrr_bytes: phases as little-endian float64 bytes
// Returns: scored JSON array sorted desc, hrr_vector stripped.

inline std::string score_candidates(const std::string &p_candidates_json,
		const std::string &p_query_text,
		const std::string &p_query_hrr_bytes,
		double p_fts_w, double p_jaccard_w, double p_hrr_w,
		double p_half_life_days,
		int64_t p_dim) {
	TwValue root = TwLoader::parse_json_str(p_candidates_json);
	if (!root.is_array()) {
		return "[]";
	}

	// Decode query HRR
	const uint8_t *qptr = reinterpret_cast<const uint8_t *>(p_query_hrr_bytes.data());
	TwHRR::PhaseVec query_vec = TwHRR::bytes_to_phases(qptr, p_query_hrr_bytes.size());
	std::unordered_set<std::string> query_tokens = tokenize(p_query_text);

	struct Scored {
		TwValue fact;
		double score = 0.0;
	};
	std::vector<Scored> scored;
	scored.reserve(root.as_array().size());

	for (const TwValue &item : root.as_array()) {
		if (!item.is_dict()) {
			continue;
		}
		// Extract fields
		const TwValue::Dict &d = item.as_dict();

		auto get_str = [&](const char *key, const std::string &def = "") -> std::string {
			auto it = d.find(key);
			return it != d.end() && it->second.is_string() ? it->second.as_string() : def;
		};
		auto get_dbl = [&](const char *key, double def = 0.0) -> double {
			auto it = d.find(key);
			if (it == d.end()) return def;
			return it->second.is_number() ? it->second.as_number() : def;
		};

		std::string content  = get_str("content");
		std::string tags     = get_str("tags");
		double trust         = get_dbl("trust_score", 0.5);
		double fts_rank      = get_dbl("fts_rank", 0.0);
		double age_days      = get_dbl("age_days", -1.0);

		// Jaccard
		std::unordered_set<std::string> content_tokens = tokenize(content);
		std::unordered_set<std::string> tag_tokens     = tokenize(tags);
		// Merge content + tag tokens for Jaccard
		std::unordered_set<std::string> all_tokens = content_tokens;
		all_tokens.insert(tag_tokens.begin(), tag_tokens.end());
		double jac = jaccard(query_tokens, all_tokens);

		// HRR similarity
		double hrr_sim = 0.5; // neutral if no vector
		auto hvit = d.find("hrr_vector");
		if (hvit != d.end() && hvit->second.is_string() && p_hrr_w > 0.0) {
			const std::string &hv = hvit->second.as_string();
			const uint8_t *hptr = reinterpret_cast<const uint8_t *>(hv.data());
			if (!query_vec.empty() && hv.size() > 0) {
				TwHRR::PhaseVec fact_vec = TwHRR::bytes_to_phases(hptr, hv.size());
				if (!fact_vec.empty()) {
					hrr_sim = (TwHRR::similarity(query_vec, fact_vec) + 1.0) / 2.0;
				}
			}
		}

		double relevance = p_fts_w * fts_rank + p_jaccard_w * jac + p_hrr_w * hrr_sim;
		double decay     = temporal_decay(age_days, p_half_life_days);
		double score     = relevance * trust * decay;

		// Build output fact (strip hrr_vector)
		TwValue::Dict out_dict;
		for (const auto &[k, v] : d) {
			if (k != "hrr_vector") {
				out_dict[k] = v;
			}
		}
		out_dict["score"] = TwValue(score);
		scored.push_back({TwValue(std::move(out_dict)), score});
	}

	std::stable_sort(scored.begin(), scored.end(),
			[](const Scored &a, const Scored &b) { return a.score > b.score; });

	std::ostringstream oss;
	oss << '[';
	for (size_t i = 0; i < scored.size(); ++i) {
		if (i) { oss << ','; }
		oss << TwJson::to_json(scored[i].fact);
	}
	oss << ']';
	return oss.str();
}

// ---- probe_score ------------------------------------------------------------
// candidates_json: [{fact_id, content, trust_score, binding_vector (bytes)}]
// entity_hrr_bytes: entity atom vector as bytes
// Returns: scored JSON sorted desc, binding_vector stripped.

inline std::string probe_score(const std::string &p_candidates_json,
		const std::string &p_entity_hrr_bytes,
		int64_t p_dim) {
	TwValue root = TwLoader::parse_json_str(p_candidates_json);
	if (!root.is_array()) {
		return "[]";
	}

	const uint8_t *eptr = reinterpret_cast<const uint8_t *>(p_entity_hrr_bytes.data());
	TwHRR::PhaseVec entity_vec = TwHRR::bytes_to_phases(eptr, p_entity_hrr_bytes.size());

	struct Scored {
		TwValue fact;
		double score = 0.0;
	};
	std::vector<Scored> scored;

	for (const TwValue &item : root.as_array()) {
		if (!item.is_dict()) { continue; }
		const TwValue::Dict &d = item.as_dict();

		auto get_str = [&](const char *key, const std::string &def = "") -> std::string {
			auto it = d.find(key);
			return it != d.end() && it->second.is_string() ? it->second.as_string() : def;
		};
		auto get_dbl = [&](const char *key, double def = 0.0) -> double {
			auto it = d.find(key);
			if (it == d.end()) return def;
			return it->second.is_number() ? it->second.as_number() : def;
		};

		std::string content = get_str("content");
		double trust        = get_dbl("trust_score", 0.5);

		// Exact algebraic extraction: unbind(binding, entity) ≈ content
		double hrr_sim = 0.5;
		auto bvit = d.find("binding_vector");
		if (bvit != d.end() && bvit->second.is_string() && !entity_vec.empty()) {
			const std::string &bv = bvit->second.as_string();
			const uint8_t *bptr = reinterpret_cast<const uint8_t *>(bv.data());
			TwHRR::PhaseVec binding = TwHRR::bytes_to_phases(bptr, bv.size());
			if (!binding.empty()) {
				TwHRR::PhaseVec recovered = TwHRR::unbind(binding, entity_vec);
				TwHRR::PhaseVec content_vec = TwHRR::encode_text(content, static_cast<int>(p_dim));
				hrr_sim = TwHRR::similarity(recovered, content_vec);
			}
		}

		double score = (hrr_sim + 1.0) / 2.0 * trust;

		TwValue::Dict out_dict;
		for (const auto &[k, v] : d) {
			if (k != "binding_vector") {
				out_dict[k] = v;
			}
		}
		out_dict["score"] = TwValue(score);
		scored.push_back({TwValue(std::move(out_dict)), score});
	}

	std::stable_sort(scored.begin(), scored.end(),
			[](const Scored &a, const Scored &b) { return a.score > b.score; });

	std::ostringstream oss;
	oss << '[';
	for (size_t i = 0; i < scored.size(); ++i) {
		if (i) { oss << ','; }
		oss << TwJson::to_json(scored[i].fact);
	}
	oss << ']';
	return oss.str();
}

// ---- reason_score -----------------------------------------------------------
// candidates_json: [{fact_id, content, trust_score}]
// entity_hrr_bytes_list: list of entity vectors encoded as binary strings (one per entity)
//   wire format: JSON array of base64? No — Fine passes as vector<string>.
// Each entity HRR is compared via encode_binding on the fly.
// Uses min-sim across entities (AND semantics).

inline std::string reason_score(const std::string &p_candidates_json,
		const std::vector<std::string> &p_entity_hrr_bytes_list,
		int64_t p_dim) {
	TwValue root = TwLoader::parse_json_str(p_candidates_json);
	if (!root.is_array() || p_entity_hrr_bytes_list.empty()) {
		return "[]";
	}

	// Decode entity vectors
	std::vector<TwHRR::PhaseVec> entity_vecs;
	entity_vecs.reserve(p_entity_hrr_bytes_list.size());
	for (const std::string &bytes : p_entity_hrr_bytes_list) {
		const uint8_t *ptr = reinterpret_cast<const uint8_t *>(bytes.data());
		entity_vecs.push_back(TwHRR::bytes_to_phases(ptr, bytes.size()));
	}

	struct Scored {
		TwValue fact;
		double score = 0.0;
	};
	std::vector<Scored> scored;

	for (const TwValue &item : root.as_array()) {
		if (!item.is_dict()) { continue; }
		const TwValue::Dict &d = item.as_dict();

		auto get_str = [&](const char *key, const std::string &def = "") -> std::string {
			auto it = d.find(key);
			return it != d.end() && it->second.is_string() ? it->second.as_string() : def;
		};
		auto get_dbl = [&](const char *key, double def = 0.0) -> double {
			auto it = d.find(key);
			if (it == d.end()) return def;
			return it->second.is_number() ? it->second.as_number() : def;
		};

		std::string content = get_str("content");
		double trust        = get_dbl("trust_score", 0.5);

		TwHRR::PhaseVec content_vec = TwHRR::encode_text(content, static_cast<int>(p_dim));
		double min_sim = 1.0;

		for (const TwHRR::PhaseVec &ev : entity_vecs) {
			if (ev.empty()) { continue; }
			// encode_binding on the fly, then unbind to recover content
			TwHRR::PhaseVec bound = TwHRR::bind(content_vec, ev);
			TwHRR::PhaseVec recovered = TwHRR::unbind(bound, ev);
			double sim = TwHRR::similarity(recovered, content_vec);
			if (sim < min_sim) {
				min_sim = sim;
			}
		}

		double score = (min_sim + 1.0) / 2.0 * trust;
		TwValue::Dict out_dict;
		for (const auto &[k, v] : d) {
			out_dict[k] = v;
		}
		out_dict["score"] = TwValue(score);
		scored.push_back({TwValue(std::move(out_dict)), score});
	}

	std::stable_sort(scored.begin(), scored.end(),
			[](const Scored &a, const Scored &b) { return a.score > b.score; });

	std::ostringstream oss;
	oss << '[';
	for (size_t i = 0; i < scored.size(); ++i) {
		if (i) { oss << ','; }
		oss << TwJson::to_json(scored[i].fact);
	}
	oss << ']';
	return oss.str();
}

} // namespace TwRetriever
