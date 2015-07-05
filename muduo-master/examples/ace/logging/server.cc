#include <examples/ace/logging/logrecord.pb.h>

#include <muduo/base/Atomic.h>
#include <muduo/base/FileUtil.h>
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpServer.h>
#include <muduo/net/protobuf/ProtobufCodecLite.h>

#include <boost/bind.hpp>

#include <stdio.h>

using namespace muduo;
using namespace muduo::net;

namespace logging
{
extern const char logtag[] = "LOG0";
typedef ProtobufCodecLiteT<LogRecord, logtag> Codec;

class Session : boost::noncopyable
{
 public:
  explicit Session(const TcpConnectionPtr& conn)
    : codec_(boost::bind(&Session::onMessage, this, _1, _2, _3)),
      file_(getFileName(conn))
  {
    conn->setMessageCallback(
        boost::bind(&Codec::onMessage, &codec_, _1, _2, _3));
  }

 private:

  // FIXME: duplicate code LogFile
  // or use LogFile instead
  string getFileName(const TcpConnectionPtr& conn)
  {
    string filename;
    filename += conn->peerAddress().toIp();

    char timebuf[32];
    struct tm tm;
    time_t now = time(NULL);
    gmtime_r(&now, &tm); // FIXME: localtime_r ?
    strftime(timebuf, sizeof timebuf, ".%Y%m%d-%H%M%S.", &tm);
    filename += timebuf;

    char buf[32];
    snprintf(buf, sizeof buf, "%d", globalCount_.incrementAndGet());
    filename += buf;

    filename += ".log";
    LOG_INFO << "Session of " << conn->name() << " file " << filename;
    return filename;
  }

  void onMessage(const TcpConnectionPtr& conn,
                 const MessagePtr& message,
                 Timestamp time)
  {
    LogRecord* logRecord = muduo::down_cast<LogRecord*>(message.get());
    if (logRecord->has_heartbeat())
    {
      // FIXME ?
    }
    const char* sep = "==========\n";
    std::string str = logRecord->DebugString();
    file_.append(str.c_str(), str.size());
    file_.append(sep, strlen(sep));
    LOG_DEBUG << str;
  }

  Codec codec_;
  FileUtil::AppendFile file_;
  static AtomicInt32 globalCount_;
};
typedef boost::shared_ptr<Session> SessionPtr;

AtomicInt32 Session::globalCount_;

class LogServer : boost::noncopyable
{
 public:
  LogServer(EventLoop* loop, const InetAddress& listenAddr, int numThreads)
    : loop_(loop),
      server_(loop_, listenAddr, "AceLoggingServer")
  {
    server_.setConnectionCallback(                             //设置连接回掉函数
        boost::bind(&LogServer::onConnection, this, _1));
    if (numThreads > 1)                                        //设置IO线程数目
    {
      server_.setThreadNum(numThreads);
    }
  }

  void start()
  {
    server_.start();
  }

 private:
  void onConnection(const TcpConnectionPtr& conn)
  {
    if (conn->connected())
    {
      SessionPtr session(new Session(conn));
      conn->setContext(session);
    }
    else
    {
      conn->setContext(SessionPtr());
    }
  }

  EventLoop* loop_;                   //事件循环指针   
  TcpServer server_;                  //TcpServer 
};

}

int main(int argc, char* argv[])
{
  EventLoop loop;                                           //事件循环(与 TcpServer 同在一个线程中 主线程)
  int port = argc > 1 ? atoi(argv[1]) : 50000;              //端口号
  LOG_INFO << "Listen on port " << port;
  InetAddress listenAddr(static_cast<uint16_t>(port));      //Server 端地址
  int numThreads = argc > 2 ? atoi(argv[2]) : 1;            //Server 端的线程数目
  logging::LogServer server(&loop, listenAddr, numThreads); //LogServer 的构造
  server.start();                                           //Server 开始运行   
  loop.loop();                                              //事件循环开始运行 

}
