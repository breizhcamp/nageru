// Evaluate a .flo file against ground truth,
// outputting the average end-point error.

#include <assert.h>
#include <stdio.h>

#include <memory>

#include "util.h"

using namespace std;

int main(int argc, char **argv)
{
	Flow flow = read_flow(argv[1]);
	Flow gt = read_flow(argv[2]);

	double sum = 0.0;
	for (unsigned y = 0; y < unsigned(flow.height); ++y) {
		for (unsigned x = 0; x < unsigned(flow.width); ++x) {
			float du = flow.flow[y * flow.width + x].du;
			float dv = flow.flow[y * flow.width + x].dv;
			float gt_du = gt.flow[y * flow.width + x].du;
			float gt_dv = gt.flow[y * flow.width + x].dv;
			sum += hypot(du - gt_du, dv - gt_dv);
		}
	}
	fprintf(stderr, "Average EPE: %.2f pixels\n", sum / (flow.width * flow.height));
}
