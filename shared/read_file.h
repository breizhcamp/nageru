#ifndef _READ_FILE_H
#define _READ_FILE_H 1

#include <string>

#include <stdint.h>

// Read the contents of <filename> and return it as a string.
// If the file does not exist, which is typical outside of development,
// return the given memory area instead (presumably created by bin2h).

std::string read_file(const std::string &filename, const unsigned char *start = nullptr, const size_t size = 0);

#endif
