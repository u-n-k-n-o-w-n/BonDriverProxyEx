#ifndef __BONDRIVER_PROXYEX_H__
#define __BONDRIVER_PROXYEX_H__
#include <windows.h>
#include <tchar.h>
#include <process.h>
#include <list>
#include <queue>
#include <map>
#include "IBonDriver3.h"

#if _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#define new new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define DETAILLOG	0
#define DETAILLOG2	0
#endif

#define TUNER_NAME	"BonDriverProxyEx"
#define WAIT_TIME	10	// GetTsStream()の後で、dwRemainが0だった場合に待つ時間(ms)

////////////////////////////////////////////////////////////////////////////////

static char g_Host[64];
static unsigned short g_Port;
static size_t g_PacketFifoSize;
static size_t g_TsFifoSize;
static DWORD g_TsPacketBufSize;

#define MAX_DRIVERS	64		// ドライバのグループ数とグループ内の数の両方
static char **g_ppDriver[MAX_DRIVERS];
struct stDriver {
	char *strBonDriver;
	BOOL bUsed;
};
static std::map<char *, std::vector<stDriver> > DriversMap;

static int Init(HMODULE hModule)
{
	char szIniPath[MAX_PATH + 16] = { '\0' };
	GetModuleFileNameA(hModule, szIniPath, MAX_PATH);
	char *p = strrchr(szIniPath, '.');
	if (!p)
		return -1;
	p++;
	strcpy(p, "ini");

	GetPrivateProfileStringA("OPTION", "ADDRESS", "127.0.0.1", g_Host, sizeof(g_Host), szIniPath);
	g_Port = (unsigned short)GetPrivateProfileIntA("OPTION", "PORT", 1192, szIniPath);

	g_PacketFifoSize = GetPrivateProfileIntA("SYSTEM", "PACKET_FIFO_SIZE", 16, szIniPath);
	g_TsFifoSize = GetPrivateProfileIntA("SYSTEM", "TS_FIFO_SIZE", 32, szIniPath);
	g_TsPacketBufSize = GetPrivateProfileIntA("SYSTEM", "TSPACKET_BUFSIZE", (188 * 1024), szIniPath);

	{
		// [OPTION]
		// BONDRIVER=PT-T
		// [BONDRIVER]
		// 00=PT-T;BonDriver_PT3-T0.dll;BonDriver_PT3-T1.dll
		// 01=PT-S;BonDriver_PT3-S0.dll;BonDriver_PT3-S1.dll
		int cntD, cntT = 0;
		char *str, *p, *pos, *pp[MAX_DRIVERS], **ppDriver;
		char tag[4], buf[MAX_PATH * 4];
		for (int i = 0; i < MAX_DRIVERS; i++)
		{
			tag[0] = (char)('0' + (i / 10));
			tag[1] = (char)('0' + (i % 10));
			tag[2] = '\0';
			GetPrivateProfileStringA("BONDRIVER", tag, "", buf, sizeof(buf), szIniPath);
			if (buf[0] == '\0')
			{
				g_ppDriver[cntT] = NULL;
				break;
			}

			// format: GroupName;BonDriver1;BonDriver2;BonDriver3...
			// e.g.  : PT-T;BonDriver_PT3-T0.dll;BonDriver-PT3_T1.dll
			str = new char[strlen(buf) + 1];
			strcpy(str, buf);
			pos = pp[0] = str;
			cntD = 1;
			for (;;)
			{
				p = strchr(pos, ';');
				if (p)
				{
					*p = '\0';
					pos = pp[cntD++] = p + 1;
					if (cntD > (MAX_DRIVERS - 1))
						break;
				}
				else
					break;
			}
			if (cntD == 1)
			{
				delete[] str;
				continue;
			}
			ppDriver = g_ppDriver[cntT++] = new char *[cntD];
			memcpy(ppDriver, pp, sizeof(char *) * cntD);
			std::vector<stDriver> vstDriver(cntD - 1);
			for (int j = 1; j < cntD; j++)
			{
				vstDriver[j-1].strBonDriver = ppDriver[j];
				vstDriver[j-1].bUsed = FALSE;
			}
			DriversMap[ppDriver[0]] = vstDriver;
		}
	}

	return 0;
}

static void CleanUp()
{
	DriversMap.clear();
	for (int i = 0; i < MAX_DRIVERS; i++)
	{
		if (g_ppDriver[i] == NULL)
			break;
		delete[] g_ppDriver[i][0];
		delete[] g_ppDriver[i];
	}
}

////////////////////////////////////////////////////////////////////////////////

class cCriticalSection {
	CRITICAL_SECTION m_c;
public:
	cCriticalSection(){ ::InitializeCriticalSection(&m_c); }
	~cCriticalSection(){ ::DeleteCriticalSection(&m_c); }
	void Enter(){ ::EnterCriticalSection(&m_c); }
	void Leave(){ ::LeaveCriticalSection(&m_c); }
};

class cLock {
	cCriticalSection &m_c;
	cLock &operator=(const cLock &);	// shut up C4512
public:
	cLock(cCriticalSection &ref) : m_c(ref) { m_c.Enter(); }
	~cLock(){ m_c.Leave(); }
};

#define LOCK(key) cLock __Lock__(key)

class cEvent {
	HANDLE m_h;
	DWORD m_dwWait;
public:
	cEvent(BOOL bManualReset = FALSE, BOOL bInitialState = FALSE, DWORD dwMilliseconds = INFINITE)
	{
		m_dwWait = dwMilliseconds;
		m_h = ::CreateEvent(NULL, bManualReset, bInitialState, NULL);
	}
	~cEvent(){ ::CloseHandle(m_h); }
	BOOL IsSet(){ return (::WaitForSingleObject(m_h, 0) == WAIT_OBJECT_0); }
	DWORD Wait(){ return ::WaitForSingleObject(m_h, m_dwWait); }
	BOOL Set(){ return ::SetEvent(m_h); }
	BOOL Reset(){ return ::ResetEvent(m_h); }
	operator HANDLE () const { return m_h; }
};

////////////////////////////////////////////////////////////////////////////////

enum enumCommand {
	eSelectBonDriver = 0,
	eCreateBonDriver,
	eOpenTuner,
	eCloseTuner,
	eSetChannel1,
	eGetSignalLevel,
	eWaitTsStream,
	eGetReadyCount,
	eGetTsStream,
	ePurgeTsStream,
	eRelease,

	eGetTunerName,
	eIsTunerOpening,
	eEnumTuningSpace,
	eEnumChannelName,
	eSetChannel2,
	eGetCurSpace,
	eGetCurChannel,

	eGetTotalDeviceNum,
	eGetActiveDeviceNum,
	eSetLnbPower,
};

__declspec(align(1)) struct stPacketHead {
	BYTE m_bSync;
	BYTE m_bCommand;
	BYTE m_bReserved1;
	BYTE m_bReserved2;
	DWORD m_dwBodyLength;
};

__declspec(align(1)) struct stPacket {
	stPacketHead head;
	BYTE payload[1];
};

#define SYNC_BYTE	0xff
class cPacketHolder {
	friend class cProxyServerEx;
	union {
		stPacket *m_pPacket;
		BYTE *m_pBuf;
	};
	size_t m_Size;

	inline void init(size_t PayloadSize)
	{
		m_pBuf = new BYTE[sizeof(stPacketHead) + PayloadSize];
	}

public:
	cPacketHolder(size_t PayloadSize)
	{
		init(PayloadSize);
	}

	cPacketHolder(enumCommand eCmd, size_t PayloadSize)
	{
		init(PayloadSize);
		*(DWORD *)m_pBuf = 0;
		m_pPacket->head.m_bSync = SYNC_BYTE;
		SetCommand(eCmd);
		m_pPacket->head.m_dwBodyLength = ::htonl((DWORD)PayloadSize);
		m_Size = sizeof(stPacketHead) + PayloadSize;
	}

	~cPacketHolder()
	{
		delete[] m_pBuf;
	}
	inline BOOL IsValid(){ return (m_pPacket->head.m_bSync == SYNC_BYTE); }
	inline BOOL IsTS(){ return (m_pPacket->head.m_bCommand == (BYTE)eGetTsStream); }
	inline enumCommand GetCommand(){ return (enumCommand)m_pPacket->head.m_bCommand; }
	inline void SetCommand(enumCommand eCmd){ m_pPacket->head.m_bCommand = (BYTE)eCmd; }
	inline DWORD GetBodyLength(){ return ::ntohl(m_pPacket->head.m_dwBodyLength); }
};

class cPacketFifo : protected std::queue<cPacketHolder *> {
	const size_t m_fifoSize;
	cCriticalSection m_Lock;
	cEvent m_Event;
	cPacketFifo &operator=(const cPacketFifo &);	// shut up C4512

public:
	cPacketFifo() : m_fifoSize(g_PacketFifoSize), m_Event(TRUE, FALSE){}
	~cPacketFifo()
	{
		LOCK(m_Lock);
		while (!empty())
		{
			cPacketHolder *p = front();
			pop();
			delete p;
		}
	}

	void Push(cPacketHolder *p)
	{
		LOCK(m_Lock);
		if (size() >= m_fifoSize)
		{
#if _DEBUG
			_RPT1(_CRT_WARN, "Packet Queue OVERFLOW : size[%d]\n", size());
#endif
			// TSの場合のみドロップ
			if (p->IsTS())
			{
				delete p;
				return;
			}
		}
		push(p);
		m_Event.Set();
	}

	void Pop(cPacketHolder **p)
	{
		LOCK(m_Lock);
		if (!empty())
		{
			*p = front();
			pop();
			if (empty())
				m_Event.Reset();
		}
		else
			m_Event.Reset();
	}

	HANDLE GetEventHandle()
	{
		return (HANDLE)m_Event;
	}

#if _DEBUG
	inline size_t Size()
	{
		return size();
	}
#endif
};

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
#endif	// __BONDRIVER_PROXYEX_H__
