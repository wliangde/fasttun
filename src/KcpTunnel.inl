NAMESPACE_BEG(tun)

//--------------------------------------------------------------------------
static int kcpOutput(const char *buf, int len, ikcpcb *kcp, void *user)
{
	ITunnel *pTunnel = (ITunnel *)user;
	if (pTunnel)
	{
		assert(pTunnel->_output(buf, len) == len && "kcp outputed len illegal");
		
		ikcpcb *kcp = pTunnel->_debugGetKcpCb();
		logInfoLn("output conv="<<pTunnel->getConv()<<" len="<<len<<
				  " snd_nxt"<<kcp->snd_nxt<<" rcv_nxt="<<kcp->rcv_nxt);		
	}
	return 0;
}

template <bool IsServer>
KcpTunnel<IsServer>::~KcpTunnel()
{
	shutdown();
	delete mSndCache;
}

template <bool IsServer>
bool KcpTunnel<IsServer>::create(uint32 conv, const KcpArg &arg)
{
	if (mKcpCb)
		shutdown();

	mConv = conv;
	mKcpCb = ikcp_create(mConv, this);
	if (NULL == mKcpCb)
		return false;

	mKcpCb->output = kcpOutput;
	ikcp_nodelay(mKcpCb, arg.nodelay, arg.interval, arg.resend, arg.nc);
	ikcp_setmtu(mKcpCb, arg.mtu);
	mSentCount = mRecvCount = 0;
	logInfoLn("create kcp! conv="<<conv);
	return true;
}

template <bool IsServer>
void KcpTunnel<IsServer>::shutdown()
{	
	if (mKcpCb)
	{
		int nsnd_que = mKcpCb->nsnd_que;
		logInfoLn("close kcp! conv="<<mConv<<
				  " sentcount="<<mSentCount<<" recvcount="<<mRecvCount<<
				  " snd_nxt"<<mKcpCb->snd_nxt<<" rcv_nxt="<<mKcpCb->rcv_nxt<<
				  " peeksize="<<ikcp_peeksize(mKcpCb)<<
				  " nrcv_que="<<mKcpCb->nrcv_que<<" nsnd_que="<<mKcpCb->nsnd_que<<
				  " snd_wnd="<<mKcpCb->snd_wnd<<" rmt_wnd"<<mKcpCb->rmt_wnd<<
				  " snd_una="<<mKcpCb->snd_una<<" cwnd="<<mKcpCb->cwnd);

		ikcp_update(mKcpCb, core::coreClock());
		
		struct IQUEUEHEAD *p;
		IKCPSEG *seg;
		int length = 0;
		ikcpcb *kcp = mKcpCb;

		if (!iqueue_is_empty(&kcp->rcv_queue))
		{
			seg = iqueue_entry(kcp->rcv_queue.next, IKCPSEG, node);
			if (seg->frg == 0)
			{
				logInfoLn("kcp conv="<<mConv<<" frg=0");
			}
			else if (kcp->nrcv_que < seg->frg + 1)
			{
				logInfoLn("kcp conv="<<mConv<<" nrcv_que="<<kcp->nrcv_que<<" frg="<<(seg->frg+1));
			}
		}	   		
		
		ikcp_release(mKcpCb);		
		mKcpCb = NULL;
	}
	mSentCount = mRecvCount = 0;
}

template <bool IsServer>
int KcpTunnel<IsServer>::send(const void *data, size_t datalen)
{
	if (this->_canFlush())
	{
		_flushAll();
		flushSndBuf(data, datalen);
	}
	else
	{
		mSndCache->cache(data, datalen);
	}	
	return 0;
}

template <bool IsServer>
void KcpTunnel<IsServer>::_flushAll()
{
	if (this->_canFlush())
	{
		mSndCache->flushAll();
	}
}

template <bool IsServer>
bool KcpTunnel<IsServer>::_canFlush() const
{
	return ikcp_waitsnd(mKcpCb) < 2*mKcpCb->snd_wnd;
}

template <bool IsServer>
void KcpTunnel<IsServer>::flushSndBuf(const void *data, size_t datalen)
{
	const char *ptr = (const char *)data;
	size_t maxLen = mKcpCb->mss<<2;
	for (;;)
	{
		if (datalen <= maxLen) // in most case
		{
			ikcp_send(mKcpCb, (const char *)ptr, datalen);
			++mSentCount;
			break;
		}
		else
		{
			ikcp_send(mKcpCb, (const char *)ptr, maxLen);
			++mSentCount;
			ptr += maxLen;
			datalen -= maxLen;
		}
	}
}

template <bool IsServer>
bool KcpTunnel<IsServer>::input(const void *data, size_t datalen)
{
	int ret = ikcp_input(mKcpCb, (const char *)data, datalen);
	return 0 == ret;
}

template <bool IsServer>
uint32 KcpTunnel<IsServer>::update(uint32 current)
{
	ikcp_update(mKcpCb, current);
	_flushAll();

	int datalen = ikcp_peeksize(mKcpCb);
	if (datalen > 0)
	{
		char *buf = (char *)malloc(datalen);
		assert(buf != NULL && "ikcp_recv() malloc failed!");
		assert(ikcp_recv(mKcpCb, buf, datalen) == datalen);

		++mRecvCount;
		logInfoLn("kcp recv, conv="<<mConv<<" len="<<datalen<<
				  " recvcount="<<mRecvCount);
		if (mHandler)
			mHandler->onRecv(buf, datalen);
		free(buf);
	}

	uint32 nextCallTime = ikcp_check(mKcpCb, current);
	return nextCallTime > current ? nextCallTime - current : 0;
}
//--------------------------------------------------------------------------

//--------------------------------------------------------------------------
template <bool IsServer>
KcpTunnelGroup<IsServer>::~KcpTunnelGroup()
{
}

template <bool IsServer>
bool KcpTunnelGroup<IsServer>::create(const char *addr)
{
	if (!Supper::create(addr))
	{
		return false;
	}   	

	return this->_create();
}

template <bool IsServer>
bool KcpTunnelGroup<IsServer>::create(const SA* sa, socklen_t salen)
{
	if (!Supper::create(sa, salen))
	{
		return false;
	}

	return this->_create();
}

template <bool IsServer>
bool KcpTunnelGroup<IsServer>::_create()
{
	// register for event
	if (!mEventPoller->registerForRead(this->mFd, this))
	{
		logErrorLn("KcpTunnelGroup::create() register error!");
		return false;
	}
	return true;
}

template <bool IsServer>
void KcpTunnelGroup<IsServer>::shutdown()
{
	typename Tunnels::iterator it = this->mTunnels.begin();
	for (; it != this->mTunnels.end(); ++it)
	{
		Tun *pTunnel = it->second;
		if (pTunnel)
		{
			pTunnel->shutdown();
			delete pTunnel;
		}
	}
	this->mTunnels.clear();
	
	if (this->mFd >= 0)
	{
		mEventPoller->deregisterForRead(this->mFd);
		close(this->mFd);
		this->mFd = -1;
	}
}

template <bool IsServer>
ITunnel* KcpTunnelGroup<IsServer>::createTunnel(uint32 conv)
{
	typename Tunnels::iterator it = this->mTunnels.find(conv);
	if (it != this->mTunnels.end())
	{
		logErrorLn("KcpTunnelGroup::createTunnel() tunnul already exist! conv="<<conv);
		return NULL;
	}
	
	Tun *pTunnel = new Tun(this);
	if (!pTunnel->create(conv, mKcpArg))
	{
		return NULL;
	}

	this->mTunnels.insert(std::pair<uint32, Tun *>(conv, pTunnel));
	return pTunnel;
}

template <bool IsServer>
void KcpTunnelGroup<IsServer>::destroyTunnel(ITunnel *pTunnel)
{	
	uint32 conv = pTunnel->getConv();
	
	typename Tunnels::iterator it = this->mTunnels.find(conv);
	if (it != this->mTunnels.end())
	{
		this->mTunnels.erase(it);
	}

	static_cast<Tun *>(pTunnel)->shutdown();
	delete pTunnel;
}

template <bool IsServer>
uint32 KcpTunnelGroup<IsServer>::update()
{
	// update all tunnels
	uint32 current = core::coreClock();
	uint32 maxWait = 0xFFFFFFFF;
	typename Tunnels::iterator it = this->mTunnels.begin();
	for (; it != this->mTunnels.end(); ++it)
	{
		Tun *pTunnel = it->second;
		if (pTunnel)
		{			
			maxWait = min(maxWait, pTunnel->update(current));
		}
	}
	return maxWait;
}

template <bool IsServer>
int KcpTunnelGroup<IsServer>::handleInputNotification(int fd)
{
	// recv data from internet
	int maxlen = mKcpArg.mtu;
	char *buf = (char *)malloc(maxlen);
	assert(buf != NULL && "udp recv! malloc failed!");

	sockaddr_in addr;
	socklen_t addrlen = sizeof(addr);
	int recvlen = recvfrom(fd, buf, maxlen, 0, (SA *)&addr, &addrlen);

	// input to kcp
	if (recvlen > 0)
	{
		uint32 conv = 0;
		int ret = ikcp_get_conv(buf, recvlen, &conv);
		typename Tunnels::iterator it = ret ? mTunnels.find(conv) : mTunnels.end();
			
		if (it != mTunnels.end())
		{
			Tun *pTunnel = it->second;
			if (pTunnel)
			{
				pTunnel->input(buf, recvlen);
					
				ikcpcb *kcp = pTunnel->_debugGetKcpCb();
				logInfoLn("input conv="<<conv<<" len="<<recvlen<<
						  " snd_nxt"<<kcp->snd_nxt<<" rcv_nxt="<<kcp->rcv_nxt);
					
				pTunnel->onRecvPeerAddr((const SA *)&addr, addrlen);
				pTunnel->update(core::coreClock());
			}
		}
	}	
	free(buf);	
	return 0;
}
//--------------------------------------------------------------------------

NAMESPACE_END // namespace tun
