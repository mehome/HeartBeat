#pragma once
#include <winsock2.h>
#include <MSWSock.h>
#include <vector>
#include <string>
#include <cassert>
#include <map>

// ���������� (1024*8)
// ֮����Ϊʲô����8K��Ҳ��һ�������ϵľ���ֵ
// ���ȷʵ�ͻ��˷�����ÿ�����ݶ��Ƚ��٣���ô�����õ�СһЩ��ʡ�ڴ�
#define MAX_BUFFER_LEN        8192  
// Ĭ�ϵ�context ����
#define DEFAULT_CLIENT_CONTEXT_LEN	32
// ������Ϣ����
#define PROGRAM_INFO_LEN	128


//////////////////////////////////////////////////////////////////
// ����ɶ˿���Ͷ�ݵ�I/O����������
typedef enum _OPERATION_TYPE
{
	ACCEPT_POSTED,                     // ��־Ͷ�ݵ�Accept����
	SEND_POSTED,                       // ��־Ͷ�ݵ��Ƿ��Ͳ���
	RECV_POSTED,                       // ��־Ͷ�ݵ��ǽ��ղ���
	NULL_POSTED                        // ���ڳ�ʼ����������
}OPERATION_TYPE;

//====================================================================================
//
//				��IO���ݽṹ�嶨��(����ÿһ���ص������Ĳ���)
//
//====================================================================================

typedef struct _PER_IO_CONTEXT
{
	OVERLAPPED		m_Overlapped;                               // ÿһ���ص�����������ص��ṹ(���ÿһ��Socket��ÿһ����������Ҫ��һ��)              
	SOCKET			m_sockAccept;                               // ������������ʹ�õ�Socket
	WSABUF			m_wsaBuf;                                   // WSA���͵Ļ����������ڸ��ص�������������
	char			m_szBuffer[MAX_BUFFER_LEN];                 // �����WSABUF�������ַ��Ļ�����
	OPERATION_TYPE	m_OpType;                                   // ��ʶ�������������(��Ӧ�����ö��)
	// ��ʼ��
	_PER_IO_CONTEXT()
	{
		ZeroMemory(&m_Overlapped, sizeof(m_Overlapped));
		ZeroMemory(m_szBuffer, MAX_BUFFER_LEN);
		m_sockAccept = INVALID_SOCKET;
		m_wsaBuf.buf = m_szBuffer;
		m_wsaBuf.len = MAX_BUFFER_LEN;
		m_OpType = NULL_POSTED;
	}
	// �ͷŵ�Socket
	~_PER_IO_CONTEXT()
	{
		if (m_sockAccept != INVALID_SOCKET)
		{
			closesocket(m_sockAccept);
			m_sockAccept = INVALID_SOCKET;
		}
	}
	// ���û���������
	void ResetBuffer()
	{
		ZeroMemory(m_szBuffer, MAX_BUFFER_LEN);
		m_wsaBuf.len = MAX_BUFFER_LEN;
	}

} PER_IO_CONTEXT, *PPER_IO_CONTEXT;


//====================================================================================
//
//				��������ݽṹ�嶨��(����ÿһ����ɶ˿ڣ�Ҳ����ÿһ��Socket�Ĳ���)
//
//====================================================================================

typedef struct _PER_SOCKET_CONTEXT
{
	SOCKET      m_Socket;									// ÿһ���ͻ������ӵ�Socket
	SOCKADDR_IN m_ClientAddr;								// �ͻ��˵ĵ�ַ
	std::vector<_PER_IO_CONTEXT*> m_vectorIoContext;		// �ͻ���������������������ݣ�
															// Ҳ����˵����ÿһ���ͻ���Socket���ǿ���������ͬʱͶ�ݶ��IO�����
	int missed_heartbeats;									// ��¼��������ʧ����
	_PER_SOCKET_CONTEXT()
	{
		m_vectorIoContext.reserve(DEFAULT_CLIENT_CONTEXT_LEN);
		m_Socket = INVALID_SOCKET;
		memset(&m_ClientAddr, 0, sizeof(m_ClientAddr));
		missed_heartbeats = 0;
	}

	// �ͷ���Դ
	~_PER_SOCKET_CONTEXT()
	{
		if (m_Socket != INVALID_SOCKET)
		{
			closesocket(m_Socket);
			m_Socket = INVALID_SOCKET;
		}
		// �ͷŵ����е�IO����������
		for (int i = 0; i < m_vectorIoContext.size(); i++)
		{
			delete m_vectorIoContext.at(i);
		}
		m_vectorIoContext.clear();
	}

	// ��ȡһ���µ�IoContext
	_PER_IO_CONTEXT* GetNewIoContext()
	{
		_PER_IO_CONTEXT* p = new _PER_IO_CONTEXT;

		m_vectorIoContext.push_back(p);

		return p;
	}

	// ���������Ƴ�һ��ָ����IoContext
	void RemoveContext(_PER_IO_CONTEXT* pContext)
	{
		assert(pContext != NULL);

		for (int i = 0; i < m_vectorIoContext.size(); i++)
		{
			if (pContext == m_vectorIoContext.at(i))
			{
				delete pContext;
				pContext = NULL;
				std::vector<_PER_IO_CONTEXT*>::iterator it = m_vectorIoContext.begin() + i;
				m_vectorIoContext.erase(it);
				break;
			}
		}
	}

} PER_SOCKET_CONTEXT, *PPER_SOCKET_CONTEXT;

// �������̵߳��̲߳���
class IOCPHeartBeatServer;
typedef struct _tagThreadParams
{
	IOCPHeartBeatServer*	pIOCPServer;										// ��ָ�룬���ڵ������еĺ���
	int         nThreadNo;										// �̱߳��
} THREADPARAMS, *PTHREADPARAMS;

class IOCPHeartBeatServer
{
public:
	IOCPHeartBeatServer();
	virtual ~IOCPHeartBeatServer();

	// ����socket��
	bool LoadSockLib();

	// ж��socket��
	void UnloadSockLib();

	// ����������
	bool Start();

	//	ֹͣ������
	void Stop();

	// ��ñ�����IP��ַ
	std::string GetLocalIP();
private:
	// �̺߳�����ΪIOCP�������Ĺ������߳�
	static DWORD WINAPI _WorkerThread(LPVOID lpParam);

	// ��ʼ��IOCP
	bool _InitializeIOCP();

	// ��ʼ��Socket
	bool _InitializeListenSocket();

	// ����ͷ���Դ
	void _DeInitialize();

	// ���пͻ��������ʱ�򣬽��д���(�½�һsocket),����ȡ����һ������
	bool _DoAccpet(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext, PER_SOCKET_CONTEXT* pNewClientSocketContext, PER_IO_CONTEXT* pNewClientIoContext);

	// Ͷ��Accept����
	bool _PostAccept(PER_IO_CONTEXT* pAcceptIoContext);

	// ������󶨵���ɶ˿���(���磺��accept���socket�󶨵���ɶ˿�)
	bool _AssociateWithIOCP(PER_SOCKET_CONTEXT *pContext);

	// �������յ�������
	bool _DealRecvData(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext);

	// Ͷ�ݽ������ݵ�����
	bool _DoPostRecv(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext);

	// Ͷ�ݽ�����������
	bool _PostRecv(PER_IO_CONTEXT* pIoContext);

	// Ͷ�����ݷ�������
	bool _DoPostSend(PER_SOCKET_CONTEXT* pSocketContext, PER_IO_CONTEXT* pIoContext, uint32_t dwCmdID, LPVOID pDate, uint32_t dataLen);

	// Ͷ�����ݷ�������
	bool _PostSend(PER_IO_CONTEXT* pIoContext,uint32_t dwCmdID, LPVOID pDate, uint32_t dataLen);

	// ���ͻ��˵������Ϣ�洢��������
	void _AddToContextList(PER_SOCKET_CONTEXT *pSocketContext);

	// ���ͻ��˵���Ϣ���������Ƴ�
	void _RemoveContext(PER_SOCKET_CONTEXT *pSocketContext);

	// ��տͻ�����Ϣ
	void _ClearContextList();

	// ������ɶ˿��ϵĴ���
	bool HandleError(PER_SOCKET_CONTEXT *pSockContext, PER_IO_CONTEXT* pIoContext, const DWORD& dwErr);

		// �жϿͻ���Socket�Ƿ��Ѿ��Ͽ�
	bool _IsSocketAlive(SOCKET s);

	// ��ñ����Ĵ���������
	int _GetNoOfProcessors();

public:
	// ��ȡ������Ϣ
	bool ReadConfig();

private:
	int								m_nThreads;						// ���ɵ��߳�����
	HANDLE*							m_phWorkerThreads;				// �������̵߳ľ��ָ��
	HANDLE							m_hIOCompletionPort;			// ��ɶ˿ڵľ��
	HANDLE							m_hShutdownEvent;				// ����֪ͨ�߳�ϵͳ�˳����¼���Ϊ���ܹ����õ��˳��߳�
	bool							m_bLibLoaded;					// socket���Ƿ񱻼���
	CRITICAL_SECTION				m_csClientSockContext;          // ����Worker�߳�ͬ���Ļ�����

	std::string						m_strIP;						// �������˵�IP��ַ
	unsigned short					m_nPort;                       // �������˵ļ����˿�

	std::vector<PER_SOCKET_CONTEXT*> m_vectorClientSockContext;          // �ͻ���Socket��Context��Ϣ        

	PER_SOCKET_CONTEXT*				m_pListenContext;              // ���ڼ�����Socket��Context��Ϣ

	// ����ָ���ڹ��캯���г�ʼΪNULL��Run-time stack #2����
	LPFN_ACCEPTEX					m_lpfnAcceptEx;					// AcceptEx�ĺ���ָ�룬���ڵ��������չ����
	LPFN_GETACCEPTEXSOCKADDRS		m_lpfnGetAcceptExSockAddrs;		// GetAcceptExSockaddrs �ĺ���ָ��
	//
	LPFN_TRANSMITFILE				m_lpfnTransmitFile;				// TransmitFile �ĺ���ָ�롣
};
