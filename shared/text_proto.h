#ifndef _TEXT_PROTO_H
#define _TEXT_PROTO_H 1

// Utility functions to serialize protobufs on disk.
// We use the text format because it's friendlier
// for a user to look at and edit.

#include <string>

namespace google {
namespace protobuf {
class Message;
}  // namespace protobuf
}  // namespace google

bool load_proto_from_file(const std::string &filename, google::protobuf::Message *msg);
bool save_proto_to_file(const google::protobuf::Message &msg, const std::string &filename);

#endif  // !defined(_TEXT_PROTO_H)
