#ifndef _POST_TO_MAIN_THREAD_H
#define _POST_TO_MAIN_THREAD_H 1

#include <QApplication>
#include <QObject>
#include <future>
#include <memory>

// http://stackoverflow.com/questions/21646467/how-to-execute-a-functor-in-a-given-thread-in-qt-gcd-style
template<typename F>
static inline void post_to_main_thread(F &&fun)
{
	QObject signalSource;
	QObject::connect(&signalSource, &QObject::destroyed, qApp, std::move(fun));
}

template<typename F>
static inline void post_to_main_thread_and_wait(F &&fun)
{
	std::promise<void> done_promise;
	std::future<void> done = done_promise.get_future();
	post_to_main_thread(std::move(fun));
	post_to_main_thread([&done_promise] { done_promise.set_value(); });
	done.wait();
}

#endif  // !defined(_POST_TO_MAIN_THREAD_H)
