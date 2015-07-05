// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/EventLoopThreadPool.h>

#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThread.h>

#include <boost/bind.hpp>

#include <stdio.h>

using namespace muduo;
using namespace muduo::net;


EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, const string& nameArg)
  : baseLoop_(baseLoop),
    name_(nameArg),
    started_(false),
    numThreads_(0),
    next_(0)
{
}

EventLoopThreadPool::~EventLoopThreadPool()
{
  // Don't delete loop, it's stack variable
}

/*
在 TcpServer::start() 中被调用 :
                  开始线程池的运行 
				  客户端的连接
				  分配 IO 线程 
*/
void EventLoopThreadPool::start(const ThreadInitCallback& cb)
{
  assert(!started_);
  baseLoop_->assertInLoopThread();   //断言 是否在主线程中被调用

  started_ = true;

  for (int i = 0; i < numThreads_; ++i)  //numThreads_ 线程池的最大线程数目
  {
    char buf[name_.size() + 32];        //name_ TcpServer的名字
    snprintf(buf, sizeof buf, "%s%d", name_.c_str(), i);
    EventLoopThread* t = new EventLoopThread(cb, buf);
    threads_.push_back(t);              //threads_  EventLoopThread的一个集合( vector ) 
    loops_.push_back(t->startLoop());   //
  }
  if (numThreads_ == 0 && cb)
  {
    cb(baseLoop_);
  }
}
//
EventLoop* EventLoopThreadPool::getNextLoop()
{
  baseLoop_->assertInLoopThread(); //断言 是否在主线程中
  assert(started_);                //断言 started_==true   
  EventLoop* loop = baseLoop_;

  if (!loops_.empty())
  {
    // round-robin 
    loop = loops_[next_];
    ++next_;
    if (implicit_cast<size_t>(next_) >= loops_.size())
    {
      next_ = 0;
    }
  }
  return loop;
}

EventLoop* EventLoopThreadPool::getLoopForHash(size_t hashCode)
{
  baseLoop_->assertInLoopThread();
  EventLoop* loop = baseLoop_;

  if (!loops_.empty())
  {
    loop = loops_[hashCode % loops_.size()];
  }
  return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops()
{
  baseLoop_->assertInLoopThread();
  assert(started_);
  if (loops_.empty())
  {
    return std::vector<EventLoop*>(1, baseLoop_);
  }
  else
  {
    return loops_;
  }
}
