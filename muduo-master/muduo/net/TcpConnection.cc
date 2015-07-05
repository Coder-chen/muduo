// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/TcpConnection.h>

#include <muduo/base/Logging.h>
#include <muduo/base/WeakCallback.h>
#include <muduo/net/Channel.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/Socket.h>
#include <muduo/net/SocketsOps.h>

#include <boost/bind.hpp>

#include <errno.h>

using namespace muduo;
using namespace muduo::net;

void muduo::net::defaultConnectionCallback(const TcpConnectionPtr& conn)
{
  LOG_TRACE << conn->localAddress().toIpPort() << " -> "
            << conn->peerAddress().toIpPort() << " is "
            << (conn->connected() ? "UP" : "DOWN");
  // do not call conn->forceClose(), because some users want to register message callback only.
}

void muduo::net::defaultMessageCallback(const TcpConnectionPtr&,
                                        Buffer* buf,
                                        Timestamp)
{
  buf->retrieveAll();
}
/*
在服务端与客户端中负责消息的发送与接收
TcpConnection 运行于 IO线程中(在 TcpServer::newConnection() 中被创建)
*/
TcpConnection::TcpConnection(EventLoop* loop,
                             const string& nameArg,
                             int sockfd,
                             const InetAddress& localAddr,
                             const InetAddress& peerAddr)
  : loop_(CHECK_NOTNULL(loop)),                  //事件循环
    name_(nameArg),
    state_(kConnecting),                         //正在连接 
    socket_(new Socket(sockfd)),                 //套接字
    channel_(new Channel(loop, sockfd)),         //套接字对应的描述符 注册于 IO 线程中
    localAddr_(localAddr),
    peerAddr_(peerAddr),
    highWaterMark_(64*1024*1024)
{
  channel_->setReadCallback(
      boost::bind(&TcpConnection::handleRead, this, _1));
  channel_->setWriteCallback(
      boost::bind(&TcpConnection::handleWrite, this));
  channel_->setCloseCallback(
      boost::bind(&TcpConnection::handleClose, this));
  channel_->setErrorCallback(
      boost::bind(&TcpConnection::handleError, this));
  LOG_DEBUG << "TcpConnection::ctor[" <<  name_ << "] at " << this
            << " fd=" << sockfd;
  socket_->setKeepAlive(true);
}

TcpConnection::~TcpConnection()
{
  LOG_DEBUG << "TcpConnection::dtor[" <<  name_ << "] at " << this
            << " fd=" << channel_->fd()
            << " state=" << stateToString();
  assert(state_ == kDisconnected);           //析构时检查是否套接字被关闭了
}

/*
获得该套接字对应的各种信息
存储到 tcpi 中去
*/
bool TcpConnection::getTcpInfo(struct tcp_info* tcpi) const
{
  return socket_->getTcpInfo(tcpi);
}

/*
获得该套接字对应的各种信息
返回一个字符串
*/
string TcpConnection::getTcpInfoString() const
{
  char buf[1024];
  buf[0] = '\0';
  socket_->getTcpInfoString(buf, sizeof buf);
  return buf;
}

/*
发送消息
*/
void TcpConnection::send(const void* data, int len)
{
  send(StringPiece(static_cast<const char*>(data), len));
}

void TcpConnection::send(const StringPiece& message)
{
  if (state_ == kConnected)
  {
    if (loop_->isInLoopThread())
    {
      sendInLoop(message);
    }
    else
    {
      loop_->runInLoop(
          boost::bind(&TcpConnection::sendInLoop,
                      this,     // FIXME
                      message.as_string()));
                    //std::forward<string>(message)));
    }
  }
}

// FIXME efficiency!!!
void TcpConnection::send(Buffer* buf)
{
  if (state_ == kConnected)
  {
    if (loop_->isInLoopThread())
    {
      sendInLoop(buf->peek(), buf->readableBytes());
      buf->retrieveAll();
    }
    else
    {
      loop_->runInLoop(
          boost::bind(&TcpConnection::sendInLoop,
                      this,     // FIXME
                      buf->retrieveAllAsString()));
                    //std::forward<string>(message)));
    }
  }
}
/*
在事件循环中发送信息
*/
void TcpConnection::sendInLoop(const StringPiece& message)
{
  sendInLoop(message.data(), message.size());
}

void TcpConnection::sendInLoop(const void* data, size_t len)
{
  loop_->assertInLoopThread();
  ssize_t nwrote = 0;
  size_t remaining = len;
  bool faultError = false;
  if (state_ == kDisconnected)
  {
    LOG_WARN << "disconnected, give up writing";
    return;l
  }
  // if no thing in output queue, try writing directly
  if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
  {
    nwrote = sockets::write(channel_->fd(), data, len);
    if (nwrote >= 0)
    {
      remaining = len - nwrote;
      if (remaining == 0 && writeCompleteCallback_)
      {
        loop_->queueInLoop(boost::bind(writeCompleteCallback_, shared_from_this()));
      }
    }
    else // nwrote < 0
    {
      nwrote = 0;
      if (errno != EWOULDBLOCK)
      {
        LOG_SYSERR << "TcpConnection::sendInLoop";
        if (errno == EPIPE || errno == ECONNRESET) // FIXME: any others?
        {
          faultError = true;
        }
      }
    }
  }

  assert(remaining <= len);
  if (!faultError && remaining > 0)
  {
    size_t oldLen = outputBuffer_.readableBytes();
    if (oldLen + remaining >= highWaterMark_
        && oldLen < highWaterMark_
        && highWaterMarkCallback_)
    {
      loop_->queueInLoop(boost::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
    }
    outputBuffer_.append(static_cast<const char*>(data)+nwrote, remaining);
    if (!channel_->isWriting())
    {
      channel_->enableWriting();
    }
  }
}

void TcpConnection::shutdown()
{
  // FIXME: use compare and swap
  if (state_ == kConnected)
  {
    setState(kDisconnecting);
    // FIXME: shared_from_this()?
    loop_->runInLoop(boost::bind(&TcpConnection::shutdownInLoop, this));
  }
}

void TcpConnection::shutdownInLoop()
{
  loop_->assertInLoopThread();
  if (!channel_->isWriting())
  {
    // we are not writing
    socket_->shutdownWrite();
  }
}

// void TcpConnection::shutdownAndForceCloseAfter(double seconds)
// {
//   // FIXME: use compare and swap
//   if (state_ == kConnected)
//   {
//     setState(kDisconnecting);
//     loop_->runInLoop(boost::bind(&TcpConnection::shutdownAndForceCloseInLoop, this, seconds));
//   }
// }

// void TcpConnection::shutdownAndForceCloseInLoop(double seconds)
// {
//   loop_->assertInLoopThread();
//   if (!channel_->isWriting())
//   {
//     // we are not writing
//     socket_->shutdownWrite();
//   }
//   loop_->runAfter(
//       seconds,
//       makeWeakCallback(shared_from_this(),
//                        &TcpConnection::forceCloseInLoop));
// }

void TcpConnection::forceClose()
{
  // FIXME: use compare and swap
  if (state_ == kConnected || state_ == kDisconnecting)
  {
    setState(kDisconnecting);
    loop_->queueInLoop(boost::bind(&TcpConnection::forceCloseInLoop, shared_from_this()));
  }
}

void TcpConnection::forceCloseWithDelay(double seconds)
{
  if (state_ == kConnected || state_ == kDisconnecting)
  {
    setState(kDisconnecting);
    loop_->runAfter(
        seconds,
        makeWeakCallback(shared_from_this(),
                         &TcpConnection::forceClose));  // not forceCloseInLoop to avoid race condition
  }
}

void TcpConnection::forceCloseInLoop()
{
  loop_->assertInLoopThread();
  if (state_ == kConnected || state_ == kDisconnecting)
  {
    // as if we received 0 byte in handleRead();
    handleClose();
  }
}

const char* TcpConnection::stateToString() const
{
  switch (state_)
  {
    case kDisconnected:
      return "kDisconnected";
    case kConnecting:
      return "kConnecting";
    case kConnected:
      return "kConnected";
    case kDisconnecting:
      return "kDisconnecting";
    default:
      return "unknown state";
  }
}

void TcpConnection::setTcpNoDelay(bool on)
{
  socket_->setTcpNoDelay(on);
}
/*
连接建立后：(在IO线程中)
设置连接状态
注册文件描述符
关注文件描述符的事件
调用 TcpServer 中的设置的 connectionCallback 回掉函数
?TcpConnectiond的 EventLoop 是哪个呢?没有看到 EventLoop::Loop()
*/
void TcpConnection::connectEstablished()
{
  loop_->assertInLoopThread(); 
  assert(state_ == kConnecting);            
  setState(kConnected);                      //设置连接状态
  channel_->tie(shared_from_this());         //注册文件描述符 
  channel_->enableReading();                 //关注文件描述符的事件

  connectionCallback_(shared_from_this());   //hook 调用 TcpServer 中的设置的 connectionCallback 回掉函数
}

void TcpConnection::connectDestroyed()
{
  loop_->assertInLoopThread();
  if (state_ == kConnected)
  {
    setState(kDisconnected);
    channel_->disableAll();

    connectionCallback_(shared_from_this());
  }
  channel_->remove();
}
/*
  读回掉函数
  当读回掉读到的数据是 0 字节时，表示对方关闭了套接字的连接，此时调用 handleClose()
*/
void TcpConnection::handleRead(Timestamp receiveTime)
{
  loop_->assertInLoopThread();
  int savedErrno = 0;
  ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
  if (n > 0)
  {
    messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
  }
  else if (n == 0)
  {
    handleClose();
  }
  else
  {
    errno = savedErrno;
    LOG_SYSERR << "TcpConnection::handleRead";
    handleError();
  }
}
/*
写回掉函数
*/
void TcpConnection::handleWrite()
{
  loop_->assertInLoopThread();
  if (channel_->isWriting())
  {
    ssize_t n = sockets::write(channel_->fd(),
                               outputBuffer_.peek(),
                               outputBuffer_.readableBytes());
    if (n > 0)
    {
      outputBuffer_.retrieve(n);
      if (outputBuffer_.readableBytes() == 0)
      {
        channel_->disableWriting();
        if (writeCompleteCallback_)
        {
          loop_->queueInLoop(boost::bind(writeCompleteCallback_, shared_from_this()));
        }
        if (state_ == kDisconnecting)
        {
          shutdownInLoop();
        }
      }
    }
    else
    {
      LOG_SYSERR << "TcpConnection::handleWrite";
      // if (state_ == kDisconnecting)
      // {
      //   shutdownInLoop();
      // }
    }
  }
  else
  {
    LOG_TRACE << "Connection fd = " << channel_->fd()
              << " is down, no more writing";
  }
}
/*
关闭该 TcpConnection 套接字的连接
？为什么调用 connectionCallback_ ？
*/
void TcpConnection::handleClose()
{
  loop_->assertInLoopThread(); 
  LOG_TRACE << "fd = " << channel_->fd() << " state = " << stateToString();
  assert(state_ == kConnected || state_ == kDisconnecting);
  // we don't close fd, leave it to dtor, so we can find leaks easily.
  setState(kDisconnected);
  channel_->disableAll();         //不再监听这个 Channel 了

  TcpConnectionPtr guardThis(shared_from_this());
  connectionCallback_(guardThis);
  // must be the last line
  closeCallback_(guardThis);
}

void TcpConnection::handleError()
{
  int err = sockets::getSocketError(channel_->fd());
  LOG_ERROR << "TcpConnection::handleError [" << name_
            << "] - SO_ERROR = " << err << " " << strerror_tl(err);
}

