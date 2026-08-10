// Monte Carlo plan executor.
// Pure C++20 port of plan/ipyhop/mc_executor.py MonteCarloExecutor.
// No Godot dependency. Stateless: all inputs arrive as JSON strings.
#pragma once
#include "tw_json.hpp"
#include "tw_loader.hpp"
#include "tw_value.hpp"

#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace TwMCExecutor {

// ---- mc_execute -------------------------------------------------------------
// domain_json: JSON-LD domain string (loaded via TwLoader).
// plan_json:   [[action, arg...], ...] — plan to execute.
// probs_json:  [float, ...] — per-step success probability (default 1.0).
// seed:        random seed (default 10, matching Python default).
//
// Returns JSON:
//   {"steps": [{"action": [...], "succeeded": bool, "state_json": "..."|null}],
//    "completed": N, "failed_at": N|null}

inline std::string mc_execute(const std::string &p_domain_json,
		const std::string &p_plan_json,
		const std::string &p_probs_json,
		int64_t p_seed) {
	// Load domain
	TwLoader::TwLoaded loaded = TwLoader::load_json(p_domain_json);
	if (!loaded.state) {
		return "{\"error\":\"failed_to_load_domain\"}";
	}

	// Parse plan
	TwValue plan_val = TwLoader::parse_json_str(p_plan_json);
	std::vector<TwValue> steps;
	if (plan_val.is_array()) {
		steps = plan_val.as_array();
	}

	// Parse probabilities
	TwValue probs_val = TwLoader::parse_json_str(p_probs_json);
	std::vector<double> probs;
	if (probs_val.is_array()) {
		for (const TwValue &p : probs_val.as_array()) {
			probs.push_back(p.is_number() ? p.as_number() : 1.0);
		}
	}

	// Random engine
	std::mt19937_64 rng(static_cast<uint64_t>(p_seed));
	std::uniform_real_distribution<double> dist(0.0, 1.0);

	std::shared_ptr<TwState> state = loaded.state->copy();
	TwValue::Array result_steps;
	int64_t completed = 0;
	bool failed = false;
	int64_t failed_at = -1;

	for (size_t i = 0; i < steps.size(); ++i) {
		const TwValue &step = steps[i];

		// Build action call
		std::string action_name;
		std::vector<TwValue> args;
		if (step.is_array() && !step.as_array().empty()) {
			action_name = step.as_array()[0].as_string();
			for (size_t j = 1; j < step.as_array().size(); ++j) {
				args.push_back(step.as_array()[j]);
			}
		}

		// Success probability for this step
		double prob = (i < probs.size()) ? probs[i] : 1.0;

		// Stochastic outcome: succeed if random draw < probability
		double draw = dist(rng);
		bool succeeded = (draw < prob);

		// Apply action if the step was drawn as successful
		if (succeeded) {
			auto ait = loaded.domain.actions.find(action_name);
			if (ait != loaded.domain.actions.end()) {
				std::shared_ptr<TwState> new_state = ait->second(state->copy(), args);
				if (!new_state) {
					// Action function returned nullptr → treat as failure
					succeeded = false;
				} else {
					state = new_state;
				}
			}
		}

		TwValue::Dict step_dict;
		step_dict["action"] = step;
		step_dict["succeeded"] = TwValue(succeeded);
		if (succeeded) {
			TwValue::Dict state_dict;
			for (const auto &[k, v] : state->vars) {
				state_dict[k] = v;
			}
			step_dict["state_json"] = TwValue(TwJson::to_json(TwValue(std::move(state_dict))));
		} else {
			step_dict["state_json"] = TwValue(); // null
		}

		result_steps.push_back(TwValue(std::move(step_dict)));

		if (!succeeded) {
			failed = true;
			failed_at = static_cast<int64_t>(i);
			break;
		}
		++completed;
	}

	// Build result JSON
	std::ostringstream oss;
	oss << "{\"steps\":" << TwJson::to_json(TwValue(std::move(result_steps)));
	oss << ",\"completed\":" << completed;
	if (failed) {
		oss << ",\"failed_at\":" << failed_at;
	} else {
		oss << ",\"failed_at\":null";
	}
	oss << "}";
	return oss.str();
}

} // namespace TwMCExecutor
