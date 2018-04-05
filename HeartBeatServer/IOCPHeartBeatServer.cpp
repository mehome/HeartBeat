#include "stdafx.h"
#include "Header.h"
#include "IOCPHeartBeatServer.h"
#include "Log.h"
#include "windows.h"
#include "tchar.h"
#include <shlwapi.h>
#include <string>
using std::string;
//#include "vld.h"
//#pragma comment(lib, "vld.lib")
using namespace std;
#pragma warning(disable:4996)
// 每一个处理器上产生多少个线程(为了最大限度的提升服务器性能，详见配套文档)
#define WORKER_THREADS_PER_PROCESSOR 2
// 同时投递的Accept请求的数量(这个要根据实际的情况灵活设置)
#define MAX_POST_ACCEPT              8
// 传递给Worker线程的退出信号
#define EXIT_CODE                    NULL

// 默认端口
#define DEFAULT_PORT          "12345"
// 默认IP地址
#define DEFAULT_IP            _T("127.0.0.1")

// 释放指针和句柄资源的宏
// 释放指针宏
#define RELEASE(x)                      {if(x != NULL ){delete x;x=NULL;}}
// 释放句柄宏
#define RELEASE_HANDLE(x)               {if(x != NULL && x!=INVALID_HANDLE_VALUE){ CloseHandle(x);x = NULL;}}
// 释放Socket宏
#define RELEASE_SOCKET(x)               {if(x !=INVALID_SOCKET) { closesocket(x);x=INVALID_SOCKET;}}

#pragma comment(lib,"ws2_32.lib")

IOCPHeartBeatServer::IOCPHeartBeatServer()
	:m_nThreads(0),
	m_phWorkerThreads(NULL),
	m_hIOCompletionPort(NULL),
	m_hShutdownEvent(NULL),
	m_bLibLoaded(false),
	m_nPort(0),
	m_pListenContext(NULL)
{
	// 初始化线程临界区
	InitializeCriticalSection(&m_csClientSockContext);
	Log::getInstance().setLogLevel(LEVEL_INFOR);
}


IOCPHeartBeatServer::~IOCPHeartBeatServer()
{
	Stop();
	// 删除客户端列表临界区
	DeleteCriticalSection(&m_csClientSockContext);
	Log::getInstance().removeInstance();
}

bool IOCPHeartBeatServer::LoadSockLib()
{
	WSADATA wsaData;
	int nResult;
	nResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	// 错误(一般都不可能出现)
	if (NO_ERROR != nResult)
	{
		Loger(LEVEL_ERROR, perro("WSAStartup", WSAGetLastError()).c_str());
		return m_bLibLoaded = false;
	}
	return m_bLibLoaded = true;
}

void IOCPHeartBeatServer::UnloadSockLib()
{
	if (m_bLibLoaded)
	{
		m_bLibLoaded = false;
		if (SOCKET_ERROR == WSACleanup())
			Loger(LEVEL_ERROR, perro("WSACleanup", WSAGetLastError()).c_str());
	}
}

bool IOCPHeartBeatServer::Start()
{
	// 读取配置信息
	if (false == ReadConfig())
	{
		return false;
	}

	// 建立系统退出的事件通知
	m_hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (NULL == m_hShutdownEvent)
	{
		Loger(LEVEL_ERROR, perro("初始化退出时间失败.", WSAGetLastError()).c_str());
		return false;
	}

	// 初始化IOCP
	if (false == _InitializeIOCP())
	{
		Loger(LEVEL_ERROR, perro("初始化IOCP失败.", WSAGetLastError()).c_str());
		return false;
	}
	else
	{
		Loger(LEVEL_INFOR, "初始化IOCP完成.");
	}

	// 初始化Socket
	if (false == _InitializeListenSocket())
	{
		Loger(LEVEL_ERROR, perro("_InitializeListenSocket", WSAGetLastError()).c_str());
		Stop();
		return false;
	}
	else
	{
		Loger(LEVEL_INFOR, "Listen Socket初始化完毕.");
	}

	//this->_ShowMessage(_T("系统准备就绪，等候连接....\n"));
	return false;
}

void IOCPHeartBeatServer::Stop()
{
	// 激活关闭消息通知
	if (NULL != m_hShutdownEvent && INVALID_HANDLE_VALUE != m_hShutdownEvent)
		SetEvent(m_hShutdownEvent);

	if (NULL != m_phWorkerThreads)
	{
		for (int i = 0; i < m_nThreads; i++)
		{
			// 通知所有的完成端口操作退出
			PostQueuedCompletionStatus(m_hIOCompletionPort, 0, (DWORD)EXIT_CODE, NULL);
		}
		// 等待所有的客户端资源退出
		WaitForMultipleObjects(m_nThreads, m_phWorkerThreads, TRUE, INFINITE);
	}

	// 清除客户端列表信息
	this->_ClearContextList();

	// 释放其他资源
	this->_DeInitialize();
}

string IOCPHeartBeatServer::GetLocalIP()
{
	// 获得本机主机名
	char hostname[MAX_PATH] = { 0 };
	gethostname(hostname, MAX_PATH);
	struct hostent FAR* lpHostEnt = gethostbyname(hostname);
	if (lpHostEnt == NULL)
	{
		return DEFAULT_IP;
	}

	// 取得IP地址列表中的第一个为返回的IP(因为一台主机可能会绑定多个IP)
	LPSTR lpAddr = lpHostEnt->h_addr_list[0];

	// 将IP地址转化成字符串形式
	struct in_addr inAddr;
	memmove(&inAddr, lpAddr, 4);
	m_strIP = string(inet_ntoa(inAddr));

	return m_strIP;
}

DWORD WINAPI IOCPHeartBeatServer::_WorkerThread(LPVOID lpParam)
{
	THREADPARAMS* pParam = (THREADPARAMS*)lpParam;
	IOCPHeartBeatServer* pIOCPServer = (IOCPHeartBeatServer*)pParam->pIOCPServer;
	int nThreadNo = (int)pParam->nThreadNo;

	trace(_T("工作者线程启动，ID: %d.\n"), nThreadNo);

	OVERLAPPED           *pOverlapped = NULL;
	PER_SOCKET_CONTEXT   *pSocketContext = NULL;
	DWORD                dwBytesTransfered = 0;

	// 循环处理请求，知道接收到Shutdown信息为止
	while (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPServer->m_hShutdownEvent, 0))
	{
		BOOL bReturn = GetQueuedCompletionStatus(
			pIOCPServer->m_hIOCompletionPort,
			&dwBytesTransfered,
			(PULONG_PTR)&pSocketContext,
			&pOverlapped,
			INFINITE);

		// 如果收到的是退出标志，则直接退出
		if (EXIT_CODE == (DWORD)pSocketContext)
		{
			Loger(LEVEL_ERROR, perro("接收超时", WSAGetLastError()).c_str());
			break;
		}

		// 判断是否出现了错误
		if (!bReturn)
		{
			DWORD dwErr = GetLastError();
			// 读取传入的参数
			PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);
			// 显示一下提示信息
			if (!pIOCPServer->HandleError(pSocketContext, pIoContext, dwErr))
			{
				break;
			}

			continue;
		}
		else
		{
			// 读取传入的参数
			PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);
			// 判断是否有客户端断开了
			if ((0 == dwBytesTransfered) 
				&& (ACCEPT_POSTED == pIoContext->m_OpType 
				|| SEND_POSTED == pIoContext->m_OpType
				|| RECV_POSTED == pIoContext->m_OpType))
			{
				Loger(LEVEL_INFOR, "客户端：%s:%d 断开连接！", inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), ntohs(pSocketContext->m_ClientAddr.sin_port));
				// 释放掉对应的资源
				pIOCPServer->_RemoveContext(pSocketContext);
				continue;
			}
			else
			{
				switch (pIoContext->m_OpType)
				{
					// Accept  
				case ACCEPT_POSTED:
				{
					// 为了增加代码可读性，这里用专门的_DoAccept函数进行处理连入请求,并且获取第一组数据头。
					PER_IO_CONTEXT* pNewClientIoContext = NULL;
					PER_SOCKET_CONTEXT* pNewClientSocketContext = NULL;
					if (true == pIOCPServer->_DoAccpet(pSocketContext, pIoContext, pNewClientSocketContext, pNewClientIoContext))
					{
						// 不知道为什么取到pNewClientIoContext 和pNewClientSocketContext 的值
						// 所以处理接收第一组数据的处理移到_DoAccpet中。
						// 处理第一组数据
						//pIOCPServer->_DealRecvData(pNewClientSocketContext, pNewClientIoContext);
					}
				}
				break;
				// SEND
				case SEND_POSTED:
				{
					// 发送数据完成后，投递接收数据的请求
					pIOCPServer->_DoPostRecv(pSocketContext, pIoContext);
				}
				break;
				// RECV
				case RECV_POSTED:
				{
					// 投递收版本信息的请求,投递请求的时候不能立马关闭socket
					pIOCPServer->_DealRecvData(pSocketContext, pIoContext);
				}
				break;
				default:
					// 不应该执行到这里
					trace(_T("_WorkThread中的 pIoContext->m_OpType 参数异常.\n"));
					break;
				} //switch
			}//if
		}//if

	}//while

	trace(_T("工作者线程 %d 号退出.\n"), nThreadNo);

	// 释放线程参数
	RELEASE(lpParam);

	return 0;
}

////////////////////////////////
// 初始化完成端口
bool IOCPHeartBeatServer::_InitializeIOCP()
{
	// 建立第一个完成端口
	m_hIOCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (NULL == m_hIOCompletionPort)
	{
		Loger(LEVEL_ERROR, perro("CreateIoCompletionPort", WSAGetLastError()).c_str());
		return false;
	}

	// 根据本机中的处理器数量，建立对应的线程数
	m_nThreads = WORKER_THREADS_PER_PROCESSOR * _GetNoOfProcessors();

	// 为工作者线程初始化句柄
	m_phWorkerThreads = new HANDLE[m_nThreads];

	// 根据计算出来的数量建立工作者线程
	DWORD nThreadID;
	for (int i = 0; i < m_nThreads; i++)
	{
		PTHREADPARAMS pThreadParams = new THREADPARAMS;
		pThreadParams->pIOCPServer = this;
		pThreadParams->nThreadNo = i + 1;
		m_phWorkerThreads[i] = ::CreateThread(0, 0, _WorkerThread, (void *)pThreadParams, 0, &nThreadID);
	}

	trace(" 建立 _WorkerThread %d 个.\n", m_nThreads);
	return true;
}


/////////////////////////////////////////////////////////////////
// 初始化Socket
bool IOCPHeartBeatServer::_InitializeListenSocket()
{
	// AcceptEx 和 GetAcceptExSockaddrs 的GUID，用于导出函数指针
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;
	GUID GuidTransmitFile = WSAID_TRANSMITFILE;

	// 服务器地址信息，用于绑定Socket
	struct sockaddr_in ServerAddress;

	// 生成用于监听的Socket的信息
	m_pListenContext = new PER_SOCKET_CONTEXT;

	// 需要使用重叠IO，必须得使用WSASocket来建立Socket，才可以支持重叠IO操作
	m_pListenContext->m_Socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == m_pListenContext->m_Socket)
	{
		Loger(LEVEL_ERROR, perro("初始化socket失败，错误代码！", WSAGetLastError()).c_str());
		return false;
	}
	else
	{
		trace("WSASocket() 完成.\n");
	}

	// 将Listen Socket绑定至完成端口中
	if (NULL == CreateIoCompletionPort((HANDLE)m_pListenContext->m_Socket, m_hIOCompletionPort, (DWORD)m_pListenContext, 0))
	{
		Loger(LEVEL_ERROR, perro("绑定 Listen Socket至完成端口失败！", WSAGetLastError()).c_str());
		RELEASE_SOCKET(m_pListenContext->m_Socket);
		return false;
	}
	else
	{
		trace("Listen Socket绑定完成端口 完成.\n");
	}

	// 填充地址信息
	ZeroMemory((char *)&ServerAddress, sizeof(ServerAddress));
	ServerAddress.sin_family = AF_INET;
	// 这里可以绑定任何可用的IP地址，或者绑定一个指定的IP地址 
	//ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);                      
	ServerAddress.sin_addr.s_addr = inet_addr(m_strIP.c_str());
	ServerAddress.sin_port = htons(m_nPort);

	// 绑定地址和端口
	if (SOCKET_ERROR == ::bind(m_pListenContext->m_Socket, (struct sockaddr *) &ServerAddress, sizeof(ServerAddress)))
	{
		Loger(LEVEL_ERROR, perro("bind函数执行错误！", WSAGetLastError()).c_str());
		return false;
	}
	else
	{
		trace("bind() 完成.\n");
	}

	// 开始进行监听
	if (SOCKET_ERROR == listen(m_pListenContext->m_Socket, SOMAXCONN))
	{
		Loger(LEVEL_ERROR, perro("listen函数执行错误！", WSAGetLastError()).c_str());
		return false;
	}
	else
	{
		trace("Listen() 完成.\n");
	}

	// 使用AcceptEx函数，因为这个是属于WinSock2规范之外的微软另外提供的扩展函数
	// 所以需要额外获取一下函数的指针，
	// 获取AcceptEx函数指针
	DWORD dwBytes = 0;
	if (SOCKET_ERROR == WSAIoctl(
		m_pListenContext->m_Socket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&GuidAcceptEx,
		sizeof(GuidAcceptEx),
		&m_lpfnAcceptEx,
		sizeof(m_lpfnAcceptEx),
		&dwBytes,
		NULL,
		NULL))
	{
		Loger(LEVEL_ERROR, perro("WSAIoctl 未能获取AcceptEx函数指针！", WSAGetLastError()).c_str());
		return false;
	}

	// 获取GetAcceptExSockAddrs函数指针，也是同理
	if (SOCKET_ERROR == WSAIoctl(
		m_pListenContext->m_Socket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&GuidGetAcceptExSockAddrs,
		sizeof(GuidGetAcceptExSockAddrs),
		&m_lpfnGetAcceptExSockAddrs,
		sizeof(m_lpfnGetAcceptExSockAddrs),
		&dwBytes,
		NULL,
		NULL))
	{
		Loger(LEVEL_ERROR, perro("WSAIoctl 未能获取GetAcceptExSockAddrs函数指针！", WSAGetLastError()).c_str());
		return false;
	}

	// 获取TransmitFile函数指针，也是同理
	dwBytes = 0;
	if (SOCKET_ERROR == WSAIoctl(
		m_pListenContext->m_Socket,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&GuidTransmitFile,
		sizeof(GuidTransmitFile),
		&m_lpfnTransmitFile,
		sizeof(m_lpfnTransmitFile),
		&dwBytes,
		NULL,
		NULL))
	{
		Loger(LEVEL_ERROR, perro("WSAIoctl 未能获取TransmitFile函数指针！", WSAGetLastError()).c_str());
		return false;
	}


	// 为AcceptEx 准备参数，然后投递AcceptEx I/O请求
	for (int i = 0; i < MAX_POST_ACCEPT; i++)
	{
		// 新建一个IO_CONTEXT
		PER_IO_CONTEXT* pAcceptIoContext = m_pListenContext->GetNewIoContext();

		if (false == this->_PostAccept(pAcceptIoContext))
		{
			m_pListenContext->RemoveContext(pAcceptIoContext);
			return false;
		}
	}

	trace(_T("投递 %d 个AcceptEx请求完毕"), MAX_POST_ACCEPT);

	return true;

}

////////////////////////////////////////////////////////////
//	最后释放掉所有资源
void IOCPHeartBeatServer::_DeInitialize()
{
	// 关闭系统退出事件句柄
	RELEASE_HANDLE(m_hShutdownEvent);

	// 释放工作者线程句柄指针
	if (NULL != m_phWorkerThreads)
	{
		for (int i = 0; i < m_nThreads; i++)
		{
			RELEASE_HANDLE(m_phWorkerThreads[i]);
		}

		RELEASE(m_phWorkerThreads);
	}

	// 关闭IOCP句柄
	RELEASE_HANDLE(m_hIOCompletionPort);

	// 关闭监听Socket
	RELEASE(m_pListenContext);

}

////////////////////////////////////////////////////////////
// 在有客户端连入的时候，进行处理
// 流程有点复杂，你要是看不懂的话，就看配套的文档吧....
// 如果能理解这里的话，完成端口的机制你就消化了一大半了
// 总之你要知道，传入的是ListenSocket的Context，我们需要复制一份出来给新连入的Socket用
// 原来的Context还是要在上面继续投递下一个Accept请求
//
bool IOCPHeartBeatServer::_DoAccpet(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext, PER_SOCKET_CONTEXT* pNewClientSocketContext, PER_IO_CONTEXT* pNewClientIoContext)
{
	SOCKADDR_IN* ClientAddr = NULL;
	SOCKADDR_IN* LocalAddr = NULL;
	int remoteLen = sizeof(SOCKADDR_IN), localLen = sizeof(SOCKADDR_IN);

	///////////////////////////////////////////////////////////////////////////
	// 1. 首先取得连入客户端的地址信息
	// 这个 m_lpfnGetAcceptExSockAddrs 不得了啊~~~~~~
	// 不但可以取得客户端和本地端的地址信息，还能顺便取出客户端发来的第一组数据，老强大了...
	this->m_lpfnGetAcceptExSockAddrs(pIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.len - ((sizeof(SOCKADDR_IN) + 16) * 2),
		sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, (LPSOCKADDR*)&LocalAddr, &localLen, (LPSOCKADDR*)&ClientAddr, &remoteLen);

	// 信息输出
	Loger(LEVEL_INFOR, "客户端：%s:%d 连入！", inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port));

	//////////////////////////////////////////////////////////////////////////////////////////////////////
	// 2. 这里需要注意，这里传入的这个是ListenSocket上的Context，这个Context我们还需要用于监听下一个连接
	// 所以我还得要将ListenSocket上的Context复制出来一份为新连入的Socket新建一个SocketContext
	PER_SOCKET_CONTEXT* pNewSocketContext = new PER_SOCKET_CONTEXT;
	pNewSocketContext->m_Socket = pIoContext->m_sockAccept;
	memcpy(&(pNewSocketContext->m_ClientAddr), ClientAddr, sizeof(SOCKADDR_IN));

	// 参数设置完毕，将这个Socket和完成端口绑定(这也是一个关键步骤)
	if (false == this->_AssociateWithIOCP(pNewSocketContext))
	{
		RELEASE(pNewSocketContext);
		return false;
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// 3. 继续，建立其下的IoContext，用于在这个Socket上投递第一个数据请求
	PER_IO_CONTEXT* pNewIoContext = pNewSocketContext->GetNewIoContext();
	pNewIoContext->m_OpType = RECV_POSTED;
	pNewIoContext->m_sockAccept = pNewSocketContext->m_Socket;// 普通的两个socket值一样
	// 把接收到的数据copy在新的对象上
	memcpy(pNewIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.buf, pNewIoContext->m_wsaBuf.len);
	pNewClientIoContext = pNewIoContext;
	pNewClientSocketContext = pNewSocketContext;
	this->_DealRecvData(pNewClientSocketContext, pNewClientIoContext);

	/////////////////////////////////////////////////////////////////////////////////////////////////
	// 4. 如果投递成功，那么就把这个有效的客户端信息，加入到ContextList中去(需要统一管理，方便释放资源)
	this->_AddToContextList(pNewSocketContext);

	////////////////////////////////////////////////////////////////////////////////////////////////
	// 5. 使用完毕之后，把Listen Socket的那个IoContext重置，然后准备投递新的AcceptEx
	pIoContext->ResetBuffer();
	pIoContext->m_sockAccept = INVALID_SOCKET;// 不然会把新建立的socket关闭掉。
	return this->_PostAccept(pIoContext);
}

bool IOCPHeartBeatServer::_PostAccept(PER_IO_CONTEXT* pAcceptIoContext)
{
	assert(INVALID_SOCKET != m_pListenContext->m_Socket);

	// 准备参数
	DWORD dwBytes = 0;
	pAcceptIoContext->m_OpType = ACCEPT_POSTED;
	WSABUF *p_wbuf = &pAcceptIoContext->m_wsaBuf;
	OVERLAPPED *p_ol = &pAcceptIoContext->m_Overlapped;
	//客户端连接，未发送数据，直接关闭
	if (INVALID_SOCKET != pAcceptIoContext->m_sockAccept)
	{
		closesocket(pAcceptIoContext->m_sockAccept);
		pAcceptIoContext->m_sockAccept = INVALID_SOCKET;
	}
	// 为以后新连入的客户端先准备好Socket( 这个是与传统accept最大的区别 )
	pAcceptIoContext->m_sockAccept = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == pAcceptIoContext->m_sockAccept)
	{
		Loger(LEVEL_ERROR, perro("创建用于Accept的Socket失败！", WSAGetLastError()).c_str());
		return false;
	}
	// 投递AcceptEx
	if (FALSE == m_lpfnAcceptEx(m_pListenContext->m_Socket, pAcceptIoContext->m_sockAccept, p_wbuf->buf, p_wbuf->len - ((sizeof(SOCKADDR_IN) + 16) * 2),
		sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &dwBytes, p_ol))
	{
		if (WSA_IO_PENDING != WSAGetLastError())
		{
			Loger(LEVEL_ERROR, perro("投递 AcceptEx 请求失败！", WSAGetLastError()).c_str());
			return false;
		}
	}
	return true;
}

bool IOCPHeartBeatServer::_AssociateWithIOCP(PER_SOCKET_CONTEXT *pContext)
{
	// 将用于和客户端通信的SOCKET绑定到完成端口中
	HANDLE hTemp = CreateIoCompletionPort((HANDLE)pContext->m_Socket, m_hIOCompletionPort, (DWORD)pContext, 0);
	if (NULL == hTemp)
	{
		Loger(LEVEL_ERROR, perro("执行CreateIoCompletionPort()绑定客户端socket出错！", WSAGetLastError()).c_str());
		return false;
	}

	return true;
}

bool IOCPHeartBeatServer::_DealRecvData(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext)
{
	// 完成端口其他都是非阻塞的
	// 暂时还没找到完成端口如何实现心跳处理，先只对客户端发过来的心跳包回复。断开判断就通过HandError函数里的其他分支处理
	/*
	int nNetTimeout = (T1 + T2) * 1000; // 设置超时时间
	int nRet = setsockopt(pSocketContext->m_Socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&nNetTimeout, sizeof(nNetTimeout));
	if (SOCKET_ERROR == nRet)
	{
		Loger(LEVEL_ERROR, perro("设置接收超时时间失败", WSAGetLastError()).c_str());
		this->_RemoveContext(pSocketContext);
	}
	pSocketContext->missed_heartbeats = 0;
	*/

	// 读取收到消息类型
	HEADER _header;
	ZeroMemory(&_header, sizeof(_header));
	memmove(&_header, pIoContext->m_wsaBuf.buf, sizeof(_header));
	switch (_header.dwCmd)
	{
		// 心跳包
	case NET_CMD_HEART_BEAT:
	{
		Loger(LEVEL_INFOR, "recv heart beat packet and post return.");
		// 收到心跳包，回复
		if (false == this->_DoPostSend(pSocketContext, pIoContext, NET_CMD_HEART_BEAT, NULL, 0))
		{
			Loger(LEVEL_ERROR, "投递心跳回复包失败！");
			return false;
		}
	}
		break;
		// 数据包
	case NET_CMD_DATA:
	{
		char szBuf[MAX_BUFFER_LEN] = "hello client, I receive:";
		memcpy(szBuf + strlen(szBuf), pIoContext->m_wsaBuf.buf + sizeof(_header), _header.dwLen);
		// 收到数据包
		if (false == this->_DoPostSend(pSocketContext, pIoContext, NET_CMD_DATA, szBuf, strlen(szBuf)))
		{
			Loger(LEVEL_ERROR, "投递数据回复包失败！");
			return false;
		}
	}
		break;
	default:
		trace(("不支持的数据类型 %d.\n"), _header.dwCmd);
	}

	return false;
}

/////////////////////////////////////////////////////////////////
// 发送回复信息后，投递接收数据请求
bool IOCPHeartBeatServer::_DoPostRecv(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext)
{
	pIoContext->ResetBuffer();
	pIoContext->m_OpType = RECV_POSTED;
	// 然后开始投递下一个WSARecv请求
	return _PostRecv(pIoContext);
}

////////////////////////////////////////////////////////////////////
// 投递接收数据请求
bool IOCPHeartBeatServer::_PostRecv(PER_IO_CONTEXT* pIoContext)
{
	// 初始化变量
	DWORD dwFlags = 0;
	DWORD dwBytes = 0;
	WSABUF *p_wbuf = &pIoContext->m_wsaBuf;
	OVERLAPPED *p_ol = &pIoContext->m_Overlapped;

	// 初始化完成后，，投递WSARecv请求
	int nBytesRecv = WSARecv(pIoContext->m_sockAccept, p_wbuf, 1, &dwBytes, &dwFlags, p_ol, NULL);

	// 如果返回值错误，并且错误的代码并非是Pending的话，那就说明这个重叠请求失败了
	if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError()))
	{
		Loger(LEVEL_ERROR, perro("投递接收数据请求WSARecv失败！", WSAGetLastError()).c_str());
		return false;
	}
	return true;
}

bool IOCPHeartBeatServer::_DoPostSend(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext, uint32_t dwCmdID, LPVOID pDate, uint32_t dataLen)
{
	pIoContext->ResetBuffer();
	pIoContext->m_OpType = SEND_POSTED;
	// 然后开始投递下一个WSASend请求
	return _PostSend(pIoContext, dwCmdID, pDate, dataLen);
}

////////////////////////////////////////////////////////////////////
// 投递发送数据请求
bool IOCPHeartBeatServer::_PostSend(PER_IO_CONTEXT* pIoContext, uint32_t dwCmdID, LPVOID pDate, uint32_t dataLen)
{
	HEADER header = { 0 };
	header.dwCmd = dwCmdID;
	header.dwLen = dataLen;
	pIoContext->ResetBuffer();
	pIoContext->m_wsaBuf.len = sizeof(header);
	memmove(pIoContext->m_wsaBuf.buf, &header, pIoContext->m_wsaBuf.len);
	if (NULL != pDate)
		memmove(pIoContext->m_wsaBuf.buf + pIoContext->m_wsaBuf.len, pDate, dataLen);
	pIoContext->m_wsaBuf.len += dataLen;// 数据长度为头部 + 数据之和

	DWORD	flags = 0;		//标志
	DWORD	sendBytes = 0;	//发送字节数
	if (WSASend(pIoContext->m_sockAccept, &(pIoContext->m_wsaBuf), 1, &sendBytes, flags, &(pIoContext->m_Overlapped), NULL) == SOCKET_ERROR)
	{
		if (ERROR_IO_PENDING != WSAGetLastError())//发起重叠操作失败
		{
			Loger(LEVEL_ERROR, perro("投递发送数据请求WSASend", WSAGetLastError()).c_str())
		}
		return false;
	}
	return true;
}

//////////////////////////////////////////////////////////////
// 将客户端的相关信息存储到数组中
void IOCPHeartBeatServer::_AddToContextList(PER_SOCKET_CONTEXT *pSocketContext)
{
	EnterCriticalSection(&m_csClientSockContext);

	m_vectorClientSockContext.push_back(pSocketContext);

	LeaveCriticalSection(&m_csClientSockContext);
}

////////////////////////////////////////////////////////////////
//	移除某个特定的Context
void IOCPHeartBeatServer::_RemoveContext(PER_SOCKET_CONTEXT *pSocketContext)
{
	EnterCriticalSection(&m_csClientSockContext);
	for (int i = 0; i < m_vectorClientSockContext.size(); i++)
	{
		if (pSocketContext == m_vectorClientSockContext.at(i))
		{
			Loger(LEVEL_INFOR, "关闭客户端：%s:%d 连接.", inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), ntohs(pSocketContext->m_ClientAddr.sin_port));
			RELEASE(pSocketContext);
			std::vector<PER_SOCKET_CONTEXT*>::iterator it = m_vectorClientSockContext.begin() + i;
			m_vectorClientSockContext.erase(it);
			break;
		}
	}
	LeaveCriticalSection(&m_csClientSockContext);
}

////////////////////////////////////////////////////////////////
// 清空客户端信息
void IOCPHeartBeatServer::_ClearContextList()
{
	EnterCriticalSection(&m_csClientSockContext);

	for (int i = 0; i < m_vectorClientSockContext.size(); i++)
	{
		delete m_vectorClientSockContext.at(i);
	}

	m_vectorClientSockContext.clear();

	LeaveCriticalSection(&m_csClientSockContext);
}

bool IOCPHeartBeatServer::HandleError(PER_SOCKET_CONTEXT *pSockContext, PER_IO_CONTEXT* pIoContext, const DWORD& dwErr)
{
	// 如果是超时了，就再继续等吧  
	if (WAIT_TIMEOUT == dwErr)
	{
		// 完成端口都是非阻塞的，所以这个分支基本上不会进入
		// 暂时还没找到完成端口如何实现心跳处理，先只对客户端发过来的心跳包回复
		int nNetTimeout = (T2) * 1000; // 设置超时时间
		int nRet = setsockopt(pSockContext->m_Socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&nNetTimeout, sizeof(nNetTimeout));
		if (SOCKET_ERROR == nRet)
		{
			Loger(LEVEL_ERROR, perro("设置接收超时时间失败", WSAGetLastError()).c_str());
			this->_RemoveContext(pSockContext);
		}
		if (++pSockContext->missed_heartbeats > 3)
		{
			Loger(LEVEL_INFOR, "客户端：%s:%d 心跳检测.", inet_ntoa(pSockContext->m_ClientAddr.sin_addr), ntohs(pSockContext->m_ClientAddr.sin_port));
			this->_RemoveContext(pSockContext);
		}
		this->_DoPostRecv(pSockContext, pIoContext);
	}
	// 可能是客户端异常退出了
	else if (ERROR_NETNAME_DELETED == dwErr)
	{
		// 客户端关闭，还没有发数据就断开了
		Loger(LEVEL_ERROR, "客户端：%s:%d 异常退出. %s", inet_ntoa(pSockContext->m_ClientAddr.sin_addr), ntohs(pSockContext->m_ClientAddr.sin_port), perro("", WSAGetLastError()).c_str());
		if (pSockContext->m_Socket == m_pListenContext->m_Socket)
		{
			this->_PostAccept(pIoContext);
		}
		this->_RemoveContext(pSockContext);
		return true;
	}

	else
	{
		// 网线等硬件断开
		Loger(LEVEL_ERROR, "客户端：%s:%d .完成端口操作出现错误，线程退出. %s", inet_ntoa(pSockContext->m_ClientAddr.sin_addr), ntohs(pSockContext->m_ClientAddr.sin_port), perro("", WSAGetLastError()).c_str());
		this->_RemoveContext(pSockContext);
		return false;
	}

}

/////////////////////////////////////////////////////////////////////
// 判断客户端Socket是否已经断开，否则在一个无效的Socket上投递WSARecv操作会出现异常
// 使用的方法是尝试向这个socket发送数据，判断这个socket调用的返回值
// 因为如果客户端网络异常断开(例如客户端崩溃或者拔掉网线等)的时候，服务器端是无法收到客户端断开的通知的
bool IOCPHeartBeatServer::_IsSocketAlive(SOCKET s)
{
	int nByteSent = send(s, "", 0, 0);
	if (-1 == nByteSent) return false;
	return true;
}

///////////////////////////////////////////////////////////////////
// 获得本机中处理器的数量
int IOCPHeartBeatServer::_GetNoOfProcessors()
{
	SYSTEM_INFO si;

	GetSystemInfo(&si);

	return si.dwNumberOfProcessors;

}

// 读取配置信息
bool IOCPHeartBeatServer::ReadConfig()
{
	const uint32_t nLen = 128;
	char szBuf[nLen] = { 0 };
	char szConfigPath[nLen] = { 0 };
	// 检查配置文件是否存在
	int _res = SearchPath(".\\", "UpdateConfigServer.ini", NULL, nLen, szConfigPath, NULL);
	if (_res == 0)
	{
		Loger(LEVEL_ERROR, perro("没有找到配置文件：UpdateConfigServer.ini", WSAGetLastError()).c_str());
		return false;
	}
	// 读取ip地址和端口号
	GetPrivateProfileString("TCP\\IP", "IP", DEFAULT_IP, szBuf, nLen, szConfigPath);
	m_strIP = szBuf;
	GetPrivateProfileString("TCP\\IP", "PORT", DEFAULT_PORT, szBuf, nLen, szConfigPath);
	m_nPort = atoi(szBuf);
	return true;
}
