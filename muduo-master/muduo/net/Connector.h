// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_CONNECTOR_H
#define MUDUO_NET_CONNECTOR_H

#include <muduo/net/InetAddress.h>

#include <boost/enable_shared_from_this.hpp>
#include <boost/function.hpp>
#include <boost/noncopyable.hpp>
#include <boost/scoped_ptr.hpp>

namespace muduo
{
namespace net
{

class Channel;
class EventLoop;

class Connector : boost::noncopyable,
                  public boost::enable_shared_from_this<Connector>
{
 public:
  typedef boost::function<void (int sockfd)> NewConnectionCallback;

  Connector(EventLoop* loop, const InetAddress& serverAddr);
  ~Connector();

  void setNewConnectionCallback(const NewConnectionCallback& cb)  //客户端连接后 调用的回掉函数
  { newConnectionCallback_ = cb; }

  void start();  // can be called in any thread
  void restart();  // must be called in loop thread
  void stop();  // can be called in any thread

  const InetAddress& serverAddress() const { return serverAddr_; }

 private:
  enum States { kDisconnected, kConnecting, kConnected }; //连接断开，正在连接，已经连接
  static const int kMaxRetryDelayMs = 30*1000;
  static const int kInitRetryDelayMs = 500;

  void setState(States s) { state_ = s; }
  void startInLoop();
  void stopInLoop();
  void connect();
  void connecting(int sockfd);
  void handleWrite();                                     
  void handleError();
  void retry(int sockfd);
  int removeAndResetChannel();
  void resetChannel();

  EventLoop* loop_;                           //事件循环
  InetAddress serverAddr_;                    //服务端地址
  bool connect_; // atomic
  States state_;  // FIXME: use atomic variable
  boost::scoped_ptr<Channel> channel_;        //套接字对应的Channel 只有在连接成功，创建了套接字时才被设置
  NewConnectionCallback newConnectionCallback_;//连接回掉函数 boost::function<void (int sockfd)>
  int retryDelayMs_;
};

}
}

#endif  // MUDUO_NET_CONNECTOR_H
