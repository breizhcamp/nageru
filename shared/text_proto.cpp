#include <fcntl.h>
#include <stdio.h>
#include <string>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/message.h>
#include <google/protobuf/text_format.h>

using namespace std;
using namespace google::protobuf;

bool load_proto_from_file(const string &filename, Message *msg)
{
	// Read and parse the protobuf from disk.
	int fd = open(filename.c_str(), O_RDONLY);
	if (fd == -1) {
		perror(filename.c_str());
		return false;
	}
	io::FileInputStream input(fd);  // Takes ownership of fd.
	if (!TextFormat::Parse(&input, msg)) {
		input.Close();
		return false;
	}
	input.Close();
	return true;
}

bool save_proto_to_file(const Message &msg, const string &filename)
{
	// Save to disk. We use the text format because it's friendlier
	// for a user to look at and edit.
	int fd = open(filename.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0666);
	if (fd == -1) {
		perror(filename.c_str());
		return false;
	}
	io::FileOutputStream output(fd);  // Takes ownership of fd.
	if (!TextFormat::Print(msg, &output)) {
		// TODO: Don't overwrite the old file (if any) on error.
		output.Close();
		return false;
	}

	output.Close();
	return true;
}
