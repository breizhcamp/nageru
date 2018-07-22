// Evaluate a .flo file against ground truth,
// outputting the average end-point error.

#include <assert.h>
#include <stdio.h>

#include <memory>

#include "util.h"

using namespace std;

double eval_flow(const char *filename1, const char *filename2);

int main(int argc, char **argv)
{
	double sum_epe = 0.0;
	int num_flows = 0;
	for (int i = 1; i < argc; i += 2) {
		sum_epe += eval_flow(argv[i], argv[i + 1]);
		++num_flows;
	}
	fprintf(stderr, "Average EPE: %.2f pixels\n", sum_epe / num_flows);
}

double eval_flow(const char *filename1, const char *filename2)
{
	Flow flow = read_flow(filename1);
	Flow gt = read_flow(filename2);

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
	return sum / (flow.width * flow.height);
}
