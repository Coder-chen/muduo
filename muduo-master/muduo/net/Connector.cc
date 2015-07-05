// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//

#include <muduo/net/Connector.h>

#include <muduo/base/Logging.h>
#include <muduo/net/Channel.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/SocketsOps.h>

#include <boost/bind.hpp>

#include <errno.h>

using namespace muduo;
using namespace muduo::net;

const int Connector::kMaxRetryDelayMs;
/*
    客户端套接字文件描述符的包装类
负责具体的绑定，连接，等等工作；不负责收发信息的功能( 由TcpClient负责 )
    其对应的 Channel 则是复制这个文件描述符各个回掉函数的注册
    注意：TcpClient 不会保存客户端的套接字，因为一些网络的原因，创建了套接字不一定能成功
收发信息，需要加入重新试验的功能，有时需要删除刚刚创建的套接字，重新创建套接字。
    
*/
Connector::Connector(EventLoop* loop, const InetAddress& serverAddr)
  : loop_(loop),                         //事件循环，由 EventLoop 来调用该文件描述符的各个回掉函数
    serverAddr_(serverAddr),                     
    connect_(false),                    //connect_是否开始运行           
    state_(kDisconnected),				//套接字的连接状态
    retryDelayMs_(kInitRetryDelayMs)
{
  LOG_DEBUG << "ctor[" << this << "]";
}

Connector::~Connector()
{
  LOG_DEBUG << "dtor[" << this << "]";
  assert(!channel_);                    //对象的生存周期 Connector 应该先析构;其对应的 Channel 后析构 
}
/*
能够跨线程调用；
eg::将一个全局的 EventLoop 用于某个特定的 Client对象，在另外的线程中启动这个 Client
    下面的 startInLoop()
*/
void Connector::start()              
{
  connect_ = true;           // stat(...) 运行标志位
  loop_->runInLoop(boost::bind(&Connector::startInLoop, this)); // FIXME: unsafe
}

void Connector::startInLoop()
{
  loop_->assertInLoopThread();
  assert(state_ == kDisconnected);
  if (connect_)
  {
    connect();
  }
  else
  {
    LOG_DEBUG << "do not connect";
  }
}

void Connector::stop()
{
  connect_ = false;
  loop_->queueInLoop(boost::bind(&Connector::stopInLoop, this)); // FIXME: unsafe
  // FIXME: cancel timer
}

void Connector::stopInLoop()
{
  loop_->assertInLoopThread();
  if (state_ == kConnecting)
  {
    setState(kDisconnected);
    int sockfd = removeAndResetChannel();
    retry(sockfd);
  }
}
/*
   负责创建套接字，建立数据IO缓冲
*/
void Connector::connect()
{
  int sockfd = sockets::createNonblockingOrDie();        //注意 sockfd 是一个局部变量         
  int ret = sockets::connect(sockfd, serverAddr_.getSockAddrInet());
  int savedErrno = (ret == 0) ? 0 : errno;
  switch (savedErrno)
  {
    case 0:
    case EINPROGRESS:
    case EINTR:
    case EISCONN:         //连接成功
      connecting(sockfd); //连接，注意传递了 sockfd(没有在 Connector 中保存)
      break;

    case EAGAIN:
    case EADDRINUSE:
    case EADDRNOTAVAIL:
    case ECONNREFUSED:
    case ENETUNREACH:
      retry(sockfd);      //关闭套接字，重新连接，注意传递了 sockfd(没有在 Connector 中保存)
      break;

    case EACCES:
    case EPERM:
    case EAFNOSUPPORT:
    case EALREADY:
    case EBADF:
    case EFAULT:
    case ENOTSOCK:
      LOG_SYSERR << "connect error in Connector::startInLoop " << savedErrno;
      sockets::close(sockfd);
      break;

    default:
      LOG_SYSERR << "Unexpected error in Connector::startInLoop " << savedErrno;
      sockets::close(sockfd);
      // connectErrorCallback_();
      break;
  }
}
/*
主要是设置重新连接的事件间隔
*/
void Connector::restart()
{
  loop_->assertInLoopThread();
  setState(kDisconnected);
  retryDelayMs_ = kInitRetryDelayMs;
  connect_ = true;
  startInLoop();
}
/*
正在连接，设置套接字文件描述符的回掉函数，设置对应的 Channel 
关注了可写的事件后，有 Epoller 通知，回掉 handleWrite();
*/
void Connector::connecting(int sockfd)
{
  setState(kConnecting);
  assert(!channel_);
  channel_.reset(new Channel(loop_, sockfd));   //设置 Channel 
  channel_->setWriteCallback(
      boost::bind(&Connector::handleWrite, this)); // FIXME: unsafe
  channel_->setErrorCallback(
      boost::bind(&Connector::handleError, this)); // FIXME: unsafe

  // channel_->tie(shared_from_this()); is not working,
  // as channel_ is not managed by shared_ptr
  channel_->enableWriting();                    //  关注可写事件 因为套接字已经创建成功了，写缓冲现在  
}                                               //可写了 

int Connector::removeAndResetChannel()
{
  channel_->disableAll();                       //关闭 this channel_ 的所有监听事件
  channel_->remove();                           //从Epoller中移除 this channel_ 的 epoll_event
  int sockfd = channel_->fd();                  // this channel_ 对应的文件描述符 (套接字) 
  // Can't reset channel_ here, because we are inside Channel::handleEvent
  loop_->queueInLoop(boost::bind(&Connector::resetChannel, this)); // FIXME: unsafe
  return sockfd;
}

void Connector::resetChannel()
{
  channel_.reset();
}
/*
客户端套接字创建成功后，数据IO缓冲写缓冲可写，由 Epoller 通知该事件，进行调用；
在这里检查连接是否成功，并且做出相应的处理
*/
void Connector::handleWrite()
{
  LOG_TRACE << "Connector::handleWrite " << state_;

  if (state_ == kConnecting)
  {
    int sockfd = removeAndResetChannel();           // 从poller中移除关注，并将channel置空      
    int err = sockets::getSocketError(sockfd);      // socket可写并不意味着连接一定建立成功
    if (err)                                        // 还需要用getsockopt(...)再次确认一下。
    {                                               //  连接出错 
      LOG_WARN << "Connector::handleWrite - SO_ERROR = "
               << err << " " << strerror_tl(err);
      retry(sockfd);
    }
    else if (sockets::isSelfConnect(sockfd))        // 自身连接   
    {
      LOG_WARN << "Connector::handleWrite - Self connect";
      retry(sockfd);
    }
    else                                            //成功连接
    {
      setState(kConnected);                        //设置标志位
      if (connect_)
      {
        newConnectionCallback_(sockfd);            //调用连接回掉函数  
      }
      else
      {
        sockets::close(sockfd);
      }
    }
  }
  else
  {
    // what happened?
    assert(state_ == kDisconnected);
  }
}

void Connector::handleError()
{
  LOG_ERROR << "Connector::handleError state=" << state_;
  if (state_ == kConnecting)
  {
    int sockfd = removeAndResetChannel();
    int err = sockets::getSocketError(sockfd);
    LOG_TRACE << "SO_ERROR = " << err << " " << strerror_tl(err);
    retry(sockfd);
  }
}
/*
重新连接，关闭套接字后的重新连接。
通过connect_来控制是重新连接还是不重新连接
*/
void Connector::retry(int sockfd)
{
  sockets::close(sockfd);        //关闭创建的套接字文件描述符 (因为连接没有成功)
  setState(kDisconnected);       //设置连接关闭
  if (connect_)                  //需要重连
  {
    LOG_INFO << "Connector::retry - Retry connecting to " << serverAddr_.toIpPort()
             << " in " << retryDelayMs_ << " milliseconds. ";
    loop_->runAfter(retryDelayMs_/1000.0,
                    boost::bind(&Connector::startInLoop, shared_from_this())); //定时器 重新 start
    retryDelayMs_ = std::min(retryDelayMs_ * 2, kMaxRetryDelayMs);
  }
  else
  {
    LOG_DEBUG << "do not connect";
  }
}

