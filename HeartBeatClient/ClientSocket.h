#pragma once
#include <winsock2.h>
#include <stdint.h>
#include <string>

#define SEND_BUFF_LEN	8192
#define INFO_LEN		128


class ClientSocket
{
public:
	ClientSocket();
	virtual ~ClientSocket();

	// ����socket��
	bool LoadSockLib();

	// ж��socket��
	void UnloadSockLib();

	// ��ȡ������Ϣ
	bool ReadConfig();

	//�ͻ��˳�ʼ�������ӷ�����
	bool start();

	// ֹͣ�ͻ��ˣ��ر�socket
	void stop();
	
private:
	static DWORD WINAPI UserDataThread(LPVOID lpParam);

	//ԭʼ�����ݴ��亯��
	bool SendData(uint32_t dwCmdID, const void* pData, uint32_t nLen);
	bool RecvData(void*  pData, uint32_t nLen);
	bool SendRaw(void*  pData, uint32_t nLen);
private:
	std::string						m_strIP;						// �������˵�IP��ַ
	unsigned short					m_nPort;						// �������˵ļ����˿�
	bool							m_bLibLoaded;					// socket���Ƿ񱻼���
	SOCKET							m_hClientSock;					// socket�׽���
	char							m_szSendBuf[SEND_BUFF_LEN];		// ����ƴ���ַ���
	fd_set							m_fdAll;
	struct timeval					m_tv;
	int								m_nHeartbeats;
};

