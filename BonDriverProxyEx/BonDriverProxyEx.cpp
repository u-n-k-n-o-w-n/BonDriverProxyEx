#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "BonDriverProxyEx.h"

#if _DEBUG
#define DETAILLOG	0
#define DETAILLOG2	0
static cProxyServerEx *debug;
#endif

static int Init(HMODULE hModule)
{
	char szIniPath[MAX_PATH + 16] = { '\0' };
	GetModuleFileNameA(hModule, szIniPath, MAX_PATH);
	char *p = strrchr(szIniPath, '.');
	if (!p)
		return -1;
	p++;
	strcpy(p, "ini");

	HANDLE hFile = CreateFileA(szIniPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return -2;
	CloseHandle(hFile);

	GetPrivateProfileStringA("OPTION", "ADDRESS", "127.0.0.1", g_Host, sizeof(g_Host), szIniPath);
	GetPrivateProfileStringA("OPTION", "PORT", "1192", g_Port, sizeof(g_Port), szIniPath);
	g_OpenTunerRetDelay = GetPrivateProfileIntA("OPTION", "OPENTUNER_RETURN_DELAY", 0, szIniPath);

	g_PacketFifoSize = GetPrivateProfileIntA("SYSTEM", "PACKET_FIFO_SIZE", 64, szIniPath);
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

cProxyServerEx::cProxyServerEx() : m_Error(TRUE, FALSE)
{
	m_s = INVALID_SOCKET;
	m_hModule = NULL;
	m_pIBon = m_pIBon2 = m_pIBon3 = NULL;
	m_bTunerOpen = m_bChannelLock = FALSE;
	m_hTsRead = NULL;
	m_pTsReceiversList = NULL;
	m_pStopTsRead = NULL;
	m_pTsLock = NULL;
	m_ppos = NULL;
	m_dwSpace = m_dwChannel = 0x7fffffff;	// INT_MAX
	m_pDriversMapKey = NULL;
	m_iDriverNo = -1;
	m_iDriverUseOrder = 0;
}

cProxyServerEx::~cProxyServerEx()
{
	LOCK(g_Lock);
	BOOL bRelease = TRUE;
	std::list<cProxyServerEx *>::iterator it = g_InstanceList.begin();
	while (it != g_InstanceList.end())
	{
		if (*it == this)
			g_InstanceList.erase(it++);
		else
		{
			if ((m_hModule != NULL) && (m_hModule == (*it)->m_hModule))
				bRelease = FALSE;
			++it;
		}
	}
	if (bRelease)
	{
		if (m_hTsRead)
		{
			*m_pStopTsRead = TRUE;
			::WaitForSingleObject(m_hTsRead, INFINITE);
			::CloseHandle(m_hTsRead);
			delete m_pTsReceiversList;
			delete m_pStopTsRead;
			delete m_pTsLock;
			delete m_ppos;
		}
		if (m_pIBon)
			m_pIBon->Release();
		if (m_hModule)
		{
			std::vector<stDriver> &vstDriver = DriversMap[m_pDriversMapKey];
			vstDriver[m_iDriverNo].bUsed = FALSE;
			::FreeLibrary(m_hModule);
		}
	}
	else
	{
		if (m_hTsRead)
			StopTsReceive();
	}
	if (m_s != INVALID_SOCKET)
		::closesocket(m_s);
}

DWORD WINAPI cProxyServerEx::Reception(LPVOID pv)
{
	cProxyServerEx *pProxy = static_cast<cProxyServerEx *>(pv);
	DWORD ret = pProxy->Process();
#if _DEBUG
	debug = NULL;
#endif
	delete pProxy;
	return ret;
}

DWORD cProxyServerEx::Process()
{
	HANDLE hThread[2];
	hThread[0] = ::CreateThread(NULL, 0, cProxyServerEx::Sender, this, 0, NULL);
	if (hThread[0] == NULL)
		return 1;

	hThread[1] = ::CreateThread(NULL, 0, cProxyServerEx::Receiver, this, 0, NULL);
	if (hThread[1] == NULL)
	{
		m_Error.Set();
		::WaitForSingleObject(hThread[0], INFINITE);
		::CloseHandle(hThread[0]);
		return 2;
	}

#if _DEBUG
	debug = this;
#endif

	HANDLE h[2] = { m_Error, m_fifoRecv.GetEventHandle() };
	for (;;)
	{
		DWORD dwRet = ::WaitForMultipleObjects(2, h, FALSE, INFINITE);
		switch (dwRet)
		{
		case WAIT_OBJECT_0:
			goto end;

		case WAIT_OBJECT_0 + 1:
		{
			// コマンド処理の全体をロックするので、BonDriver_Proxyをロードして自分自身に
			// 接続させるとデッドロックする
			// しかしそうしなければ困る状況と言うのは多分無いと思うので、これは仕様と言う事で
			LOCK(g_Lock);
			cPacketHolder *pPh;
			m_fifoRecv.Pop(&pPh);
#if _DEBUG && DETAILLOG2
			{
				char *CommandName[]={
					"eSelectBonDriver",
					"eCreateBonDriver",
					"eOpenTuner",
					"eCloseTuner",
					"eSetChannel1",
					"eGetSignalLevel",
					"eWaitTsStream",
					"eGetReadyCount",
					"eGetTsStream",
					"ePurgeTsStream",
					"eRelease",

					"eGetTunerName",
					"eIsTunerOpening",
					"eEnumTuningSpace",
					"eEnumChannelName",
					"eSetChannel2",
					"eGetCurSpace",
					"eGetCurChannel",

					"eGetTotalDeviceNum",
					"eGetActiveDeviceNum",
					"eSetLnbPower",
				};
				if (pPh->GetCommand() <= eSetLnbPower)
				{
					_RPT1(_CRT_WARN, "Recieve Command : [%s]\n", CommandName[pPh->GetCommand()]);
				}
				else
				{
					_RPT1(_CRT_WARN, "Illegal Command : [%d]\n", (int)(pPh->GetCommand()));
				}
			}
#endif
			switch (pPh->GetCommand())
			{
			case eSelectBonDriver:
			{
				if (pPh->GetBodyLength() <= sizeof(char))
					makePacket(eSelectBonDriver, FALSE);
				else
				{
					char *p;
					if ((p = ::strrchr((char *)(pPh->m_pPacket->payload), ':')) != NULL)
					{
						if (::strcmp(p, ":desc") == 0)	// 降順
						{
							*p = '\0';
							m_iDriverUseOrder = 1;
						}
						else if (::strcmp(p, ":asc") == 0)	// 昇順
							*p = '\0';
					}
					BOOL b = SelectBonDriver((LPCSTR)(pPh->m_pPacket->payload));
					if (b)
						g_InstanceList.push_back(this);
					makePacket(eSelectBonDriver, b);
				}
				break;
			}

			case eCreateBonDriver:
			{
				if (m_pIBon == NULL)
				{
					BOOL bFind = FALSE;
					for (std::list<cProxyServerEx *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
					{
						if (*it == this)
							continue;
						if (m_hModule == (*it)->m_hModule)
						{
							if ((*it)->m_pIBon != NULL)
							{
								bFind = TRUE;	// ここに来るのはかなりのレアケースのハズ
								m_pIBon = (*it)->m_pIBon;
								m_pIBon2 = (*it)->m_pIBon2;
								m_pIBon3 = (*it)->m_pIBon3;
								break;
							}
							// ここに来るのは上より更にレアケース
							// 一応リストの最後まで検索してみて、それでも見つからなかったら
							// CreateBonDriver()をやらせてみる
						}
					}
					if (!bFind)
					{
						if ((CreateBonDriver() != NULL) && (m_pIBon2 != NULL))
							makePacket(eCreateBonDriver, TRUE);
						else
						{
							makePacket(eCreateBonDriver, FALSE);
							m_Error.Set();
						}
					}
					else
						makePacket(eCreateBonDriver, TRUE);
				}
				else
					makePacket(eCreateBonDriver, TRUE);
				break;
			}

			case eOpenTuner:
			{
				BOOL bFind = FALSE;
				{
					for (std::list<cProxyServerEx *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
					{
						if (*it == this)
							continue;
						if ((m_pIBon != NULL) && (m_pIBon == (*it)->m_pIBon))
						{
							if ((*it)->m_bTunerOpen)
							{
								bFind = TRUE;
								m_bTunerOpen = TRUE;
								break;
							}
						}
					}
				}
				if (!bFind)
					m_bTunerOpen = OpenTuner();
				makePacket(eOpenTuner, m_bTunerOpen);
				break;
			}

			case eCloseTuner:
			{
				BOOL bFind = FALSE;
				for (std::list<cProxyServerEx *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
				{
					if (*it == this)
						continue;
					if ((m_pIBon != NULL) && (m_pIBon == (*it)->m_pIBon))
					{
						if ((*it)->m_bTunerOpen)
						{
							bFind = TRUE;
							break;
						}
					}
				}
				if (!bFind)
				{
					if (m_hTsRead)
					{
						*m_pStopTsRead = TRUE;
						::WaitForSingleObject(m_hTsRead, INFINITE);
						::CloseHandle(m_hTsRead);
						m_hTsRead = NULL;
						m_pTsReceiversList->clear();
						delete m_pTsReceiversList;
						m_pTsReceiversList = NULL;
						delete m_pStopTsRead;
						m_pStopTsRead = NULL;
						delete m_pTsLock;
						m_pTsLock = NULL;
						delete m_ppos;
						m_ppos = NULL;
					}
					CloseTuner();
				}
				m_bTunerOpen = FALSE;
				break;
			}

			case ePurgeTsStream:
			{
				if (m_hTsRead && m_bChannelLock)
				{
					LOCK(*m_pTsLock);
					PurgeTsStream();
					*m_ppos = 0;
					makePacket(ePurgeTsStream, TRUE);
				}
				else
					makePacket(ePurgeTsStream, FALSE);
				break;
			}

			case eRelease:
				m_Error.Set();
				break;

			case eEnumTuningSpace:
			{
				if (pPh->GetBodyLength() != sizeof(DWORD))
					makePacket(eEnumTuningSpace, _T(""));
				else
				{
					LPCTSTR p = EnumTuningSpace(::ntohl(*(DWORD *)(pPh->m_pPacket->payload)));
					if (p)
						makePacket(eEnumTuningSpace, p);
					else
						makePacket(eEnumTuningSpace, _T(""));
				}
				break;
			}

			case eEnumChannelName:
			{
				if (pPh->GetBodyLength() != (sizeof(DWORD) * 2))
					makePacket(eEnumChannelName, _T(""));
				else
				{
					LPCTSTR p = EnumChannelName(::ntohl(*(DWORD *)(pPh->m_pPacket->payload)), ::ntohl(*(DWORD *)&(pPh->m_pPacket->payload[sizeof(DWORD)])));
					if (p)
						makePacket(eEnumChannelName, p);
					else
						makePacket(eEnumChannelName, _T(""));
				}
				break;
			}

			case eSetChannel2:
			{
				if (pPh->GetBodyLength() != ((sizeof(DWORD) * 2) + sizeof(BYTE)))
					makePacket(eSetChannel2, (DWORD)0xff);
				else
				{
					m_bChannelLock = pPh->m_pPacket->payload[sizeof(DWORD) * 2];
					DWORD dwReqSpace = ::ntohl(*(DWORD *)(pPh->m_pPacket->payload));
					DWORD dwReqChannel = ::ntohl(*(DWORD *)&(pPh->m_pPacket->payload[sizeof(DWORD)]));
					BOOL bLocked = FALSE;
					BOOL bChanged = FALSE;
					BOOL bShared = FALSE;
					for (std::list<cProxyServerEx *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
					{
						if (*it == this)
							continue;
						// ひとまず現在のインスタンスが共有されているかどうかを確認しておく
						if ((m_pIBon != NULL) && (m_pIBon == (*it)->m_pIBon))
							bShared = TRUE;

						// 対象BonDriver群の中でチューナをオープンしているもの
						if (m_pDriversMapKey == (*it)->m_pDriversMapKey && (*it)->m_pIBon != NULL && (*it)->m_bTunerOpen)
						{
							// かつクライアントからの要求と同一チャンネルを選択しているもの
							if ((*it)->m_dwSpace == dwReqSpace && (*it)->m_dwChannel == dwReqChannel)
							{
								// 今クライアントがオープンしているチューナに関して
								if (m_pIBon != NULL)
								{
									BOOL bModule = FALSE;
									BOOL bIBon = FALSE;
									BOOL bTuner = FALSE;
									for (std::list<cProxyServerEx *>::iterator it2 = g_InstanceList.begin(); it2 != g_InstanceList.end(); ++it2)
									{
										if (*it2 == this)
											continue;
										if (m_hModule == (*it2)->m_hModule)
										{
											bModule = TRUE;	// モジュール使用者有り
											if (m_pIBon == (*it2)->m_pIBon)
											{
												bIBon = TRUE;	// インスタンス使用者有り
												if ((*it2)->m_bTunerOpen)
												{
													bTuner = TRUE;	// チューナ使用者有り
													break;
												}
											}
										}
									}

									// チューナ使用者無しならクローズ
									if (!bTuner)
									{
										if (m_hTsRead)
										{
											*m_pStopTsRead = TRUE;
											::WaitForSingleObject(m_hTsRead, INFINITE);
											::CloseHandle(m_hTsRead);
											//m_hTsRead = NULL;
											m_pTsReceiversList->clear();
											delete m_pTsReceiversList;
											//m_pTsReceiversList = NULL;
											delete m_pStopTsRead;
											//m_pStopTsRead = NULL;
											delete m_pTsLock;
											//m_pTsLock = NULL;
											delete m_ppos;
											//m_ppos = NULL;
										}
										CloseTuner();
										//m_bTunerOpen = FALSE;
										// かつインスタンス使用者も無しならインスタンスリリース
										if (!bIBon)
										{
											m_pIBon->Release();
											// m_pIBon = NULL;
											// かつモジュール使用者も無しならモジュールリリース
											if (!bModule)
											{
												std::vector<stDriver> &vstDriver = DriversMap[m_pDriversMapKey];
												vstDriver[m_iDriverNo].bUsed = FALSE;
												::FreeLibrary(m_hModule);
												// m_hModule = NULL;
											}
										}
									}
									else	// 他にチューナ使用者有りの場合
									{
										// 現在TSストリーム配信中ならその配信対象リストから自身を削除
										if (m_hTsRead)
											StopTsReceive();
									}
								}

								// インスタンス切り替え
								bChanged = TRUE;
								m_hModule = (*it)->m_hModule;
								m_iDriverNo = (*it)->m_iDriverNo;
								m_pIBon = (*it)->m_pIBon;
								m_pIBon2 = (*it)->m_pIBon2;
								m_pIBon3 = (*it)->m_pIBon3;
								m_bTunerOpen = TRUE;
								m_hTsRead = (*it)->m_hTsRead;	// この時点でもNULLの可能性はゼロではない
								m_pTsReceiversList = (*it)->m_pTsReceiversList;
								m_pStopTsRead = (*it)->m_pStopTsRead;
								m_pTsLock = (*it)->m_pTsLock;
								m_ppos = (*it)->m_ppos;
								if (m_hTsRead)
								{
									m_pTsLock->Enter();
									m_pTsReceiversList->push_back(this);
									m_pTsLock->Leave();
								}
#if _DEBUG && DETAILLOG2
								_RPT3(_CRT_WARN, "** found! ** : m_hModule[%p] / m_iDriverNo[%d] / m_pIBon[%p]\n", m_hModule, m_iDriverNo, m_pIBon);
								_RPT3(_CRT_WARN, "             : m_dwSpace[%d] / m_dwChannel[%d] / m_bChannelLock[%d]\n", dwReqSpace, dwReqChannel, m_bChannelLock);
#endif
								goto ok;	// これは酷い
							}
						}
					}

					// 同一チャンネルを使用中のチューナは見つからず、現在のチューナは共有されていたら
					if (bShared)
					{
						IBonDriver *old_pIBon = m_pIBon;
						BOOL old_bTunerOpen = m_bTunerOpen;
						// 出来れば未使用、無理ならなるべくロックされてないチューナを選択して、
						// 一気にチューナオープン状態にまで持って行く
						if (SelectBonDriver(m_pDriversMapKey))
						{
							if (m_pIBon == NULL)
							{
								// 未使用チューナがあった
								if ((CreateBonDriver() == NULL) || (m_pIBon2 == NULL))
								{
									makePacket(eSetChannel2, (DWORD)0xff);
									m_Error.Set();
									break;
								}
							}
							else
							{
								// インスタンス切り替えか？
								if (m_pIBon != old_pIBon)
									bChanged = TRUE;
								else
									m_bTunerOpen = old_bTunerOpen;
							}
							if (!m_bTunerOpen)
							{
								m_bTunerOpen = OpenTuner();
								if (!m_bTunerOpen)
								{
									makePacket(eSetChannel2, (DWORD)0xff);
									m_Error.Set();
									break;
								}
							}
						}
						else
						{
							makePacket(eSetChannel2, (DWORD)0xff);
							m_Error.Set();
							break;
						}

						// 使用チューナのチャンネルロック状態確認
						for (std::list<cProxyServerEx *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
						{
							if (*it == this)
								continue;
							if ((m_pIBon == (*it)->m_pIBon) && (*it)->m_bChannelLock)
							{
								bLocked = TRUE;
								break;
							}
						}
					}

#if _DEBUG && DETAILLOG2
					_RPT3(_CRT_WARN, "eSetChannel2 : bShared[%d] / bLocked[%d] / bChanged[%d]\n", bShared, bLocked, bChanged);
					_RPT3(_CRT_WARN, "             : dwReqSpace[%d] / dwReqChannel[%d] / m_bChannelLock[%d]\n", dwReqSpace, dwReqChannel, m_bChannelLock);
#endif

					if (bLocked && !m_bChannelLock)
					{
						// ロックされてる時は単純にロックされてる事を通知
						// この場合クライアントアプリへのSetChannel()の戻り値は成功になる
						// (おそらく致命的な問題にはならない)
						makePacket(eSetChannel2, (DWORD)0x01);
					}
					else
					{
						if (m_hTsRead)
							m_pTsLock->Enter();
						BOOL b = SetChannel(dwReqSpace, dwReqChannel);
						if (m_hTsRead)
							m_pTsLock->Leave();
						if (b)
						{
							// 同一BonDriverインスタンスを使用しているインスタンスの保持チャンネルを変更
							for (std::list<cProxyServerEx *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
							{
								if (*it == this)
									continue;
								if (m_pIBon == (*it)->m_pIBon)
								{
									(*it)->m_dwSpace = dwReqSpace;
									(*it)->m_dwChannel = dwReqChannel;
								}
							}
						ok:
							m_dwSpace = dwReqSpace;
							m_dwChannel = dwReqChannel;
							makePacket(eSetChannel2, (DWORD)0x00);
							if (m_hTsRead == NULL)
							{
								m_pTsReceiversList = new std::list<cProxyServerEx *>();
								m_pTsReceiversList->push_back(this);
								m_pStopTsRead = new BOOL(FALSE);
								m_pTsLock = new cCriticalSection();
								m_ppos = new DWORD(0);
								LPVOID *ppv = new LPVOID[5];
								ppv[0] = m_pIBon;
								ppv[1] = m_pTsReceiversList;
								ppv[2] = m_pStopTsRead;
								ppv[3] = m_pTsLock;
								ppv[4] = m_ppos;
								m_hTsRead = ::CreateThread(NULL, 0, cProxyServerEx::TsReader, ppv, 0, NULL);
								if (m_hTsRead == NULL)
								{
									delete[] ppv;
									m_pTsReceiversList->clear();
									delete m_pTsReceiversList;
									m_pTsReceiversList = NULL;
									delete m_pStopTsRead;
									m_pStopTsRead = NULL;
									delete m_pTsLock;
									m_pTsLock = NULL;
									delete m_ppos;
									m_ppos = NULL;
									m_Error.Set();
								}
							}
							else
							{
								// インスタンス切り替えだった場合は既存のTSバッファに介入しない
								if (!bChanged)
								{
									LOCK(*m_pTsLock);
									*m_ppos = 0;
								}
							}
						}
						else
							makePacket(eSetChannel2, (DWORD)0xff);
					}
				}
				break;
			}

			case eGetTotalDeviceNum:
				makePacket(eGetTotalDeviceNum, GetTotalDeviceNum());
				break;

			case eGetActiveDeviceNum:
				makePacket(eGetActiveDeviceNum, GetActiveDeviceNum());
				break;

			case eSetLnbPower:
			{
				if (pPh->GetBodyLength() != sizeof(BYTE))
					makePacket(eSetLnbPower, FALSE);
				else
					makePacket(eSetLnbPower, SetLnbPower((BOOL)(pPh->m_pPacket->payload[0])));
				break;
			}

			default:
				break;
			}
			delete pPh;
			break;
		}

		default:
			// 何かのエラー
			m_Error.Set();
			goto end;
		}
	}
end:
	::WaitForMultipleObjects(2, hThread, TRUE, INFINITE);
	::CloseHandle(hThread[0]);
	::CloseHandle(hThread[1]);
	return 0;
}

int cProxyServerEx::ReceiverHelper(char *pDst, DWORD left)
{
	int len, ret;
	fd_set rd;
	timeval tv;

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	while (left > 0)
	{
		if (m_Error.IsSet())
			return -1;

		FD_ZERO(&rd);
		FD_SET(m_s, &rd);
		if ((len = ::select((int)(m_s + 1), &rd, NULL, NULL, &tv)) == SOCKET_ERROR)
		{
			ret = -2;
			goto err;
		}

		if (len == 0)
			continue;

		// MSDNのrecv()のソース例とか見る限り、"SOCKET_ERROR"が負の値なのは保証されてるっぽい
		if ((len = ::recv(m_s, pDst, left, 0)) <= 0)
		{
			ret = -3;
			goto err;
		}
		left -= len;
		pDst += len;
	}
	return 0;
err:
	m_Error.Set();
	return ret;
}

DWORD WINAPI cProxyServerEx::Receiver(LPVOID pv)
{
	cProxyServerEx *pProxy = static_cast<cProxyServerEx *>(pv);
	char *p;
	DWORD left, ret;
	cPacketHolder *pPh = NULL;

	for (;;)
	{
		pPh = new cPacketHolder(16);
		left = sizeof(stPacketHead);
		p = (char *)&(pPh->m_pPacket->head);
		if (pProxy->ReceiverHelper(p, left) != 0)
		{
			ret = 201;
			goto end;
		}

		if (!pPh->IsValid())
		{
			pProxy->m_Error.Set();
			ret = 202;
			goto end;
		}

		left = pPh->GetBodyLength();
		if (left == 0)
		{
			pProxy->m_fifoRecv.Push(pPh);
			continue;
		}

		if (left > 16)
		{
			if (left > 512)
			{
				pProxy->m_Error.Set();
				ret = 203;
				goto end;
			}
			cPacketHolder *pTmp = new cPacketHolder(left);
			pTmp->m_pPacket->head = pPh->m_pPacket->head;
			delete pPh;
			pPh = pTmp;
		}

		p = (char *)(pPh->m_pPacket->payload);
		if (pProxy->ReceiverHelper(p, left) != 0)
		{
			ret = 204;
			goto end;
		}

		pProxy->m_fifoRecv.Push(pPh);
	}
end:
	delete pPh;
	return ret;
}

void cProxyServerEx::makePacket(enumCommand eCmd, BOOL b)
{
	cPacketHolder *p = new cPacketHolder(eCmd, sizeof(BYTE));
	p->m_pPacket->payload[0] = (BYTE)b;
	m_fifoSend.Push(p);
}

void cProxyServerEx::makePacket(enumCommand eCmd, DWORD dw)
{
	cPacketHolder *p = new cPacketHolder(eCmd, sizeof(DWORD));
	DWORD *pos = (DWORD *)(p->m_pPacket->payload);
	*pos = ::htonl(dw);
	m_fifoSend.Push(p);
}

void cProxyServerEx::makePacket(enumCommand eCmd, LPCTSTR str)
{
	register size_t size = (::_tcslen(str) + 1) * sizeof(TCHAR);
	cPacketHolder *p = new cPacketHolder(eCmd, size);
	::memcpy(p->m_pPacket->payload, str, size);
	m_fifoSend.Push(p);
}

void cProxyServerEx::makePacket(enumCommand eCmd, BYTE *pSrc, DWORD dwSize, float fSignalLevel)
{
	register size_t size = (sizeof(DWORD) * 2) + dwSize;
	cPacketHolder *p = new cPacketHolder(eCmd, size);
	union {
		DWORD dw;
		float f;
	} u;
	u.f = fSignalLevel;
	DWORD *pos = (DWORD *)(p->m_pPacket->payload);
	*pos++ = ::htonl(dwSize);
	*pos++ = ::htonl(u.dw);
	if (dwSize > 0)
		::memcpy(pos, pSrc, dwSize);
	m_fifoSend.Push(p);
}

DWORD WINAPI cProxyServerEx::Sender(LPVOID pv)
{
	cProxyServerEx *pProxy = static_cast<cProxyServerEx *>(pv);
	DWORD ret;
	HANDLE h[2] = { pProxy->m_Error, pProxy->m_fifoSend.GetEventHandle() };
	for (;;)
	{
		DWORD dwRet = ::WaitForMultipleObjects(2, h, FALSE, INFINITE);
		switch (dwRet)
		{
		case WAIT_OBJECT_0:
			ret = 101;
			goto end;

		case WAIT_OBJECT_0 + 1:
		{
			cPacketHolder *pPh;
			pProxy->m_fifoSend.Pop(&pPh);
			int left = (int)pPh->m_Size;
			char *p = (char *)(pPh->m_pPacket);
			while (left > 0)
			{
				int len = ::send(pProxy->m_s, p, left, 0);
				if (len == SOCKET_ERROR)
				{
					pProxy->m_Error.Set();
					break;
				}
				left -= len;
				p += len;
			}
			delete pPh;
			break;
		}

		default:
			// 何かのエラー
			pProxy->m_Error.Set();
			ret = 102;
			goto end;
		}
	}
end:
	return ret;
}

DWORD WINAPI cProxyServerEx::TsReader(LPVOID pv)
{
	LPVOID *ppv = static_cast<LPVOID *>(pv);
	IBonDriver *pIBon = static_cast<IBonDriver *>(ppv[0]);
	std::list<cProxyServerEx *> &TsReceiversList = *(static_cast<std::list<cProxyServerEx *> *>(ppv[1]));
	volatile BOOL &StopTsRead = *(static_cast<BOOL *>(ppv[2]));
	cCriticalSection &TsLock = *(static_cast<cCriticalSection *>(ppv[3]));
	DWORD &pos = *(static_cast<DWORD *>(ppv[4]));
	delete[] ppv;
	DWORD dwSize, dwRemain, now, before = 0;
	float fSignalLevel = 0;
	DWORD ret = 300;
	const DWORD TsPacketBufSize = g_TsPacketBufSize;
	BYTE *pBuf, *pTsBuf = new BYTE[TsPacketBufSize];
#if _DEBUG && DETAILLOG
	DWORD Counter = 0;
#endif

	// TS読み込みループ
	while (!StopTsRead)
	{
		if (((now = ::GetTickCount()) - before) >= 1000)
		{
			fSignalLevel = pIBon->GetSignalLevel();
			before = now;
		}
		dwSize = dwRemain = 0;
		{
			LOCK(TsLock);
			if (pIBon->GetTsStream(&pBuf, &dwSize, &dwRemain) && (dwSize != 0))
			{
				if ((pos + dwSize) < TsPacketBufSize)
				{
					::memcpy(&pTsBuf[pos], pBuf, dwSize);
					pos += dwSize;
					if (dwRemain == 0)
					{
						for (std::list<cProxyServerEx *>::iterator it = TsReceiversList.begin(); it != TsReceiversList.end(); ++it)
							(*it)->makePacket(eGetTsStream, pTsBuf, pos, fSignalLevel);
#if _DEBUG && DETAILLOG
						_RPT3(_CRT_WARN, "makePacket0() : %u : size[%x] / dwRemain[%d]\n", Counter++, pos, dwRemain);
#endif
						pos = 0;
					}
				}
				else
				{
					DWORD left, dwLen = TsPacketBufSize - pos;
					::memcpy(&pTsBuf[pos], pBuf, dwLen);
					for (std::list<cProxyServerEx *>::iterator it = TsReceiversList.begin(); it != TsReceiversList.end(); ++it)
						(*it)->makePacket(eGetTsStream, pTsBuf, TsPacketBufSize, fSignalLevel);
#if _DEBUG && DETAILLOG
					_RPT3(_CRT_WARN, "makePacket1() : %u : size[%x] / dwRemain[%d]\n", Counter++, TsPacketBufSize, dwRemain);
#endif
					left = dwSize - dwLen;
					pBuf += dwLen;
					while (left >= TsPacketBufSize)
					{
						for (std::list<cProxyServerEx *>::iterator it = TsReceiversList.begin(); it != TsReceiversList.end(); ++it)
							(*it)->makePacket(eGetTsStream, pBuf, TsPacketBufSize, fSignalLevel);
#if _DEBUG && DETAILLOG
						_RPT2(_CRT_WARN, "makePacket2() : %u : size[%x]\n", Counter++, TsPacketBufSize);
#endif
						left -= TsPacketBufSize;
						pBuf += TsPacketBufSize;
					}
					if (left != 0)
					{
						if (dwRemain == 0)
						{
							for (std::list<cProxyServerEx *>::iterator it = TsReceiversList.begin(); it != TsReceiversList.end(); ++it)
								(*it)->makePacket(eGetTsStream, pBuf, left, fSignalLevel);
#if _DEBUG && DETAILLOG
							_RPT3(_CRT_WARN, "makePacket3() : %u : size[%x] / dwRemain[%d]\n", Counter++, left, dwRemain);
#endif
							left = 0;
						}
						else
							::memcpy(pTsBuf, pBuf, left);
					}
					pos = left;
				}
			}
		}
		if (dwRemain == 0)
			::Sleep(WAIT_TIME);
	}
	delete[] pTsBuf;
	return ret;
}

void cProxyServerEx::StopTsReceive()
{
	// このメソッドは必ず、
	// 1. グローバルなインスタンスロック中
	// 2. かつ、TS受信中(m_hTsRead != NULL)
	// の2つを満たす状態で呼び出す事
	{
		LOCK(*m_pTsLock);
		std::list<cProxyServerEx *>::iterator it = m_pTsReceiversList->begin();
		while (it != m_pTsReceiversList->end())
		{
			if (*it == this)
			{
				m_pTsReceiversList->erase(it);
				break;
			}
			++it;
		}
	}
	// 自分が最後の受信者だった場合は、TS配信スレッドも停止
	if (m_pTsReceiversList->empty())
	{
		*m_pStopTsRead = TRUE;
		::WaitForSingleObject(m_hTsRead, INFINITE);
		::CloseHandle(m_hTsRead);
		m_hTsRead = NULL;
		delete m_pTsReceiversList;
		m_pTsReceiversList = NULL;
		delete m_pStopTsRead;
		m_pStopTsRead = NULL;
		delete m_pTsLock;
		m_pTsLock = NULL;
		delete m_ppos;
		m_ppos = NULL;
	}
}

BOOL cProxyServerEx::SelectBonDriver(LPCSTR p)
{
	char *pKey = NULL;
	std::vector<stDriver> *pvstDriver = NULL;
	for (std::map<char *, std::vector<stDriver> >::iterator it = DriversMap.begin(); it != DriversMap.end(); ++it)
	{
		if (::strcmp(p, it->first) == 0)
		{
			pKey = it->first;
			pvstDriver = &(it->second);
			break;
		}
	}
	if (pvstDriver == NULL)
	{
		m_hModule = NULL;
		return FALSE;
	}

	// まず使われてないのを探す
	std::vector<stDriver> &vstDriver = *pvstDriver;
	int i;
	if (m_iDriverUseOrder == 0)
		i = 0;
	else
		i = (int)(vstDriver.size() - 1);
	for (;;)
	{
		if (vstDriver[i].bUsed)
			goto next;
		HMODULE hModule = ::LoadLibraryA(vstDriver[i].strBonDriver);
		if (hModule != NULL)
		{
			m_hModule = hModule;
			vstDriver[i].bUsed = TRUE;
			m_pDriversMapKey = pKey;
			m_iDriverNo = i;

			// 各種項目再初期化の前に、現在TSストリーム配信中ならその配信対象リストから自身を削除
			if (m_hTsRead)
				StopTsReceive();

			// eSetChannel2からも呼ばれるので、各種項目再初期化
			m_pIBon = m_pIBon2 = m_pIBon3 = NULL;
			m_bTunerOpen = FALSE;
			m_hTsRead = NULL;
			m_pTsReceiversList = NULL;
			m_pStopTsRead = NULL;
			m_pTsLock = NULL;
			m_ppos = NULL;
			return TRUE;
		}
	next:
		if (m_iDriverUseOrder == 0)
		{
			if (i >= (int)(vstDriver.size() - 1))
				break;
			i++;
		}
		else
		{
			if (i <= 0)
				break;
			i--;
		}
	}

	// eSetChannel2からの呼び出しの場合
	if (m_pIBon)
	{
		// まず現在のインスタンスがチャンネルロックされてるかどうかチェックする
		std::list<cProxyServerEx *>::iterator it;
		BOOL bLocked = FALSE;
		for (it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
		{
			if (*it == this)
				continue;
			if (m_hModule == (*it)->m_hModule)
			{
				if ((*it)->m_bChannelLock)	// ロックされてた
				{
					bLocked = TRUE;
					break;
				}
			}
		}
		if (!bLocked)	// ロックされてなければインスタンスはそのままでOK
			return TRUE;

		// ここまで粘ったけど結局インスタンスが変わる可能性が大なので、
		// 現在TSストリーム配信中ならその配信対象リストから自身を削除
		if (m_hTsRead)
			StopTsReceive();
	}

	// 全部使われてたら(あるいはLoadLibrary()出来なければ)、チャンネルロックされてないのを優先で選択
	for (std::list<cProxyServerEx *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
	{
		if (*it == this)
			continue;
		if (::strcmp(p, (*it)->m_pDriversMapKey) == 0)	// ひとまず候補
		{
			m_hModule = (*it)->m_hModule;
			m_pDriversMapKey = (*it)->m_pDriversMapKey;
			m_iDriverNo = (*it)->m_iDriverNo;
			m_pIBon = (*it)->m_pIBon;	// (*it)->m_pIBonがNULLの可能性はゼロではない
			m_pIBon2 = (*it)->m_pIBon2;
			m_pIBon3 = (*it)->m_pIBon3;
			m_bTunerOpen = (*it)->m_bTunerOpen;
			m_hTsRead = (*it)->m_hTsRead;
			m_pTsReceiversList = (*it)->m_pTsReceiversList;
			m_pStopTsRead = (*it)->m_pStopTsRead;
			m_pTsLock = (*it)->m_pTsLock;
			m_ppos = (*it)->m_ppos;
			// かなりダサいけど多分現実的に問題になる程ではないので良しとする…
			BOOL bLocked = FALSE;
			for (std::list<cProxyServerEx *>::iterator it2 = g_InstanceList.begin(); it2 != g_InstanceList.end(); ++it2)
			{
				if (*it2 == this)
					continue;
				if (m_hModule == (*it2)->m_hModule)
				{
					if ((*it2)->m_bChannelLock)	// ロックされてた
					{
						bLocked = TRUE;
						break;
					}
				}
			}
			if (!bLocked)	// ロックされてなければ即決定
				break;
		}
	}
	// 選択したインスタンスが既にTSストリーム配信中なら、その配信対象リストに自身を追加
	if (m_hTsRead)
	{
		m_pTsLock->Enter();
		m_pTsReceiversList->push_back(this);
		m_pTsLock->Leave();
	}

	return (m_hModule != NULL);
}

IBonDriver *cProxyServerEx::CreateBonDriver()
{
	if (m_hModule)
	{
		IBonDriver *(*f)() = (IBonDriver *(*)())::GetProcAddress(m_hModule, "CreateBonDriver");
		if (f)
		{
			try { m_pIBon = f(); }
			catch (...) {}
			if (m_pIBon)
			{
				m_pIBon2 = dynamic_cast<IBonDriver2 *>(m_pIBon);
				m_pIBon3 = dynamic_cast<IBonDriver3 *>(m_pIBon);
			}
		}
	}
	return m_pIBon;
}

const BOOL cProxyServerEx::OpenTuner(void)
{
	BOOL b = FALSE;
	if (m_pIBon)
		b = m_pIBon->OpenTuner();
	if (g_OpenTunerRetDelay != 0)
		::Sleep(g_OpenTunerRetDelay);
	return b;
}

void cProxyServerEx::CloseTuner(void)
{
	if (m_pIBon)
		m_pIBon->CloseTuner();
}

void cProxyServerEx::PurgeTsStream(void)
{
	if (m_pIBon)
		m_pIBon->PurgeTsStream();
}

LPCTSTR cProxyServerEx::EnumTuningSpace(const DWORD dwSpace)
{
	LPCTSTR pStr = NULL;
	if (m_pIBon2)
		pStr = m_pIBon2->EnumTuningSpace(dwSpace);
	return pStr;
}

LPCTSTR cProxyServerEx::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	LPCTSTR pStr = NULL;
	if (m_pIBon2)
		pStr = m_pIBon2->EnumChannelName(dwSpace, dwChannel);
	return pStr;
}

const BOOL cProxyServerEx::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	BOOL b = FALSE;
	if (m_pIBon2)
		b = m_pIBon2->SetChannel(dwSpace, dwChannel);
	return b;
}

const DWORD cProxyServerEx::GetTotalDeviceNum(void)
{
	DWORD d = 0;
	if (m_pIBon3)
		d = m_pIBon3->GetTotalDeviceNum();
	return d;
}

const DWORD cProxyServerEx::GetActiveDeviceNum(void)
{
	DWORD d = 0;
	if (m_pIBon3)
		d = m_pIBon3->GetActiveDeviceNum();
	return d;
}

const BOOL cProxyServerEx::SetLnbPower(const BOOL bEnable)
{
	BOOL b = FALSE;
	if (m_pIBon3)
		b = m_pIBon3->SetLnbPower(bEnable);
	return b;
}

#if _DEBUG
struct HostInfo{
	char *host;
	char *port;
};
static DWORD WINAPI Listen(LPVOID pv)
{
	HostInfo *hinfo = static_cast<HostInfo *>(pv);
	char *host = hinfo->host;
	char *port = hinfo->port;
#else
static int Listen(char *host, char *port)
{
#endif
	addrinfo hints, *results, *rp;
	SOCKET lsock, csock;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
	if (getaddrinfo(host, port, &hints, &results) != 0)
	{
		hints.ai_flags = AI_PASSIVE;
		if (getaddrinfo(host, port, &hints, &results) != 0)
			return 1;
	}

	for (rp = results; rp != NULL; rp = rp->ai_next)
	{
		lsock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (lsock == INVALID_SOCKET)
			continue;

		BOOL exclusive = TRUE;
		setsockopt(lsock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));

		if (bind(lsock, rp->ai_addr, (int)(rp->ai_addrlen)) != SOCKET_ERROR)
			break;

		closesocket(lsock);
	}
	freeaddrinfo(results);
	if (rp == NULL)
		return 2;

	if (listen(lsock, 4) == SOCKET_ERROR)
	{
		closesocket(lsock);
		return 3;
	}

	for (;;)
	{
		csock = accept(lsock, NULL, NULL);
		if (csock == INVALID_SOCKET)
			continue;

		cProxyServerEx *pProxy = new cProxyServerEx();
		pProxy->setSocket(csock);
		HANDLE hThread = ::CreateThread(NULL, 0, cProxyServerEx::Reception, pProxy, 0, NULL);
		if (hThread)
			CloseHandle(hThread);
		else
			delete pProxy;
	}

	return 0;	// ここには来ない
}

#if _DEBUG
LRESULT CALLBACK WndProc(HWND hWnd, UINT iMsg, WPARAM wParam, LPARAM lParam)
{
	HDC hDc;
	TCHAR buf[1024];
	static int n = 0;

	switch (iMsg)
	{
	case WM_CREATE:
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	case WM_PAINT:
		PAINTSTRUCT ps;
		hDc = BeginPaint(hWnd, &ps);
		wsprintf(buf, TEXT("pProxy [%p] n[%d]"), debug, n);
		TextOut(hDc, 0, 0, buf, (int)_tcslen(buf));
		if (debug)
		{
			wsprintf(buf, TEXT("send_fifo size[%d] recv_fifo size[%d]"), debug->m_fifoSend.Size(), debug->m_fifoRecv.Size());
			TextOut(hDc, 0, 40, buf, (int)_tcslen(buf));
		}
		EndPaint(hWnd, &ps);
		return 0;

	case WM_LBUTTONDOWN:
		n++;
		InvalidateRect(hWnd, NULL, FALSE);
		return 0;
	}
	return DefWindowProc(hWnd, iMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE/*hPrevInstance*/, LPSTR/*lpCmdLine*/, int nCmdShow)
{
	static HANDLE hLogFile = CreateFile(_T("dbglog.txt"), GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
	_CrtSetReportFile(_CRT_WARN, hLogFile);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
	_CrtSetReportFile(_CRT_ERROR, hLogFile);
	_RPT0(_CRT_WARN, "--- PROCESS_START ---\n");
//	int *p = new int[2];	// リーク検出テスト用

	if (Init(hInstance) != 0)
		return -1;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return -2;

	HostInfo hinfo;
	hinfo.host = g_Host;
	hinfo.port = g_Port;
	HANDLE hThread = CreateThread(NULL, 0, Listen, &hinfo, 0, NULL);
	CloseHandle(hThread);

	HWND hWnd;
	MSG msg;
	WNDCLASSEX wndclass;

	wndclass.cbSize = sizeof(wndclass);
	wndclass.style = CS_HREDRAW | CS_VREDRAW;
	wndclass.lpfnWndProc = WndProc;
	wndclass.cbClsExtra = 0;
	wndclass.cbWndExtra = 0;
	wndclass.hInstance = hInstance;
	wndclass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
	wndclass.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
	wndclass.lpszMenuName = NULL;
	wndclass.lpszClassName = _T("Debug");
	wndclass.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

	RegisterClassEx(&wndclass);

	hWnd = CreateWindow(_T("Debug"), _T("Debug"), WS_OVERLAPPEDWINDOW, 256, 256, 512, 256, NULL, NULL, hInstance, NULL);

	ShowWindow(hWnd, nCmdShow);
	UpdateWindow(hWnd);

	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	delete debug;

	{
		LOCK(g_Lock);
		CleanUp();
	}

	WSACleanup();

	_RPT0(_CRT_WARN, "--- PROCESS_END ---\n");
//	CloseHandle(hLogFile);

	return (int)msg.wParam;
}
#else
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE/*hPrevInstance*/, LPSTR/*lpCmdLine*/, int/*nCmdShow*/)
{
	if (Init(hInstance) != 0)
		return -1;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return -2;

	int ret = Listen(g_Host, g_Port);

	{
		// 来ないけど一応
		LOCK(g_Lock);
		CleanUp();
	}

	WSACleanup();
	return ret;
}
#endif
