// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//

#include <muduo/net/TcpClient.h>

#include <muduo/base/Logging.h>
#include <muduo/net/Connector.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/SocketsOps.h>

#include <boost/bind.hpp>

#include <stdio.h>  // snprintf

using namespace muduo;
using namespace muduo::net;

// TcpClient::TcpClient(EventLoop* loop)
//   : loop_(loop)
// {
// }

// TcpClient::TcpClient(EventLoop* loop, const string& host, uint16_t port)
//   : loop_(CHECK_NOTNULL(loop)),
//     serverAddr_(host, port)
// {
// }

namespace muduo
{
namespace net
{
namespace detail
{

void removeConnection(EventLoop* loop, const TcpConnectionPtr& conn)
{
  loop->queueInLoop(boost::bind(&TcpConnection::connectDestroyed, conn));
}

void removeConnector(const ConnectorPtr& connector)
{
  //connector->
}

}
}
}
/*
   TcpClient 是Client的包装类；提供接口给用户
   
   具体的绑定地址，连接服务端 等等动作是由 connector (负者连接服务器，及处理连接中的异常等等)成员来完成的;
而 TcpClient 则是调用 connector的各个函数来完成特定的功能的，这只是提供给用户的一个接口；
   
   收发信息则是由套接字文件描述符注册的相应的回掉函数来进行的；
  
  何时收发信息则是由 Epoll 事件通知

与之类似:
TcpServer 是Server的包装类；提供接口给用户
*/
TcpClient::TcpClient(EventLoop* loop,
                     const InetAddress& serverAddr,
                     const string& nameArg)
  : loop_(CHECK_NOTNULL(loop)),                     //某个线程中的唯一 EventLoop 其负责调用各个回掉函数
    connector_(new Connector(loop, serverAddr)),    //具体的连接动作执行类 ( 客户端套接字的包装 )  
    name_(nameArg),                                 //给TcpClient对象的名称 区别多个线程中的多个对象           
    connectionCallback_(defaultConnectionCallback), //   默认的回掉函数 
    messageCallback_(defaultMessageCallback),       //只是声明了函数，但是没有实现该函数(初始化时占位)
    retry_(false),
    connect_(true),
    nextConnId_(1)
{
  connector_->setNewConnectionCallback(                  //设置客户端套接字文件描述符的回掉函数 
      boost::bind(&TcpClient::newConnection, this, _1)); // TcpClient::newConnection(int sockfd)
  // FIXME setConnectFailedCallback                      //boost::bind(...) 来生成一个函数对象
  LOG_INFO << "TcpClient::TcpClient[" << name_        
           << "] - connector " << get_pointer(connector_);
}

TcpClient::~TcpClient()
{
  LOG_INFO << "TcpClient::~TcpClient[" << name_
           << "] - connector " << get_pointer(connector_);
  TcpConnectionPtr conn;
  bool unique = false;
  {
    MutexLockGuard lock(mutex_);
    unique = connection_.unique();
    conn = connection_;
  }
  if (conn)
  {
    assert(loop_ == conn->getLoop());
    // FIXME: not 100% safe, if we are in different thread
    CloseCallback cb = boost::bind(&detail::removeConnection, loop_, _1);
    loop_->runInLoop(
        boost::bind(&TcpConnection::setCloseCallback, conn, cb));
    if (unique)
    {
      conn->forceClose();
    }
  }
  else
  {
    connector_->stop();
    // FIXME: HACK
    loop_->runAfter(1, boost::bind(&detail::removeConnector, connector_));
  }
}
/*
连接服务端
将连接任务委派给 connector_ 成员
*/
void TcpClient::connect()
{
  // FIXME: check state
  LOG_INFO << "TcpClient::connect[" << name_ << "] - connecting to "
           << connector_->serverAddress().toIpPort();
  connect_ = true;
  connector_->start();      
}
/*
断开与服务端的连接
*/
void TcpClient::disconnect()
{
  connect_ = false;

  {
    MutexLockGuard lock(mutex_);
    if (connection_)
    {
      connection_->shutdown();
    }
  }
}

void TcpClient::stop()
{
  connect_ = false;
  connector_->stop();
}
/*
当 connector_ 建立了连接后(Connector::handleWrite(...) )会马上调用这个函数
并且传入 sockfd 
*/
void TcpClient::newConnection(int sockfd)
{
  loop_->assertInLoopThread();                        //不能跨线程调用 不能在线程A中去指示线程B中的 Client 发起连接
  InetAddress peerAddr(sockets::getPeerAddr(sockfd));
  char buf[32];
  snprintf(buf, sizeof buf, ":%s#%d", peerAddr.toIpPort().c_str(), nextConnId_);
  ++nextConnId_;
  string connName = name_ + buf;

  InetAddress localAddr(sockets::getLocalAddr(sockfd));
  // FIXME poll with zero timeout to double confirm the new connection
  // FIXME use make_shared if necessary
  TcpConnectionPtr conn(new TcpConnection(loop_,        //创建一个 TcpConnection 负责信息的收发
                                          connName,
                                          sockfd,
                                          localAddr,
                                          peerAddr));

  conn->setConnectionCallback(connectionCallback_);     //设置回掉函数
  conn->setMessageCallback(messageCallback_);
  conn->setWriteCompleteCallback(writeCompleteCallback_);
  conn->setCloseCallback(
      boost::bind(&TcpClient::removeConnection, this, _1)); // FIXME: unsafe
  {
    MutexLockGuard lock(mutex_);
    connection_ = conn;
  }
  conn->connectEstablished();
}

void TcpClient::removeConnection(const TcpConnectionPtr& conn)
{
  loop_->assertInLoopThread();                 // 不能跨线程调用
  assert(loop_ == conn->getLoop());            // 在连接线程中

  {
    MutexLockGuard lock(mutex_);
    assert(connection_ == conn);
    connection_.reset();
  }

  loop_->queueInLoop(boost::bind(&TcpConnection::connectDestroyed, conn));
  if (retry_ && connect_)
  {
    LOG_INFO << "TcpClient::connect[" << name_ << "] - Reconnecting to "
             << connector_->serverAddress().toIpPort();
    connector_->restart();
  }
}

