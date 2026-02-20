// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

// Includes
#include <Nodos/Plugin.hpp>
#include <glm/glm.hpp>
#include <Builtins_generated.h>
#include <Module_generated.h>

namespace nos::mediaio
{

nosResult MigrateReadImageToGraph(nosFbNodePtr node, nosBuffer* outBuffer) {
	auto pluginVersion = node->plugin_version();
	bool needsMigration = false;
	// TODO: This was moved from nos.utilities. So check that.
	// needsMigration = !pluginVersion || pluginVersion->major() <= 2 || (pluginVersion->major() == 3 && pluginVersion->minor() < 10);
	if (!needsMigration)
		return NOS_RESULT_SUCCESS;
	fb::TNode cur;
	node->UnPackTo(&cur);
	std::string ReadImageGraphFile = ReadToString(std::string(nosEngine.Plugin->RootFolderPath) + "/Nodes/ReadImage.nosnode");
	auto graphBuffer = GenerateBufferFromJson(NOS_NAME(nos::fb::NodeDefinitions::GetFullyQualifiedName()), ReadImageGraphFile.c_str());
	if (!graphBuffer) {
		// Failed to read graph file
		nosEngine.LogE("Failed to read graph file");
		return NOS_RESULT_FAILED;
	}
	nos::fb::TNodeDefinitions read;
	graphBuffer->As<nos::fb::NodeDefinitions>()->UnPackTo(&read);
	nos::fb::TNode outNode;
	outNode = *read.nodes[0]->node;
	nos::CopyNodeMetaInfo(outNode, cur);
	nos::MigrateJobPinToGraphPin(outNode, cur, "Path", "Path", true);
	nos::MigrateJobPinToGraphPin(outNode, cur, "sRGB", "sRGB", true);
	nos::MigrateJobPinToGraphPin(outNode, cur, "Out", "Out", false);
	nos::MigrateJobPinToGraphPin(outNode, cur, "OutExe", "OnLoaded", false, "OnImageLoaded");
	nos::MigrateJobPinToGraphPin(outNode, cur, "InExe", "Load", false, "ReadImage_Load");
	auto nodeBuffer = EngineBuffer::CopyFrom(outNode);
	*outBuffer = nodeBuffer.Release();
	return NOS_RESULT_SUCCESS;
}

nosResult RegisterReadImage(nosNodeFunctions* outFuncs) {
	*outFuncs = nosNodeFunctions{
		.ClassName = NOS_NAME("nos.mediaio.ReadImage"),
		.MigrateNode = MigrateReadImageToGraph
	};
	return NOS_RESULT_SUCCESS;
}
}