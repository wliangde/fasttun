#include "FasttunBase.h"
#include "Listener.h"
#include "Connection.h"
#include "KcpTunnel.h"
#include "FastConnection.h"
#include "Cache.h"

using namespace tun;

typedef KcpTunnelGroup<true> MyTunnelGroup;
static MyTunnelGroup *gTunnelManager = NULL;

static sockaddr_in ListenAddr;
static sockaddr_in ConnectAddr;

//--------------------------------------------------------------------------
class ServerBridge : public Connection::Handler, public FastConnection::Handler
{
  public:
	struct Handler
	{
		virtual void onExtConnDisconnected(ServerBridge *pBridge) = 0;
		virtual void onExtConnError(ServerBridge *pBridge) = 0;
	};
	
    ServerBridge(EventPoller *poller, Handler *h)
			:mEventPoller(poller)
			,mpHandler(h)
			,mpIntConn(NULL)
			,mpExtConn(NULL)
			,mCache(NULL)
			,mLastExtConnTime(0)
	{
		mCache = new MyCache(this, &ServerBridge::flush);
	}
	
    virtual ~ServerBridge()
	{
		delete mCache;
	}

	bool acceptConnection(int connfd)
	{
		assert(NULL == mpIntConn && NULL == mpExtConn &&
			   "NULL == mpIntConn && NULL == mpExtConn");

		mpExtConn = new FastConnection(mEventPoller, gTunnelManager);
		if (!mpExtConn->acceptConnection(connfd))
		{
			delete mpExtConn;
			mpExtConn = NULL;
			return false;
		}
		mpExtConn->setEventHandler(this);

		mLastExtConnTime = core::coreClock();
		mpIntConn = new Connection(mEventPoller);
		if (!mpIntConn->connect((const SA *)&ConnectAddr, sizeof(ConnectAddr)))
		{
			mpExtConn->shutdown();
			delete mpExtConn;
			mpExtConn = NULL;
			delete mpIntConn;
			mpIntConn = NULL;
			return false;
		}
		mpIntConn->setEventHandler(this);

		return true;
	}

	void shutdown()
	{
		if (mpExtConn)
		{
			mpExtConn->shutdown();
			delete mpExtConn;
			mpExtConn = NULL;
		}
		if (mpIntConn)
		{
			mpIntConn->shutdown();
			delete mpIntConn;
			mpIntConn = NULL;
		}
	}	

	FastConnection* getExtConn() const
	{
		return mpExtConn;
	}

	// Connection::Handler
	virtual void onConnected(Connection *pConn)
	{
		_flushAll();
		logInfoLn("connected to a internal connection!");
	}
	virtual void onDisconnected(Connection *pConn)
	{
		_reconnectInternal();
		logInfoLn("disconnected with a internal connection!");
	}

	virtual void onRecv(Connection *pConn, const void *data, size_t datalen)
	{
		mpExtConn->send(data, datalen);
		logInfoLn("internal recvlen="<<datalen);
	}
	
	virtual void onError(Connection *pConn)
	{
		_reconnectInternal();
		logInfoLn("occur an error at an internal connection! reason:"<<coreStrError());
	}

	// FastConnection::Handler
	virtual void onDisconnected(FastConnection *pConn)
	{
		if (mpHandler)
			mpHandler->onExtConnDisconnected(this);
	}
	virtual void onError(FastConnection *pConn)
	{
		if (mpHandler)
			mpHandler->onExtConnError(this);
	}
		
	virtual void onCreateKcpTunnelFailed(FastConnection *pConn)
	{
		if (mpHandler)
			mpHandler->onExtConnError(this);
	}

	virtual void onRecv(FastConnection *pConn, const void *data, size_t datalen)
	{
		if (!mpIntConn->isConnected())
		{
			_reconnectInternal();
			mCache->cache(data, datalen);
		}
		else
		{
			_flushAll();
			mpIntConn->send(data, datalen);
		}
	}

	void _reconnectInternal()
	{
		ulong curtick = core::coreClock();
		if (curtick > mLastExtConnTime+1000)
		{
			mLastExtConnTime = curtick;			
			mpIntConn->connect((const SA *)&ConnectAddr, sizeof(ConnectAddr));
		}		
	}

	void _flushAll()
	{
		if (mpIntConn->isConnected())
		{
			mCache->flushAll();
		}
	}
	void flush(const void *data, size_t len)
	{
		mpIntConn->send(data, len);
	}
	
  private:	
	typedef Cache<ServerBridge> MyCache;
	
	EventPoller *mEventPoller;
	Handler *mpHandler;

	Connection *mpIntConn;
	FastConnection *mpExtConn;

	MyCache *mCache;

	ulong mLastExtConnTime;
};
//--------------------------------------------------------------------------

//--------------------------------------------------------------------------
class Server : public Listener::Handler, public ServerBridge::Handler
{
  public:
    Server(EventPoller *poller)
			:Listener::Handler()
			,mEventPoller(poller)
			,mListener(poller)
			,mBridges()
	{
	}
	
    virtual ~Server()
	{
	}

	bool create(const SA *sa, socklen_t salen)
	{
		if (!mListener.create(sa, salen))
		{
			logErrorLn("create listener failed.");
			return false;
		}
		mListener.setEventHandler(this);

		return true;
	}

	void finalise()
	{		
		mListener.finalise();
		BridgeList::iterator it = mBridges.begin();
		for (; it != mBridges.end(); ++it)
		{
			ServerBridge *bridge = *it;
			if (bridge)
			{
				bridge->shutdown();
				delete bridge;
			}
		}
		mBridges.clear();
	}

	virtual void onAccept(int connfd)
	{
		ServerBridge *bridge = new ServerBridge(mEventPoller, this);
		if (!bridge->acceptConnection(connfd))
		{
			delete bridge;
			return;
		}

		mBridges.insert(bridge);
		logInfoLn("a fast connection createted! cursize:"<<mBridges.size());
	}

	virtual void onExtConnDisconnected(ServerBridge *pBridge)
	{		
		onBridgeShut(pBridge);
		logInfoLn("a fast connection closed! cursize:"<<mBridges.size());
	}
	
	virtual void onExtConnError(ServerBridge *pBridge)
	{		
		onBridgeShut(pBridge);
		logInfoLn("a fast connection occur error! cursize:"<<mBridges.size()<<" reason:"<<coreStrError());
	}
	
  private:
	void onBridgeShut(ServerBridge *pBridge)
	{		
		BridgeList::iterator it = mBridges.find(pBridge);
		if (it != mBridges.end())
		{			
			mBridges.erase(it);
		}

		pBridge->shutdown();
		delete pBridge;
	}
	
  private:
	typedef std::set<ServerBridge *> BridgeList;
	
	EventPoller *mEventPoller;
	Listener mListener;

	BridgeList mBridges;
};
//--------------------------------------------------------------------------

int main(int argc, char *argv[])
{
	// initialise tracer
	core::createTrace();
	core::output2Console();
	core::output2Html("fasttun_server.html");

	// parse parameter
	const char *confPath = NULL;
	const char *listenAddr = NULL;
	const char *connectAddr = NULL;
	
	int opt = 0;
	while ((opt = getopt(argc, argv, "c:l:r:")) != -1)
	{
		switch (opt)
		{
		case 'c':
			confPath = optarg;
			break;
		case 'l':
			listenAddr = optarg;
			break;
		case 'r':
			connectAddr = optarg;
			break;
		default:
			break;
		}
	}	
	
	if (argc == 1)
	{
		confPath = DEFAULT_CONF_PATH;
	}
	if (confPath)
	{
		Ini ini(confPath);
		static std::string s_listenAddr = ini.getString("server", "listen", "");
		static std::string s_connectAddr = ini.getString("server", "connect", "");
		if (s_listenAddr != "")
			listenAddr = s_listenAddr.c_str();
		if (s_connectAddr != "")
			connectAddr = s_connectAddr.c_str();
	}

	if (NULL == listenAddr || NULL == connectAddr)
	{
		fprintf(stderr, "no argument assigned or parse argument failed!\n");
		core::closeTrace();
		exit(EXIT_FAILURE);
	}
	if (!core::str2Ipv4(listenAddr, ListenAddr) || !core::str2Ipv4(connectAddr, ConnectAddr))
	{
		logErrorLn("invalid socket address!");
		core::closeTrace();
		exit(EXIT_FAILURE);
	}

	// create event poller
	EventPoller *netPoller = EventPoller::create();

	// create server
	Server svr(netPoller);
	if (!svr.create((const SA *)&ListenAddr, sizeof(ListenAddr)))
	{
		logErrorLn("create server error!");
		delete netPoller;
		core::closeTrace();
		exit(EXIT_FAILURE);
	}	

	gTunnelManager = new MyTunnelGroup(netPoller);
	if (!gTunnelManager->create((const SA *)&ListenAddr, sizeof(ListenAddr)))
	{
		logErrorLn("initialise Tunnel Manager error!");
		delete netPoller;
		core::closeTrace();
		exit(EXIT_FAILURE);
	}	

	double maxWait = 0;
	for (;;)
	{
		netPoller->processPendingEvents(maxWait);
		maxWait = gTunnelManager->update();
		maxWait *= 0.001f;
	}
	
	gTunnelManager->shutdown();
	delete gTunnelManager;
	svr.finalise();
	delete netPoller;

	// close tracer
	core::closeTrace();
	exit(0);
}
