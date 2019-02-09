#ifndef _REF_COUNTED_FRAME_H
#define _REF_COUNTED_FRAME_H 1

// A wrapper around FrameAllocator::Frame that is automatically refcounted;
// when the refcount goes to zero, the frame is given back to the allocator.
//
// Note that the important point isn't really the pointer to the Frame itself,
// it's the resources it's representing that need to go back to the allocator.
//
// FIXME: There's an issue here in that we could be releasing a frame while
// we're still uploading textures from it, causing it to be written to in
// another thread. (Thankfully, it goes to the back of the queue, and there's
// usually a render in-between, meaning it's fairly unlikely that someone
// actually managed to get to that race.) We should probably have some mechanism
// for registering fences.

#include <memory>

#include "bmusb/bmusb.h"

void release_refcounted_frame(bmusb::FrameAllocator::Frame *frame);

typedef std::shared_ptr<bmusb::FrameAllocator::Frame> RefCountedFrameBase;

class RefCountedFrame : public RefCountedFrameBase {
public:
	RefCountedFrame() {}

	RefCountedFrame(const bmusb::FrameAllocator::Frame &frame)
		: RefCountedFrameBase(new bmusb::FrameAllocator::Frame(frame), release_refcounted_frame) {}
};

// Similar to RefCountedFrame, but as unique_ptr instead of shared_ptr.

struct Unique_frame_deleter {
	void operator() (bmusb::FrameAllocator::Frame *frame) const {
		release_refcounted_frame(frame);
	}
};

typedef std::unique_ptr<bmusb::FrameAllocator::Frame, Unique_frame_deleter>
	UniqueFrameBase;

class UniqueFrame : public UniqueFrameBase {
public:
	UniqueFrame() {}

	UniqueFrame(const bmusb::FrameAllocator::Frame &frame)
		: UniqueFrameBase(new bmusb::FrameAllocator::Frame(frame)) {}

	bmusb::FrameAllocator::Frame get_and_release()
	{
		bmusb::FrameAllocator::Frame *ptr = release();
		bmusb::FrameAllocator::Frame frame = *ptr;
		delete ptr;
		return frame;
	}
};

#endif  // !defined(_REF_COUNTED_FRAME_H)
