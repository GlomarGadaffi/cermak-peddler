#ifndef PBX_PERSIST_HPP
#define PBX_PERSIST_HPP

#include <string>
#include <vector>

// Shared NVS-blob conventions for the persisted PBX tables (forwards, ring
// groups, page zones, adopted devices, ...). Each table is serialized as a
// single blob — one record per line, fields tab-separated — under one NVS key,
// so a mutation rewrites exactly one key (bounded size, low flash wear). The
// AOR charset (isValidAor) excludes tab/newline, so the delimiters are safe.

namespace pbxpersist
{
	// NVS namespace holding the PBX config tables (keys capped at 15 chars).
	constexpr auto kNvsNamespace = "pbxcfg";

	// Split a serialized blob into newline-delimited records, then each record
	// into tab-separated fields. Pure; used by the loaders.
	inline std::vector<std::vector<std::string>> deserializeBlob(const std::string& blob)
	{
		std::vector<std::vector<std::string>> records;
		size_t pos = 0;
		while (pos < blob.size())
		{
			size_t nl = blob.find('\n', pos);
			std::string line = blob.substr(pos, (nl == std::string::npos ? blob.size() : nl) - pos);
			pos = (nl == std::string::npos) ? blob.size() : nl + 1;
			if (line.empty()) continue;

			std::vector<std::string> fields;
			size_t fp = 0;
			while (true)
			{
				size_t tab = line.find('\t', fp);
				fields.push_back(line.substr(fp, (tab == std::string::npos ? line.size() : tab) - fp));
				if (tab == std::string::npos) break;
				fp = tab + 1;
			}
			records.push_back(std::move(fields));
		}
		return records;
	}
}

#endif
