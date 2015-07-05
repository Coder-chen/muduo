// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/EventLoopThread.h>

#include <muduo/net/EventLoop.h>

#include <boost/bind.hpp>

using namespace muduo;
using namespace muduo::net;

/*被 EventLoopThreadPool::start() 调用，用 for(...)循环来生成一个线程池
这是一个 thread 的包装类，这个类(在主线程中创建 EventLoopThreadPool )用来控制生成的子线程 ，
并且这个类还运行了一个 EventLoop */
EventLoopThread::EventLoopThread(const ThreadInitCallback& cb,
                                 const string& name)
  : loop_(NULL),
    exiting_(false),
    thread_(boost::bind(&EventLoopThread::threadFunc, this), name),  //thread 的运行函数 与 thread 的名字
    mutex_(),
    cond_(mutex_),
    callback_(cb)
{
}

EventLoopThread::~EventLoopThread()
{
  exiting_ = true;
  if (loop_ != NULL) // not 100% race-free, eg. threadFunc could be running callback_.
  {
    // still a tiny chance to call destructed object, if threadFunc exits just now.
    // but when EventLoopThread destructs, usually programming is exiting anyway.
    loop_->quit();
    thread_.join();
  }
}
/*
创建一个子线程，并且返回子线程的 EventLoop 指针
相当于子线程的一个包装
*/
EventLoop* EventLoopThread::startLoop()
{
  assert(!thread_.started());
  thread_.start();   //创建线程实体 并且开始运行函数

  {
    MutexLockGuard lock(mutex_);
    while (loop_ == NULL)
    {
      cond_.wait(); //等待条件变量 等待 loop_!= NULL
    }
  }

  return loop_;
}
/*
创建的子线程  运行的函数
callback_ :: TcpServer::start()--> EventLoopThreadPool-->start(cb,)-->EventLoopThread::EventLoopThread()
*/
void EventLoopThread::threadFunc()
{
  EventLoop loop;           // 生成一个事件循环

  if (callback_)           //有回掉函数  ThreadInitCallback
  {
    callback_(&loop);      //在回掉函数中注册事件循环
  }

  {
    MutexLockGuard lock(mutex_);
    loop_ = &loop;        //设置 loop_  
    cond_.notify();       //通知
  }

  loop.loop();            //子线程的事件循环开始可控的无限 loop()
  //assert(exiting_);     // 新的连接还没有建立时，空循环  
  loop_ = NULL;
}

