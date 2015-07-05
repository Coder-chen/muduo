// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/EventLoop.h>

#include <muduo/base/Logging.h>
#include <muduo/base/Mutex.h>
#include <muduo/net/Channel.h>
#include <muduo/net/Poller.h>
#include <muduo/net/SocketsOps.h>
#include <muduo/net/TimerQueue.h>

#include <boost/bind.hpp>

#include <signal.h>
#include <sys/eventfd.h>

using namespace muduo;
using namespace muduo::net;

namespace
{
__thread EventLoop* t_loopInThisThread = 0;

const int kPollTimeMs = 10000;

int createEventfd()
{
  int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (evtfd < 0)
  {
    LOG_SYSERR << "Failed in eventfd";
    abort();
  }
  return evtfd;
}

#pragma GCC diagnostic ignored "-Wold-style-cast"
class IgnoreSigPipe
{
 public:
  IgnoreSigPipe()
  {
    ::signal(SIGPIPE, SIG_IGN);
    // LOG_TRACE << "Ignore SIGPIPE";
  }
};
#pragma GCC diagnostic error "-Wold-style-cast"

IgnoreSigPipe initObj;
}

EventLoop* EventLoop::getEventLoopOfCurrentThread()
{
  return t_loopInThisThread;
}
/*
事件循环
*/
EventLoop::EventLoop()
  : looping_(false),                           //初始状态: 即没 loop 由没有 quit
    quit_(false),
    eventHandling_(false),
    callingPendingFunctors_(false),
    iteration_(0),                             //loop循环的次数
    threadId_(CurrentThread::tid()),           //运行该事件循环的线程ID
    poller_(Poller::newDefaultPoller(this)),   //该事件循环的 Poller (构造时传入 this 指针表明所属的关系) ( one thread,one eventloop,one poller )
    timerQueue_(new TimerQueue(this)),         //定时器队列         
    wakeupFd_(createEventfd()),                //创建唤醒该线程的文件描述符
    wakeupChannel_(new Channel(this, wakeupFd_)),//wakeupFd_ ---> wakeupChannel_  (multiply channel)
    currentActiveChannel_(NULL)
{
  LOG_DEBUG << "EventLoop created " << this << " in thread " << threadId_;
  if (t_loopInThisThread)                      //该线程已经有了一个 EventLoop 
  {
    LOG_FATAL << "Another EventLoop " << t_loopInThisThread
              << " exists in this thread " << threadId_;
  }
  else
  {
    t_loopInThisThread = this;                 //初始化该线程的局部变量
  }
  wakeupChannel_->setReadCallback(             //注册 wakeupChannel 的读回掉函数
      boost::bind(&EventLoop::handleRead, this));
  // we are always reading the wakeupfd
  wakeupChannel_->enableReading();             //监控 wakeupChannel 的读事件 一旦有读的事件
}                                              //则调用 EventLoop:: handleRead() 

EventLoop::~EventLoop()
{
  LOG_DEBUG << "EventLoop " << this << " of thread " << threadId_
            << " destructs in thread " << CurrentThread::tid();
  wakeupChannel_->disableAll();               //主要是处理 wakeupFd_ 与 wakeupChannel_ (这两个是由 EventLoop创建的 )
  wakeupChannel_->remove();                   //其余的文件描述符和其对应的channel不是有 EventLoop 创建的
  ::close(wakeupFd_);                         //EventLoop 仅仅是持有它们的指针，不拥有它们的实体     
  t_loopInThisThread = NULL;
}
/*
EventLoop的主体 loop()是一个可控的无限循环
*/
void EventLoop::loop()
{
  assert(!looping_);
  assertInLoopThread();  //不能跨线程调用
  looping_ = true;
  quit_ = false;  // FIXME: what if someone calls quit() before loop() ? // assert(quit_!=true)
  LOG_TRACE << "EventLoop " << this << " start looping";

  while (!quit_)
  {
    activeChannels_.clear();                                        //清除 activeChannels_  
    pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_); //设置 activeChannels_
    ++iteration_;                                                   //循环的次数
    if (Logger::logLevel() <= Logger::TRACE)
    {
      printActiveChannels();
    }
    // TODO sort channel by priority
    eventHandling_ = true;                                   //开始处理事件
    for (ChannelList::iterator it = activeChannels_.begin();
        it != activeChannels_.end(); ++it)
    {
      currentActiveChannel_ = *it;
      currentActiveChannel_->handleEvent(pollReturnTime_);   //调用各个 Channel 对应的事件处理函数
    }
    currentActiveChannel_ = NULL;
    eventHandling_ = false;
    doPendingFunctors();                                    //调用由其它线程设置的相关函数(延时运行)
  }

  LOG_TRACE << "EventLoop " << this << " stop looping";
  looping_ = false;
}
/*
loop()是一个可控的无限循环，要想使得 EventLoop 退出循环，
(1)：在它的 loop() 调用的某个回掉函数中调用 quit(),这样这是最后一次循环
(2): 在其他线程中调用 this thread 中 EventLoop的 quit().
*/
void EventLoop::quit()
{
  quit_ = true;   //quit_=true 与 while(!quit_) 同时发生 这样结果是不确定的 考虑加锁来保证时序
  // There is a chance that loop() just executes while(!quit_) and exists,
  // then EventLoop destructs, then we are accessing an invalid object.
  // Can be fixed using mutex_ in both places.
  if (!isInLoopThread())
  {
    wakeup();
  }
}
/*
确保运行的函数是在 EventLoop::loop() 运行
从线程A中设置在B中需要运行的某个函数(任务) 
*/
void EventLoop::runInLoop(const Functor& cb)
{
  if (isInLoopThread())
  {
    cb();
  }
  else
  {
    queueInLoop(cb);
  }
}

void EventLoop::queueInLoop(const Functor& cb)
{
// loop(...)和 queueInLoop(...) 都会对 pendingFunctors_进行操作 需要加临界区
  {
  MutexLockGuard lock(mutex_);
  pendingFunctors_.push_back(cb);
  }

  if (!isInLoopThread() || callingPendingFunctors_)
  {
    wakeup();
  }
}

/*
添加定时器
*/
TimerId EventLoop::runAt(const Timestamp& time, const TimerCallback& cb)
{
  return timerQueue_->addTimer(cb, time, 0.0);
}

/*
先计算出 delay 后的时间，然后添加一个定时器
*/
TimerId EventLoop::runAfter(double delay, const TimerCallback& cb)
{
  Timestamp time(addTime(Timestamp::now(), delay));
  return runAt(time, cb);
}

/*
添加一个循环定时器 注意 interval
*/
TimerId EventLoop::runEvery(double interval, const TimerCallback& cb)
{
  Timestamp time(addTime(Timestamp::now(), interval));
  return timerQueue_->addTimer(cb, time, interval);
}

#ifdef __GXX_EXPERIMENTAL_CXX0X__
// FIXME: remove duplication
void EventLoop::runInLoop(Functor&& cb)
{
  if (isInLoopThread())
  {
    cb();
  }
  else
  {
    queueInLoop(std::move(cb));
  }
}

void EventLoop::queueInLoop(Functor&& cb)
{
  {
  MutexLockGuard lock(mutex_);
  pendingFunctors_.push_back(std::move(cb));  // emplace_back
  }

  if (!isInLoopThread() || callingPendingFunctors_)
  {
    wakeup();
  }
}

TimerId EventLoop::runAt(const Timestamp& time, TimerCallback&& cb)
{
  return timerQueue_->addTimer(std::move(cb), time, 0.0);
}

TimerId EventLoop::runAfter(double delay, TimerCallback&& cb)
{
  Timestamp time(addTime(Timestamp::now(), delay));
  return runAt(time, std::move(cb));
}

TimerId EventLoop::runEvery(double interval, TimerCallback&& cb)
{
  Timestamp time(addTime(Timestamp::now(), interval));
  return timerQueue_->addTimer(std::move(cb), time, interval);
}
#endif

/*
定时器队列中删除一个 timerId
*/
void EventLoop::cancel(TimerId timerId)
{
  return timerQueue_->cancel(timerId);
}

/*
更新一个 Channel,使得 poller 能够监视该 channel 
*/
void EventLoop::updateChannel(Channel* channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();
  poller_->updateChannel(channel);
}

/*
这是一个实时的 remove 操作，remove 后马上从 poller 中移除监视；
this thread 正在 EventLoop::loop(),只能是从 loop()中的函数间接的调用
跨线程调用会引起混乱( 一个线程同时执行两个函数 )
*/
void EventLoop::removeChannel(Channel* channel)
{
  assert(channel->ownerLoop() == this);    
  assertInLoopThread();
  if (eventHandling_) //正在事件循环中 将要被调用了 此时不能移除
  {
    assert(currentActiveChannel_ == channel || 
        std::find(activeChannels_.begin(), activeChannels_.end(), channel) == activeChannels_.end());
  }
  poller_->removeChannel(channel);  //移除
}

bool EventLoop::hasChannel(Channel* channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();
  return poller_->hasChannel(channel);
}

void EventLoop::abortNotInLoopThread()
{
  LOG_FATAL << "EventLoop::abortNotInLoopThread - EventLoop " << this
            << " was created in threadId_ = " << threadId_
            << ", current thread id = " <<  CurrentThread::tid();
}

/*
往 wakeupFd_ 中写入数据，立即引起 handleRead() 的调用
*/
void EventLoop::wakeup()
{
  uint64_t one = 1;
  ssize_t n = sockets::write(wakeupFd_, &one, sizeof one);
  if (n != sizeof one)
  {
    LOG_ERROR << "EventLoop::wakeup() writes " << n << " bytes instead of 8";
  }
}

/*
handleRead()的调用是在 EventLoop::loop() 使得 poll(...)不被阻塞，继而其它一些事件都中被调用
*/
void EventLoop::handleRead()
{
  uint64_t one = 1;
  ssize_t n = sockets::read(wakeupFd_, &one, sizeof one);
  if (n != sizeof one)
  {
    LOG_ERROR << "EventLoop::handleRead() reads " << n << " bytes instead of 8";
  }
}

void EventLoop::doPendingFunctors()
{
  std::vector<Functor> functors;
  callingPendingFunctors_ = true;

  {
  MutexLockGuard lock(mutex_);
  functors.swap(pendingFunctors_);
  }

  for (size_t i = 0; i < functors.size(); ++i)
  {
    functors[i]();
  }
  callingPendingFunctors_ = false;
}

void EventLoop::printActiveChannels() const
{
  for (ChannelList::const_iterator it = activeChannels_.begin();
      it != activeChannels_.end(); ++it)
  {
    const Channel* ch = *it;
    LOG_TRACE << "{" << ch->reventsToString() << "} ";
  }
}

