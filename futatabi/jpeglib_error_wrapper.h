#ifndef _JPEGLIB_ERROR_WRAPPER
#define _JPEGLIB_ERROR_WRAPPER 1

/*
  A wrapper class for libjpeg's very cumbersome error handling.
  By default, any error will simply exit(); you can set your own
  error handler, but it can't return. You can't throw exceptions
  through C code legally, so the only real choice is setjmp/longjmp,
  which is also what libjpeg recommends. However, longjmp has
  undefined behavior if a similar try/catch pair would invoke
  running any nontrivial destructors, so it's better to wrap it
  into a common class where we know for sure there are no such
  destructors; we choose to simply convert it into a normal
  true/false idiom for success/failure.

  Use as:

  JPEGWrapErrorManager error_mgr(&cinfo);
  if (!error_mgr.run([&cinfo]{ jpeg_read_header(&cinfo, true); })) {
          // Something went wrong.
          return nullptr;
  }
  if (!error_mgr.run([&cinfo]{ jpeg_start_decompress(&cinfo); })) {
          // Something went wrong.
          return nullptr;
  }
  // etc.

  If you call libjpeg calls outside of run() and they fail, or if
  you declare objects with nontrivial destructors in your lambda
  (including in the capture), you end up in undefined behavior.
 */

#include <jpeglib.h>
#include <setjmp.h>

struct JPEGWrapErrorManager {
	struct jpeg_error_mgr pub;
	jmp_buf setjmp_buffer;

	explicit JPEGWrapErrorManager(jpeg_compress_struct *cinfo)  // Does not take ownership.
	{
		cinfo->err = jpeg_std_error(&pub);
		pub.error_exit = error_exit_thunk;
	}

	explicit JPEGWrapErrorManager(jpeg_decompress_struct *dinfo)  // Does not take ownership.
	{
		dinfo->err = jpeg_std_error(&pub);
		pub.error_exit = error_exit_thunk;
	}

	static void error_exit_thunk(jpeg_common_struct *cinfo)
	{
		((JPEGWrapErrorManager *)cinfo->err)->error_exit(cinfo);
	}

	void error_exit(jpeg_common_struct *cinfo)
	{
		(pub.output_message)(cinfo);
		longjmp(setjmp_buffer, 1);
	}

	// Returns false if and only if the call failed.
	template<class T>
	inline bool run(T &&func)
	{
		if (setjmp(setjmp_buffer)) {
			return false;
		}
		func();
		return true;
	}
};

#endif
