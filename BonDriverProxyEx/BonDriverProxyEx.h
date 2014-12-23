#ifndef __BONDRIVER_PROXYEX_H__
#define __BONDRIVER_PROXYEX_H__
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tchar.h>
#include <process.h>
#include <list>
#include <queue>
#include <map>
#include "Common.h"
#include "IBonDriver3.h"

#if _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#define new new(_NORMAL_BLOCK, __FILE__, __LINE__)
#endif

#define WAIT_TIME	10	// GetTsStream()の後で、dwRemainが0だった場合に待つ時間(ms)

////////////////////////////////////////////////////////////////////////////////

static char g_Host[256];
static char g_Port[8];
static size_t g_PacketFifoSize;
static DWORD g_TsPacketBufSize;
static DWORD g_OpenTunerRetDelay;

#include "BdpPacket.h"

#define MAX_DRIVERS	64		// ドライバのグループ数とグループ内の数の両方
static char **g_ppDriver[MAX_DRIVERS];
struct stDriver {
	char *strBonDriver;
	BOOL bUsed;
	FILETIME ftLoad;
};
static std::map<char *, std::vector<stDriver> > DriversMap;

////////////////////////////////////////////////////////////////////////////////

class cProxyServerEx {
	IBonDriver *m_pIBon;
	IBonDriver2 *m_pIBon2;
	IBonDriver3 *m_pIBon3;
	HMODULE m_hModule;
	SOCKET m_s;
	cEvent m_Error;
	BOOL m_bTunerOpen;
	HANDLE m_hTsRead;
	std::list<cProxyServerEx *> *m_pTsReceiversList;
	BOOL * volatile m_pStopTsRead;
	cCriticalSection *m_pTsLock;
	DWORD *m_ppos;
	BOOL m_bChannelLock;
	DWORD m_dwSpace;
	DWORD m_dwChannel;
	char *m_pDriversMapKey;
	int m_iDriverNo;
	int m_iDriverUseOrder;
#if _DEBUG
public:
#endif
	cPacketFifo m_fifoSend;
	cPacketFifo m_fifoRecv;

#if _DEBUG
private:
#endif
	DWORD Process();
	int ReceiverHelper(char *pDst, DWORD left);
	static DWORD WINAPI Receiver(LPVOID pv);
	void makePacket(enumCommand eCmd, BOOL b);
	void makePacket(enumCommand eCmd, DWORD dw);
	void makePacket(enumCommand eCmd, LPCTSTR str);
	void makePacket(enumCommand eCmd, BYTE *pSrc, DWORD dwSize, float fSignalLevel);
	static DWORD WINAPI Sender(LPVOID pv);
	static DWORD WINAPI TsReader(LPVOID pv);
	void StopTsReceive();

	BOOL SelectBonDriver(LPCSTR p);
	IBonDriver *CreateBonDriver();

	// IBonDriver
	const BOOL OpenTuner(void);
	void CloseTuner(void);
	void PurgeTsStream(void);

	// IBonDriver2
	LPCTSTR EnumTuningSpace(const DWORD dwSpace);
	LPCTSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel);
	const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel);

	// IBonDriver3
	const DWORD GetTotalDeviceNum(void);
	const DWORD GetActiveDeviceNum(void);
	const BOOL SetLnbPower(const BOOL bEnable);

public:
	cProxyServerEx();
	~cProxyServerEx();
	void setSocket(SOCKET s){ m_s = s; }
	static DWORD WINAPI Reception(LPVOID pv);
};

static std::list<cProxyServerEx *> g_InstanceList;
static cCriticalSection g_Lock;

#endif	// __BONDRIVER_PROXYEX_H__
