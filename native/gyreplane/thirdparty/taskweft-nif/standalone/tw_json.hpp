// Minimal TwValue JSON serializer + parser. No Godot dependency.
// Used by tw_rebac, tw_retriever, and tw_bridge.
#pragma once
#include "tw_value.hpp"
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

namespace TwJson {

inline std::string escape_string(const std::string &p_s) {
	std::ostringstream oss;
	oss << '"';
	for (char c : p_s) {
		if (c == '"')       { oss << "\\\""; }
		else if (c == '\\') { oss << "\\\\"; }
		else if (c == '\n') { oss << "\\n"; }
		else if (c == '\r') { oss << "\\r"; }
		else if (c == '\t') { oss << "\\t"; }
		else                { oss << c; }
	}
	oss << '"';
	return oss.str();
}

inline std::string to_json(const TwValue &p_v) {
	std::ostringstream oss;
	switch (p_v.type()) {
		case TwValue::Type::NIL:
			return "null";
		case TwValue::Type::BOOL:
			return p_v.as_bool() ? "true" : "false";
		case TwValue::Type::INT:
			oss << p_v.as_int();
			return oss.str();
		case TwValue::Type::FLOAT:
			oss << p_v.as_float();
			return oss.str();
		case TwValue::Type::STRING:
			return escape_string(p_v.as_string());
		case TwValue::Type::ARRAY: {
			oss << '[';
			bool first = true;
			for (const TwValue &item : p_v.as_array()) {
				if (!first) { oss << ','; }
				oss << to_json(item);
				first = false;
			}
			oss << ']';
			return oss.str();
		}
		case TwValue::Type::DICT: {
			oss << '{';
			bool first = true;
			for (const auto &[key, val] : p_v.as_dict()) {
				if (!first) { oss << ','; }
				oss << escape_string(key) << ':' << to_json(val);
				first = false;
			}
			oss << '}';
			return oss.str();
		}
	}
	return "null";
}

// ---- Parser: JSON text → TwValue -------------------------------------------
// Moved here from tw_loader.hpp so tw_rebac.hpp can parse without pulling in
// tw_loader.hpp → tw_domain.hpp → tw_state.hpp.

inline void skip_ws(const char *&p, const char *end) {
	while (p < end && std::isspace((unsigned char)*p)) ++p;
}

inline TwValue parse_json(const char *&p, const char *end);

inline TwValue parse_json_string(const char *&p, const char *end) {
	++p;
	std::string s;
	while (p < end && *p != '"') {
		if (*p == '\\' && p + 1 < end) {
			++p;
			switch (*p) {
				case '"':  s += '"';  break;
				case '\\': s += '\\'; break;
				case '/':  s += '/';  break;
				case 'n':  s += '\n'; break;
				case 'r':  s += '\r'; break;
				case 't':  s += '\t'; break;
				default:   s += *p;   break;
			}
		} else {
			s += *p;
		}
		++p;
	}
	if (p < end) ++p;
	return TwValue(std::move(s));
}

inline TwValue parse_json_number(const char *&p, const char *end) {
	const char *start = p;
	bool is_float = false;
	if (*p == '-') ++p;
	while (p < end && std::isdigit((unsigned char)*p)) ++p;
	if (p < end && *p == '.') {
		is_float = true; ++p;
		while (p < end && std::isdigit((unsigned char)*p)) ++p;
	}
	if (p < end && (*p == 'e' || *p == 'E')) {
		is_float = true; ++p;
		if (p < end && (*p == '+' || *p == '-')) ++p;
		while (p < end && std::isdigit((unsigned char)*p)) ++p;
	}
	std::string tok(start, p - start);
	// strtod/strtoll (unlike stod/stoll) never throw -- an out-of-range
	// value clamps to +/-HUGE_VAL or LLONG_MIN/LLONG_MAX and sets errno,
	// which is exactly the fallback-to-double behavior the try/catch
	// below used to implement, just without needing exception support.
	if (is_float) return TwValue(std::strtod(tok.c_str(), nullptr));
	errno = 0;
	char *int_end = nullptr;
	long long v = std::strtoll(tok.c_str(), &int_end, 10);
	if (errno == ERANGE) return TwValue(std::strtod(tok.c_str(), nullptr));
	return TwValue((int64_t)v);
}

inline TwValue parse_json(const char *&p, const char *end) {
	skip_ws(p, end);
	if (p >= end) return TwValue{};
	if (*p == '"') return parse_json_string(p, end);
	if (*p == '[') {
		++p;
		TwValue::Array arr;
		skip_ws(p, end);
		if (p < end && *p == ']') { ++p; return TwValue(std::move(arr)); }
		while (p < end) {
			arr.push_back(parse_json(p, end));
			skip_ws(p, end);
			if (p < end && *p == ',') { ++p; continue; }
			break;
		}
		if (p < end && *p == ']') ++p;
		return TwValue(std::move(arr));
	}
	if (*p == '{') {
		++p;
		TwValue::Dict dict;
		skip_ws(p, end);
		if (p < end && *p == '}') { ++p; return TwValue(std::move(dict)); }
		while (p < end) {
			skip_ws(p, end);
			auto key = parse_json_string(p, end);
			skip_ws(p, end);
			if (p < end && *p == ':') ++p;
			dict[key.as_string()] = parse_json(p, end);
			skip_ws(p, end);
			if (p < end && *p == ',') { ++p; continue; }
			break;
		}
		if (p < end && *p == '}') ++p;
		return TwValue(std::move(dict));
	}
	if (*p == '-' || std::isdigit((unsigned char)*p)) return parse_json_number(p, end);
	if (p + 4 <= end && std::strncmp(p, "true",  4) == 0) { p += 4; return TwValue(true); }
	if (p + 5 <= end && std::strncmp(p, "false", 5) == 0) { p += 5; return TwValue(false); }
	if (p + 4 <= end && std::strncmp(p, "null",  4) == 0) { p += 4; return TwValue{}; }
	++p;
	return TwValue{};
}

inline TwValue parse_json_str(const std::string &json) {
	const char *p = json.c_str();
	return parse_json(p, p + json.size());
}

} // namespace TwJson
