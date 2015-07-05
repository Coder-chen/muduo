// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/TcpServer.h>

#include <muduo/base/Logging.h>
#include <muduo/net/Acceptor.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThreadPool.h>
#include <muduo/net/SocketsOps.h>

#include <boost/bind.hpp>

#include <stdio.h>  // snprintf

using namespace muduo;
using namespace muduo::net;

/*
TcpServer 服务端，管理服务端套接字的监听(Acceptor);
                  以及连接服务端后的 Client 与 Server 的连接流(TcpConnection)
*/
TcpServer::TcpServer(EventLoop* loop,                  //事件循环 复制监听 Server 端的套接字的连接信息
                     const InetAddress& listenAddr,    //Server 端地址
                     const string& nameArg,            //Server 端名字
                     Option option)
  : loop_(CHECK_NOTNULL(loop)),                        //检测是否注册了事件循环( EventLoop )
    hostport_(listenAddr.toIpPort()),                  //Server 端的 Ip + Port (string)  
    name_(nameArg),                                    //Server 端名字
    acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)), /* acceptor_ 负责 Server 端套接字的连接
	                                                                 使用的主线程中的事件循环(传入的是主线程的 EventLoop*) */
    threadPool_(new EventLoopThreadPool(loop, name_)),  //线程池，当有一个 new Client 时分配一个线程 
    connectionCallback_(defaultConnectionCallback),     /*有一个 Client 连接进来时的默认回掉函数 (此时)
	                                                    (此时 accept 对应的 Channel 有 read事件发生，主线程 EventLoop::loop() 会调用该函数 )*/ 
    messageCallback_(defaultMessageCallback),
    nextConnId_(1)                                     //下一个 Client 连接的序号
{
  acceptor_->setNewConnectionCallback(
      boost::bind(&TcpServer::newConnection, this, _1, _2));
}

TcpServer::~TcpServer()
{
  loop_->assertInLoopThread();
  LOG_TRACE << "TcpServer::~TcpServer [" << name_ << "] destructing";

  for (ConnectionMap::iterator it(connections_.begin());
      it != connections_.end(); ++it)
  {
    TcpConnectionPtr conn = it->second;
    it->second.reset();
    conn->getLoop()->runInLoop(
      boost::bind(&TcpConnection::connectDestroyed, conn));
    conn.reset();
  }
}

void TcpServer::setThreadNum(int numThreads)
{
  assert(0 <= numThreads);
  threadPool_->setThreadNum(numThreads);
}
/*
TcpServer开始运行
*/
void TcpServer::start()
{
  if (started_.getAndSet(1) == 0)    /* AtomicInt32 started_ (原子计数) 多个线程中只能调用一次(在主线程中才能调用)
                                      其它线程中调用不起作用*/
  {  
    threadPool_->start(threadInitCallback_);       //线程池开始运行

    assert(!acceptor_->listenning());   
    loop_->runInLoop(                //在 TcpSer ( Accept )事件循环中注册待调用的函数( 监听套接字 ) 此函数会唤醒 EventLoop::loop() 
        boost::bind(&Acceptor::listen, get_pointer(acceptor_))); //Acceptor开始监听
  }
}
/*
有一个 Client 连接进来时 TcpServer 所在的线程(主线程) 中的 EventLoop::loop() 会自动的调用该函数
( acceptor_始终与 TcpServer 对象在同一个线程(主线程) )

参数 sockfd 是怎么样传入进来的呢？
TcpServer 中 EventLoop::loop() 监听到 Accept(acceptor_) 有读的事件时，回掉其 Accept::handlRead(...),
进而掉用 Accept::NewConnectionCallback ；这是一个函数指针被设置成了 TcpServer::newConnection()。
这种调用 从 top 到 bottom ；然后从 bottom 在到 top 的方式，相当于从 top 往 bottom 升入了一个 hook，
正是这个 hook 体现了责任的分离原则，TcpServer 管理连接等等一切事项( 有连接后的一系列配置选项 ),
Acceptor 负责连接这一动作(事件)

调用此函数后，从线程池中分派一个 EventLoop ( ioLoop ),该 EventLoop 正在 loop(),通过 runInLoop()
将一个新的 TcpConnetion 注册到该 ioLoop 中，自此新连接( TcpConnection )开始与 Client 通信
*/
void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr)
{
  loop_->assertInLoopThread();      /*断言 :: 是否在(主线程) EventLoop::Loop()中被调用
                                     原因：主线程的事件循环一直在无限 loop(), 任何函数的调用只能在
									 loop()中被调用；
                                     当另外一个线程主动调用这个函数时，相当于一个线程在某一时刻同时执行两个函数了
                                     这肯定是不行的*/
  EventLoop* ioLoop = threadPool_->getNextLoop(); /*从主线程的线程池中 分配一个专门用于 数据I/O 的线程(子线程)
                                                    这样主线程的 loop()只负责接收连接，在接受其它 Client 的连接时，
                                                    同一时间还可以和 Client 通信。(两者位于不同的线程中)													
												  */  
  char buf[32];                                  //Server端地址 + Client连接的序号
  snprintf(buf, sizeof buf, ":%s#%d", hostport_.c_str(), nextConnId_);  
  ++nextConnId_;
  string connName = name_ + buf;

  LOG_INFO << "TcpServer::newConnection [" << name_
           << "] - new connection [" << connName
           << "] from " << peerAddr.toIpPort();
  InetAddress localAddr(sockets::getLocalAddr(sockfd));
  // FIXME poll with zero timeout to double confirm the new connection
  // FIXME use make_shared if necessary
  TcpConnectionPtr conn(new TcpConnection(ioLoop,          //TcpConnection 运行的 EventLoop (子线程中运行) 
                                          connName,        //Server端地址 + Client连接的序号
                                          sockfd,          //本机的套接字(由acceptor_提供)
                                          localAddr,       //本机的地址
                                          peerAddr));      //对方的地址 
  connections_[connName] = conn;
  conn->setConnectionCallback(connectionCallback_);        //在主线程中设置(配置)子线程的回掉函数
  conn->setMessageCallback(messageCallback_);
  conn->setWriteCompleteCallback(writeCompleteCallback_);
  conn->setCloseCallback(
      boost::bind(&TcpServer::removeConnection, this, _1)); // FIXME: unsafe
  ioLoop->runInLoop(boost::bind(&TcpConnection::connectEstablished, conn)); /*还是在主线程中设置(配置)子线程的回掉函数
                                                                                io线程创建的时候已经在 EventLoop::loop()了，这里
																				一定要用 runInLoop()
                                                                               runInLoop()::在事件循环中调用，而非现在立即调用*/
  }

void TcpServer::removeConnection(const TcpConnectionPtr& conn)
{
  // FIXME: unsafe
  loop_->runInLoop(boost::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
  loop_->assertInLoopThread();
  LOG_INFO << "TcpServer::removeConnectionInLoop [" << name_
           << "] - connection " << conn->name();
  size_t n = connections_.erase(conn->name());
  (void)n;
  assert(n == 1);
  EventLoop* ioLoop = conn->getLoop();
  ioLoop->queueInLoop(
      boost::bind(&TcpConnection::connectDestroyed, conn));
}

