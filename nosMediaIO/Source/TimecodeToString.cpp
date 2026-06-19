// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

#include <cstdio>
#include <cstring>

#include "ANC_generated.h"

namespace nos::mediaio
{

struct TimecodeToStringNode : NodeContext
{
	TimecodeToStringNode(nosFbNodePtr node) : NodeContext(node) {}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		const Timecode* tc = execParams.GetPinData<Timecode>(NOS_NAME_STATIC("Timecode"));
		if (!tc)
			return NOS_RESULT_FAILED;

		char buf[16];
		std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u%c%02u",
			unsigned(tc->hours()), unsigned(tc->minutes()), unsigned(tc->seconds()),
			tc->drop_frame() ? ';' : ':', unsigned(tc->frames()));
		SetPinValue(NOS_NAME_STATIC("TimecodeStr"), nos::Buffer(buf, std::strlen(buf) + 1));
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterTimecodeToString(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("TimecodeToString"), TimecodeToStringNode, fn)
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::mediaio
