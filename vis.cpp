// Visualize a .flo file.

#include <assert.h>
#include <stdio.h>

#include <memory>

#include "util.h"

using namespace std;

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "Usage: ./vis input.flo out.ppm\n");
		exit(1);
	}

	Flow flow = read_flow(argv[1]);

	FILE *fp = fopen(argv[2], "wb");
	fprintf(fp, "P6\n%d %d\n255\n", flow.width, flow.height);
	for (unsigned y = 0; y < unsigned(flow.height); ++y) {
		for (unsigned x = 0; x < unsigned(flow.width); ++x) {
			float du = flow.flow[y * flow.width + x].du;
			float dv = flow.flow[y * flow.width + x].dv;

			uint8_t r, g, b;
			flow2rgb(du, dv, &r, &g, &b);
			putc(r, fp);
			putc(g, fp);
			putc(b, fp);
		}
	}
	fclose(fp);
}
