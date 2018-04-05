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
// ÿһ���������ϲ������ٸ��߳�(Ϊ������޶ȵ��������������ܣ���������ĵ�)
#define WORKER_THREADS_PER_PROCESSOR 2
// ͬʱͶ�ݵ�Accept���������(���Ҫ����ʵ�ʵ�����������)
#define MAX_POST_ACCEPT              8
// ���ݸ�Worker�̵߳��˳��ź�
#define EXIT_CODE                    NULL

// Ĭ�϶˿�
#define DEFAULT_PORT          "12345"
// Ĭ��IP��ַ
#define DEFAULT_IP            _T("127.0.0.1")

// �ͷ�ָ��;����Դ�ĺ�
// �ͷ�ָ���
#define RELEASE(x)                      {if(x != NULL ){delete x;x=NULL;}}
// �ͷž����
#define RELEASE_HANDLE(x)               {if(x != NULL && x!=INVALID_HANDLE_VALUE){ CloseHandle(x);x = NULL;}}
// �ͷ�Socket��
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
	// ��ʼ���߳��ٽ���
	InitializeCriticalSection(&m_csClientSockContext);
	Log::getInstance().setLogLevel(LEVEL_INFOR);
}


IOCPHeartBeatServer::~IOCPHeartBeatServer()
{
	Stop();
	// ɾ���ͻ����б��ٽ���
	DeleteCriticalSection(&m_csClientSockContext);
	Log::getInstance().removeInstance();
}

bool IOCPHeartBeatServer::LoadSockLib()
{
	WSADATA wsaData;
	int nResult;
	nResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	// ����(һ�㶼�����ܳ���)
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
	// ��ȡ������Ϣ
	if (false == ReadConfig())
	{
		return false;
	}

	// ����ϵͳ�˳����¼�֪ͨ
	m_hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (NULL == m_hShutdownEvent)
	{
		Loger(LEVEL_ERROR, perro("��ʼ���˳�ʱ��ʧ��.", WSAGetLastError()).c_str());
		return false;
	}

	// ��ʼ��IOCP
	if (false == _InitializeIOCP())
	{
		Loger(LEVEL_ERROR, perro("��ʼ��IOCPʧ��.", WSAGetLastError()).c_str());
		return false;
	}
	else
	{
		Loger(LEVEL_INFOR, "��ʼ��IOCP���.");
	}

	// ��ʼ��Socket
	if (false == _InitializeListenSocket())
	{
		Loger(LEVEL_ERROR, perro("_InitializeListenSocket", WSAGetLastError()).c_str());
		Stop();
		return false;
	}
	else
	{
		Loger(LEVEL_INFOR, "Listen Socket��ʼ�����.");
	}

	//this->_ShowMessage(_T("ϵͳ׼���������Ⱥ�����....\n"));
	return false;
}

void IOCPHeartBeatServer::Stop()
{
	// ����ر���Ϣ֪ͨ
	if (NULL != m_hShutdownEvent && INVALID_HANDLE_VALUE != m_hShutdownEvent)
		SetEvent(m_hShutdownEvent);

	if (NULL != m_phWorkerThreads)
	{
		for (int i = 0; i < m_nThreads; i++)
		{
			// ֪ͨ���е���ɶ˿ڲ����˳�
			PostQueuedCompletionStatus(m_hIOCompletionPort, 0, (DWORD)EXIT_CODE, NULL);
		}
		// �ȴ����еĿͻ�����Դ�˳�
		WaitForMultipleObjects(m_nThreads, m_phWorkerThreads, TRUE, INFINITE);
	}

	// ����ͻ����б���Ϣ
	this->_ClearContextList();

	// �ͷ�������Դ
	this->_DeInitialize();
}

string IOCPHeartBeatServer::GetLocalIP()
{
	// ��ñ���������
	char hostname[MAX_PATH] = { 0 };
	gethostname(hostname, MAX_PATH);
	struct hostent FAR* lpHostEnt = gethostbyname(hostname);
	if (lpHostEnt == NULL)
	{
		return DEFAULT_IP;
	}

	// ȡ��IP��ַ�б��еĵ�һ��Ϊ���ص�IP(��Ϊһ̨�������ܻ�󶨶��IP)
	LPSTR lpAddr = lpHostEnt->h_addr_list[0];

	// ��IP��ַת�����ַ�����ʽ
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

	trace(_T("�������߳�������ID: %d.\n"), nThreadNo);

	OVERLAPPED           *pOverlapped = NULL;
	PER_SOCKET_CONTEXT   *pSocketContext = NULL;
	DWORD                dwBytesTransfered = 0;

	// ѭ����������֪�����յ�Shutdown��ϢΪֹ
	while (WAIT_OBJECT_0 != WaitForSingleObject(pIOCPServer->m_hShutdownEvent, 0))
	{
		BOOL bReturn = GetQueuedCompletionStatus(
			pIOCPServer->m_hIOCompletionPort,
			&dwBytesTransfered,
			(PULONG_PTR)&pSocketContext,
			&pOverlapped,
			INFINITE);

		// ����յ������˳���־����ֱ���˳�
		if (EXIT_CODE == (DWORD)pSocketContext)
		{
			Loger(LEVEL_ERROR, perro("���ճ�ʱ", WSAGetLastError()).c_str());
			break;
		}

		// �ж��Ƿ�����˴���
		if (!bReturn)
		{
			DWORD dwErr = GetLastError();
			// ��ȡ����Ĳ���
			PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);
			// ��ʾһ����ʾ��Ϣ
			if (!pIOCPServer->HandleError(pSocketContext, pIoContext, dwErr))
			{
				break;
			}

			continue;
		}
		else
		{
			// ��ȡ����Ĳ���
			PER_IO_CONTEXT* pIoContext = CONTAINING_RECORD(pOverlapped, PER_IO_CONTEXT, m_Overlapped);
			// �ж��Ƿ��пͻ��˶Ͽ���
			if ((0 == dwBytesTransfered) 
				&& (ACCEPT_POSTED == pIoContext->m_OpType 
				|| SEND_POSTED == pIoContext->m_OpType
				|| RECV_POSTED == pIoContext->m_OpType))
			{
				Loger(LEVEL_INFOR, "�ͻ��ˣ�%s:%d �Ͽ����ӣ�", inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), ntohs(pSocketContext->m_ClientAddr.sin_port));
				// �ͷŵ���Ӧ����Դ
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
					// Ϊ�����Ӵ���ɶ��ԣ�������ר�ŵ�_DoAccept�������д�����������,���һ�ȡ��һ������ͷ��
					PER_IO_CONTEXT* pNewClientIoContext = NULL;
					PER_SOCKET_CONTEXT* pNewClientSocketContext = NULL;
					if (true == pIOCPServer->_DoAccpet(pSocketContext, pIoContext, pNewClientSocketContext, pNewClientIoContext))
					{
						// ��֪��Ϊʲôȡ��pNewClientIoContext ��pNewClientSocketContext ��ֵ
						// ���Դ�����յ�һ�����ݵĴ����Ƶ�_DoAccpet�С�
						// �����һ������
						//pIOCPServer->_DealRecvData(pNewClientSocketContext, pNewClientIoContext);
					}
				}
				break;
				// SEND
				case SEND_POSTED:
				{
					// ����������ɺ�Ͷ�ݽ������ݵ�����
					pIOCPServer->_DoPostRecv(pSocketContext, pIoContext);
				}
				break;
				// RECV
				case RECV_POSTED:
				{
					// Ͷ���հ汾��Ϣ������,Ͷ�������ʱ��������ر�socket
					pIOCPServer->_DealRecvData(pSocketContext, pIoContext);
				}
				break;
				default:
					// ��Ӧ��ִ�е�����
					trace(_T("_WorkThread�е� pIoContext->m_OpType �����쳣.\n"));
					break;
				} //switch
			}//if
		}//if

	}//while

	trace(_T("�������߳� %d ���˳�.\n"), nThreadNo);

	// �ͷ��̲߳���
	RELEASE(lpParam);

	return 0;
}

////////////////////////////////
// ��ʼ����ɶ˿�
bool IOCPHeartBeatServer::_InitializeIOCP()
{
	// ������һ����ɶ˿�
	m_hIOCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (NULL == m_hIOCompletionPort)
	{
		Loger(LEVEL_ERROR, perro("CreateIoCompletionPort", WSAGetLastError()).c_str());
		return false;
	}

	// ���ݱ����еĴ�����������������Ӧ���߳���
	m_nThreads = WORKER_THREADS_PER_PROCESSOR * _GetNoOfProcessors();

	// Ϊ�������̳߳�ʼ�����
	m_phWorkerThreads = new HANDLE[m_nThreads];

	// ���ݼ�����������������������߳�
	DWORD nThreadID;
	for (int i = 0; i < m_nThreads; i++)
	{
		PTHREADPARAMS pThreadParams = new THREADPARAMS;
		pThreadParams->pIOCPServer = this;
		pThreadParams->nThreadNo = i + 1;
		m_phWorkerThreads[i] = ::CreateThread(0, 0, _WorkerThread, (void *)pThreadParams, 0, &nThreadID);
	}

	trace(" ���� _WorkerThread %d ��.\n", m_nThreads);
	return true;
}


/////////////////////////////////////////////////////////////////
// ��ʼ��Socket
bool IOCPHeartBeatServer::_InitializeListenSocket()
{
	// AcceptEx �� GetAcceptExSockaddrs ��GUID�����ڵ�������ָ��
	GUID GuidAcceptEx = WSAID_ACCEPTEX;
	GUID GuidGetAcceptExSockAddrs = WSAID_GETACCEPTEXSOCKADDRS;
	GUID GuidTransmitFile = WSAID_TRANSMITFILE;

	// ��������ַ��Ϣ�����ڰ�Socket
	struct sockaddr_in ServerAddress;

	// �������ڼ�����Socket����Ϣ
	m_pListenContext = new PER_SOCKET_CONTEXT;

	// ��Ҫʹ���ص�IO�������ʹ��WSASocket������Socket���ſ���֧���ص�IO����
	m_pListenContext->m_Socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == m_pListenContext->m_Socket)
	{
		Loger(LEVEL_ERROR, perro("��ʼ��socketʧ�ܣ�������룡", WSAGetLastError()).c_str());
		return false;
	}
	else
	{
		trace("WSASocket() ���.\n");
	}

	// ��Listen Socket������ɶ˿���
	if (NULL == CreateIoCompletionPort((HANDLE)m_pListenContext->m_Socket, m_hIOCompletionPort, (DWORD)m_pListenContext, 0))
	{
		Loger(LEVEL_ERROR, perro("�� Listen Socket����ɶ˿�ʧ�ܣ�", WSAGetLastError()).c_str());
		RELEASE_SOCKET(m_pListenContext->m_Socket);
		return false;
	}
	else
	{
		trace("Listen Socket����ɶ˿� ���.\n");
	}

	// ����ַ��Ϣ
	ZeroMemory((char *)&ServerAddress, sizeof(ServerAddress));
	ServerAddress.sin_family = AF_INET;
	// ������԰��κο��õ�IP��ַ�����߰�һ��ָ����IP��ַ 
	//ServerAddress.sin_addr.s_addr = htonl(INADDR_ANY);                      
	ServerAddress.sin_addr.s_addr = inet_addr(m_strIP.c_str());
	ServerAddress.sin_port = htons(m_nPort);

	// �󶨵�ַ�Ͷ˿�
	if (SOCKET_ERROR == ::bind(m_pListenContext->m_Socket, (struct sockaddr *) &ServerAddress, sizeof(ServerAddress)))
	{
		Loger(LEVEL_ERROR, perro("bind����ִ�д���", WSAGetLastError()).c_str());
		return false;
	}
	else
	{
		trace("bind() ���.\n");
	}

	// ��ʼ���м���
	if (SOCKET_ERROR == listen(m_pListenContext->m_Socket, SOMAXCONN))
	{
		Loger(LEVEL_ERROR, perro("listen����ִ�д���", WSAGetLastError()).c_str());
		return false;
	}
	else
	{
		trace("Listen() ���.\n");
	}

	// ʹ��AcceptEx��������Ϊ���������WinSock2�淶֮���΢�������ṩ����չ����
	// ������Ҫ�����ȡһ�º�����ָ�룬
	// ��ȡAcceptEx����ָ��
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
		Loger(LEVEL_ERROR, perro("WSAIoctl δ�ܻ�ȡAcceptEx����ָ�룡", WSAGetLastError()).c_str());
		return false;
	}

	// ��ȡGetAcceptExSockAddrs����ָ�룬Ҳ��ͬ��
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
		Loger(LEVEL_ERROR, perro("WSAIoctl δ�ܻ�ȡGetAcceptExSockAddrs����ָ�룡", WSAGetLastError()).c_str());
		return false;
	}

	// ��ȡTransmitFile����ָ�룬Ҳ��ͬ��
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
		Loger(LEVEL_ERROR, perro("WSAIoctl δ�ܻ�ȡTransmitFile����ָ�룡", WSAGetLastError()).c_str());
		return false;
	}


	// ΪAcceptEx ׼��������Ȼ��Ͷ��AcceptEx I/O����
	for (int i = 0; i < MAX_POST_ACCEPT; i++)
	{
		// �½�һ��IO_CONTEXT
		PER_IO_CONTEXT* pAcceptIoContext = m_pListenContext->GetNewIoContext();

		if (false == this->_PostAccept(pAcceptIoContext))
		{
			m_pListenContext->RemoveContext(pAcceptIoContext);
			return false;
		}
	}

	trace(_T("Ͷ�� %d ��AcceptEx�������"), MAX_POST_ACCEPT);

	return true;

}

////////////////////////////////////////////////////////////
//	����ͷŵ�������Դ
void IOCPHeartBeatServer::_DeInitialize()
{
	// �ر�ϵͳ�˳��¼����
	RELEASE_HANDLE(m_hShutdownEvent);

	// �ͷŹ������߳̾��ָ��
	if (NULL != m_phWorkerThreads)
	{
		for (int i = 0; i < m_nThreads; i++)
		{
			RELEASE_HANDLE(m_phWorkerThreads[i]);
		}

		RELEASE(m_phWorkerThreads);
	}

	// �ر�IOCP���
	RELEASE_HANDLE(m_hIOCompletionPort);

	// �رռ���Socket
	RELEASE(m_pListenContext);

}

////////////////////////////////////////////////////////////
// ���пͻ��������ʱ�򣬽��д���
// �����е㸴�ӣ���Ҫ�ǿ������Ļ����Ϳ����׵��ĵ���....
// ������������Ļ�����ɶ˿ڵĻ������������һ�����
// ��֮��Ҫ֪�����������ListenSocket��Context��������Ҫ����һ�ݳ������������Socket��
// ԭ����Context����Ҫ���������Ͷ����һ��Accept����
//
bool IOCPHeartBeatServer::_DoAccpet(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext, PER_SOCKET_CONTEXT* pNewClientSocketContext, PER_IO_CONTEXT* pNewClientIoContext)
{
	SOCKADDR_IN* ClientAddr = NULL;
	SOCKADDR_IN* LocalAddr = NULL;
	int remoteLen = sizeof(SOCKADDR_IN), localLen = sizeof(SOCKADDR_IN);

	///////////////////////////////////////////////////////////////////////////
	// 1. ����ȡ������ͻ��˵ĵ�ַ��Ϣ
	// ��� m_lpfnGetAcceptExSockAddrs �����˰�~~~~~~
	// ��������ȡ�ÿͻ��˺ͱ��ض˵ĵ�ַ��Ϣ������˳��ȡ���ͻ��˷����ĵ�һ�����ݣ���ǿ����...
	this->m_lpfnGetAcceptExSockAddrs(pIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.len - ((sizeof(SOCKADDR_IN) + 16) * 2),
		sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, (LPSOCKADDR*)&LocalAddr, &localLen, (LPSOCKADDR*)&ClientAddr, &remoteLen);

	// ��Ϣ���
	Loger(LEVEL_INFOR, "�ͻ��ˣ�%s:%d ���룡", inet_ntoa(ClientAddr->sin_addr), ntohs(ClientAddr->sin_port));

	//////////////////////////////////////////////////////////////////////////////////////////////////////
	// 2. ������Ҫע�⣬���ﴫ��������ListenSocket�ϵ�Context�����Context���ǻ���Ҫ���ڼ�����һ������
	// �����һ���Ҫ��ListenSocket�ϵ�Context���Ƴ���һ��Ϊ�������Socket�½�һ��SocketContext
	PER_SOCKET_CONTEXT* pNewSocketContext = new PER_SOCKET_CONTEXT;
	pNewSocketContext->m_Socket = pIoContext->m_sockAccept;
	memcpy(&(pNewSocketContext->m_ClientAddr), ClientAddr, sizeof(SOCKADDR_IN));

	// ����������ϣ������Socket����ɶ˿ڰ�(��Ҳ��һ���ؼ�����)
	if (false == this->_AssociateWithIOCP(pNewSocketContext))
	{
		RELEASE(pNewSocketContext);
		return false;
	}

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// 3. �������������µ�IoContext�����������Socket��Ͷ�ݵ�һ����������
	PER_IO_CONTEXT* pNewIoContext = pNewSocketContext->GetNewIoContext();
	pNewIoContext->m_OpType = RECV_POSTED;
	pNewIoContext->m_sockAccept = pNewSocketContext->m_Socket;// ��ͨ������socketֵһ��
	// �ѽ��յ�������copy���µĶ�����
	memcpy(pNewIoContext->m_wsaBuf.buf, pIoContext->m_wsaBuf.buf, pNewIoContext->m_wsaBuf.len);
	pNewClientIoContext = pNewIoContext;
	pNewClientSocketContext = pNewSocketContext;
	this->_DealRecvData(pNewClientSocketContext, pNewClientIoContext);

	/////////////////////////////////////////////////////////////////////////////////////////////////
	// 4. ���Ͷ�ݳɹ�����ô�Ͱ������Ч�Ŀͻ�����Ϣ�����뵽ContextList��ȥ(��Ҫͳһ���������ͷ���Դ)
	this->_AddToContextList(pNewSocketContext);

	////////////////////////////////////////////////////////////////////////////////////////////////
	// 5. ʹ�����֮�󣬰�Listen Socket���Ǹ�IoContext���ã�Ȼ��׼��Ͷ���µ�AcceptEx
	pIoContext->ResetBuffer();
	pIoContext->m_sockAccept = INVALID_SOCKET;// ��Ȼ����½�����socket�رյ���
	return this->_PostAccept(pIoContext);
}

bool IOCPHeartBeatServer::_PostAccept(PER_IO_CONTEXT* pAcceptIoContext)
{
	assert(INVALID_SOCKET != m_pListenContext->m_Socket);

	// ׼������
	DWORD dwBytes = 0;
	pAcceptIoContext->m_OpType = ACCEPT_POSTED;
	WSABUF *p_wbuf = &pAcceptIoContext->m_wsaBuf;
	OVERLAPPED *p_ol = &pAcceptIoContext->m_Overlapped;
	//�ͻ������ӣ�δ�������ݣ�ֱ�ӹر�
	if (INVALID_SOCKET != pAcceptIoContext->m_sockAccept)
	{
		closesocket(pAcceptIoContext->m_sockAccept);
		pAcceptIoContext->m_sockAccept = INVALID_SOCKET;
	}
	// Ϊ�Ժ�������Ŀͻ�����׼����Socket( ������봫ͳaccept�������� )
	pAcceptIoContext->m_sockAccept = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == pAcceptIoContext->m_sockAccept)
	{
		Loger(LEVEL_ERROR, perro("��������Accept��Socketʧ�ܣ�", WSAGetLastError()).c_str());
		return false;
	}
	// Ͷ��AcceptEx
	if (FALSE == m_lpfnAcceptEx(m_pListenContext->m_Socket, pAcceptIoContext->m_sockAccept, p_wbuf->buf, p_wbuf->len - ((sizeof(SOCKADDR_IN) + 16) * 2),
		sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &dwBytes, p_ol))
	{
		if (WSA_IO_PENDING != WSAGetLastError())
		{
			Loger(LEVEL_ERROR, perro("Ͷ�� AcceptEx ����ʧ�ܣ�", WSAGetLastError()).c_str());
			return false;
		}
	}
	return true;
}

bool IOCPHeartBeatServer::_AssociateWithIOCP(PER_SOCKET_CONTEXT *pContext)
{
	// �����ںͿͻ���ͨ�ŵ�SOCKET�󶨵���ɶ˿���
	HANDLE hTemp = CreateIoCompletionPort((HANDLE)pContext->m_Socket, m_hIOCompletionPort, (DWORD)pContext, 0);
	if (NULL == hTemp)
	{
		Loger(LEVEL_ERROR, perro("ִ��CreateIoCompletionPort()�󶨿ͻ���socket����", WSAGetLastError()).c_str());
		return false;
	}

	return true;
}

bool IOCPHeartBeatServer::_DealRecvData(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext)
{
	// ��ɶ˿��������Ƿ�������
	// ��ʱ��û�ҵ���ɶ˿����ʵ������������ֻ�Կͻ��˷��������������ظ����Ͽ��жϾ�ͨ��HandError�������������֧����
	/*
	int nNetTimeout = (T1 + T2) * 1000; // ���ó�ʱʱ��
	int nRet = setsockopt(pSocketContext->m_Socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&nNetTimeout, sizeof(nNetTimeout));
	if (SOCKET_ERROR == nRet)
	{
		Loger(LEVEL_ERROR, perro("���ý��ճ�ʱʱ��ʧ��", WSAGetLastError()).c_str());
		this->_RemoveContext(pSocketContext);
	}
	pSocketContext->missed_heartbeats = 0;
	*/

	// ��ȡ�յ���Ϣ����
	HEADER _header;
	ZeroMemory(&_header, sizeof(_header));
	memmove(&_header, pIoContext->m_wsaBuf.buf, sizeof(_header));
	switch (_header.dwCmd)
	{
		// ������
	case NET_CMD_HEART_BEAT:
	{
		Loger(LEVEL_INFOR, "recv heart beat packet and post return.");
		// �յ����������ظ�
		if (false == this->_DoPostSend(pSocketContext, pIoContext, NET_CMD_HEART_BEAT, NULL, 0))
		{
			Loger(LEVEL_ERROR, "Ͷ�������ظ���ʧ�ܣ�");
			return false;
		}
	}
		break;
		// ���ݰ�
	case NET_CMD_DATA:
	{
		char szBuf[MAX_BUFFER_LEN] = "hello client, I receive:";
		memcpy(szBuf + strlen(szBuf), pIoContext->m_wsaBuf.buf + sizeof(_header), _header.dwLen);
		// �յ����ݰ�
		if (false == this->_DoPostSend(pSocketContext, pIoContext, NET_CMD_DATA, szBuf, strlen(szBuf)))
		{
			Loger(LEVEL_ERROR, "Ͷ�����ݻظ���ʧ�ܣ�");
			return false;
		}
	}
		break;
	default:
		trace(("��֧�ֵ��������� %d.\n"), _header.dwCmd);
	}

	return false;
}

/////////////////////////////////////////////////////////////////
// ���ͻظ���Ϣ��Ͷ�ݽ�����������
bool IOCPHeartBeatServer::_DoPostRecv(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext)
{
	pIoContext->ResetBuffer();
	pIoContext->m_OpType = RECV_POSTED;
	// Ȼ��ʼͶ����һ��WSARecv����
	return _PostRecv(pIoContext);
}

////////////////////////////////////////////////////////////////////
// Ͷ�ݽ�����������
bool IOCPHeartBeatServer::_PostRecv(PER_IO_CONTEXT* pIoContext)
{
	// ��ʼ������
	DWORD dwFlags = 0;
	DWORD dwBytes = 0;
	WSABUF *p_wbuf = &pIoContext->m_wsaBuf;
	OVERLAPPED *p_ol = &pIoContext->m_Overlapped;

	// ��ʼ����ɺ󣬣�Ͷ��WSARecv����
	int nBytesRecv = WSARecv(pIoContext->m_sockAccept, p_wbuf, 1, &dwBytes, &dwFlags, p_ol, NULL);

	// �������ֵ���󣬲��Ҵ���Ĵ��벢����Pending�Ļ����Ǿ�˵������ص�����ʧ����
	if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError()))
	{
		Loger(LEVEL_ERROR, perro("Ͷ�ݽ�����������WSARecvʧ�ܣ�", WSAGetLastError()).c_str());
		return false;
	}
	return true;
}

bool IOCPHeartBeatServer::_DoPostSend(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext, uint32_t dwCmdID, LPVOID pDate, uint32_t dataLen)
{
	pIoContext->ResetBuffer();
	pIoContext->m_OpType = SEND_POSTED;
	// Ȼ��ʼͶ����һ��WSASend����
	return _PostSend(pIoContext, dwCmdID, pDate, dataLen);
}

////////////////////////////////////////////////////////////////////
// Ͷ�ݷ�����������
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
	pIoContext->m_wsaBuf.len += dataLen;// ���ݳ���Ϊͷ�� + ����֮��

	DWORD	flags = 0;		//��־
	DWORD	sendBytes = 0;	//�����ֽ���
	if (WSASend(pIoContext->m_sockAccept, &(pIoContext->m_wsaBuf), 1, &sendBytes, flags, &(pIoContext->m_Overlapped), NULL) == SOCKET_ERROR)
	{
		if (ERROR_IO_PENDING != WSAGetLastError())//�����ص�����ʧ��
		{
			Loger(LEVEL_ERROR, perro("Ͷ�ݷ�����������WSASend", WSAGetLastError()).c_str())
		}
		return false;
	}
	return true;
}

//////////////////////////////////////////////////////////////
// ���ͻ��˵������Ϣ�洢��������
void IOCPHeartBeatServer::_AddToContextList(PER_SOCKET_CONTEXT *pSocketContext)
{
	EnterCriticalSection(&m_csClientSockContext);

	m_vectorClientSockContext.push_back(pSocketContext);

	LeaveCriticalSection(&m_csClientSockContext);
}

////////////////////////////////////////////////////////////////
//	�Ƴ�ĳ���ض���Context
void IOCPHeartBeatServer::_RemoveContext(PER_SOCKET_CONTEXT *pSocketContext)
{
	EnterCriticalSection(&m_csClientSockContext);
	for (int i = 0; i < m_vectorClientSockContext.size(); i++)
	{
		if (pSocketContext == m_vectorClientSockContext.at(i))
		{
			Loger(LEVEL_INFOR, "�رտͻ��ˣ�%s:%d ����.", inet_ntoa(pSocketContext->m_ClientAddr.sin_addr), ntohs(pSocketContext->m_ClientAddr.sin_port));
			RELEASE(pSocketContext);
			std::vector<PER_SOCKET_CONTEXT*>::iterator it = m_vectorClientSockContext.begin() + i;
			m_vectorClientSockContext.erase(it);
			break;
		}
	}
	LeaveCriticalSection(&m_csClientSockContext);
}

////////////////////////////////////////////////////////////////
// ��տͻ�����Ϣ
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
	// ����ǳ�ʱ�ˣ����ټ����Ȱ�  
	if (WAIT_TIMEOUT == dwErr)
	{
		// ��ɶ˿ڶ��Ƿ������ģ����������֧�����ϲ������
		// ��ʱ��û�ҵ���ɶ˿����ʵ������������ֻ�Կͻ��˷��������������ظ�
		int nNetTimeout = (T2) * 1000; // ���ó�ʱʱ��
		int nRet = setsockopt(pSockContext->m_Socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&nNetTimeout, sizeof(nNetTimeout));
		if (SOCKET_ERROR == nRet)
		{
			Loger(LEVEL_ERROR, perro("���ý��ճ�ʱʱ��ʧ��", WSAGetLastError()).c_str());
			this->_RemoveContext(pSockContext);
		}
		if (++pSockContext->missed_heartbeats > 3)
		{
			Loger(LEVEL_INFOR, "�ͻ��ˣ�%s:%d �������.", inet_ntoa(pSockContext->m_ClientAddr.sin_addr), ntohs(pSockContext->m_ClientAddr.sin_port));
			this->_RemoveContext(pSockContext);
		}
		this->_DoPostRecv(pSockContext, pIoContext);
	}
	// �����ǿͻ����쳣�˳���
	else if (ERROR_NETNAME_DELETED == dwErr)
	{
		// �ͻ��˹رգ���û�з����ݾͶϿ���
		Loger(LEVEL_ERROR, "�ͻ��ˣ�%s:%d �쳣�˳�. %s", inet_ntoa(pSockContext->m_ClientAddr.sin_addr), ntohs(pSockContext->m_ClientAddr.sin_port), perro("", WSAGetLastError()).c_str());
		if (pSockContext->m_Socket == m_pListenContext->m_Socket)
		{
			this->_PostAccept(pIoContext);
		}
		this->_RemoveContext(pSockContext);
		return true;
	}

	else
	{
		// ���ߵ�Ӳ���Ͽ�
		Loger(LEVEL_ERROR, "�ͻ��ˣ�%s:%d .��ɶ˿ڲ������ִ����߳��˳�. %s", inet_ntoa(pSockContext->m_ClientAddr.sin_addr), ntohs(pSockContext->m_ClientAddr.sin_port), perro("", WSAGetLastError()).c_str());
		this->_RemoveContext(pSockContext);
		return false;
	}

}

/////////////////////////////////////////////////////////////////////
// �жϿͻ���Socket�Ƿ��Ѿ��Ͽ���������һ����Ч��Socket��Ͷ��WSARecv����������쳣
// ʹ�õķ����ǳ��������socket�������ݣ��ж����socket���õķ���ֵ
// ��Ϊ����ͻ��������쳣�Ͽ�(����ͻ��˱������߰ε����ߵ�)��ʱ�򣬷����������޷��յ��ͻ��˶Ͽ���֪ͨ��
bool IOCPHeartBeatServer::_IsSocketAlive(SOCKET s)
{
	int nByteSent = send(s, "", 0, 0);
	if (-1 == nByteSent) return false;
	return true;
}

///////////////////////////////////////////////////////////////////
// ��ñ����д�����������
int IOCPHeartBeatServer::_GetNoOfProcessors()
{
	SYSTEM_INFO si;

	GetSystemInfo(&si);

	return si.dwNumberOfProcessors;

}

// ��ȡ������Ϣ
bool IOCPHeartBeatServer::ReadConfig()
{
	const uint32_t nLen = 128;
	char szBuf[nLen] = { 0 };
	char szConfigPath[nLen] = { 0 };
	// ��������ļ��Ƿ����
	int _res = SearchPath(".\\", "UpdateConfigServer.ini", NULL, nLen, szConfigPath, NULL);
	if (_res == 0)
	{
		Loger(LEVEL_ERROR, perro("û���ҵ������ļ���UpdateConfigServer.ini", WSAGetLastError()).c_str());
		return false;
	}
	// ��ȡip��ַ�Ͷ˿ں�
	GetPrivateProfileString("TCP\\IP", "IP", DEFAULT_IP, szBuf, nLen, szConfigPath);
	m_strIP = szBuf;
	GetPrivateProfileString("TCP\\IP", "PORT", DEFAULT_PORT, szBuf, nLen, szConfigPath);
	m_nPort = atoi(szBuf);
	return true;
}
