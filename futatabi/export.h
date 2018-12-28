#ifndef _EXPORT_H
#define _EXPORT_H 1

#include "clip_list.h"

#include <string>
#include <vector>

void export_multitrack_clip(const std::string &filename, const Clip &clip);
void export_interpolated_clip(const std::string &filename, const std::vector<Clip> &clips);

#endif
