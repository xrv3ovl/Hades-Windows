#pragma once
#include <NetApi.h>

class KNetWork
{
public:
	KNetWork();
	~KNetWork();

	const bool NetDrvInit();

public:
	void SetAllRule();
	void ReLoadDnsRule();
	void ReLoadTcpConnectRule();
};