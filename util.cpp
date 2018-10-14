#include "util.h"

#include <assert.h>
#include <memory>
#include <stdio.h>

using namespace std;

Flow read_flow(const char *filename)
{
	FILE *flowfp = fopen(filename, "rb");
	uint32_t hdr, width, height;
	fread(&hdr, sizeof(hdr), 1, flowfp);
	fread(&width, sizeof(width), 1, flowfp);
	fread(&height, sizeof(height), 1, flowfp);

	unique_ptr<Vec2[]> flow(new Vec2[width * height]);
	fread(flow.get(), width * height * sizeof(Vec2), 1, flowfp);

	Flow ret;
	ret.width = width;
	ret.height = height;
	ret.flow = move(flow);
	return ret;
}
