#include "shared/read_file.h"

#include <stdio.h>

using namespace std;

string read_file(const string &filename, const unsigned char *start, const size_t size)
{
	FILE *fp = fopen(filename.c_str(), "r");
	if (fp == nullptr) {
		// Fall back to the version we compiled in. (We prefer disk if we can,
		// since that makes it possible to work on shaders without recompiling
		// all the time.)
		if (start != nullptr) {
			return string(reinterpret_cast<const char *>(start),
				reinterpret_cast<const char *>(start) + size);
		}

		perror(filename.c_str());
		exit(1);
	}

	int ret = fseek(fp, 0, SEEK_END);
	if (ret == -1) {
		perror("fseek(SEEK_END)");
		exit(1);
	}

	int disk_size = ftell(fp);
	if (disk_size == -1) {
		perror("ftell");
		exit(1);
	}

	ret = fseek(fp, 0, SEEK_SET);
	if (ret == -1) {
		perror("fseek(SEEK_SET)");
		exit(1);
	}

	string str;
	str.resize(disk_size);
	ret = fread(&str[0], disk_size, 1, fp);
	if (ret == -1) {
		perror("fread");
		exit(1);
	}
	if (ret == 0) {
		fprintf(stderr, "Short read when trying to read %d bytes from %s\n",
		        disk_size, filename.c_str());
		exit(1);
	}
	fclose(fp);

	return str;
}

