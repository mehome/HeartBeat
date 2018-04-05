// SelectServer.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "HeartBeat.h"
#include <winsock2.h>
#include "Socket.h"
#include <iostream>
#include <map>
#include <cstring>
#include <chrono>
#include <ctime>
using namespace std::chrono;
using namespace std;
#pragma comment(lib,"ws2_32.lib")
//#define CALCULATE_TIME
#ifdef CALCULATE_TIME
time_t GetTime()
{
	return system_clock::to_time_t(system_clock::now());
}
#endif

int main()
{
#ifdef CALCULATE_TIME
	// �洢ÿ��socket��ʱ
	map<SOCKET, time_t> mapSockToTime;
	//
#endif
	WSADATA wsaData;
	int nResult;
	nResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (NO_ERROR != nResult)
	{
		Log(LEVEL_ERROR, "WSAStartup.");
	}
	SOCKET sockListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (INVALID_SOCKET == sockListen)
	{
		Log(LEVEL_ERROR, "socket error.");
	}

	int32_t reuse = 1;
	if (SOCKET_ERROR == setsockopt(sockListen, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse)))
	{
		Log(LEVEL_ERROR, "setsockopt error.");
	}

	// ��������ַ��Ϣ�����ڰ�Socket
	struct sockaddr_in serverAddress;
	// ����ַ��Ϣ
	ZeroMemory((char *)&serverAddress, sizeof(serverAddress));
	serverAddress.sin_family = AF_INET;
	// ������԰��κο��õ�IP��ַ�����߰�һ��ָ����IP��ַ 
	serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);                      
	serverAddress.sin_port = htons(8888);

	// �󶨵�ַ�Ͷ˿�
	if (SOCKET_ERROR == bind(sockListen, (struct sockaddr *) &serverAddress, sizeof(serverAddress)))
	{
		Log(LEVEL_ERROR, "bind error");
		return false;
	}

	// ��ʼ���м���
	if (SOCKET_ERROR == listen(sockListen, FD_SETSIZE))
	{
		Log(LEVEL_ERROR, "listen error");
		return false;
	}

	fd_set	rset, allset;
	int		i, nready, maxi, maxfd, client[FD_SETSIZE];
	maxfd = sockListen;			/* initialize */
	maxi = -1;					/* index into client[] array */
	for (i = 0; i < FD_SETSIZE; i++)
		client[i] = -1;			/* -1 indicates available entry */
	FD_ZERO(&allset);
	FD_SET(sockListen, &allset);
	struct timeval tv;
	tv.tv_sec = T1 + T2;
	tv.tv_usec = 0;
	int32_t missed_heartbeats = 0;
#ifdef CALCULATE_TIME
	mapSockToTime[sockListen] = GetTime();
#endif
	for (;;)
	{
#ifdef CALCULATE_TIME
		time_t time = GetTime();
#endif
		rset = allset;		/* structure assignment */
		nready = select(maxfd + 1, &rset, NULL, NULL, &tv);
		if (nready < 0)
		{
			Log(LEVEL_ERROR, "select error.");
			break;
		}
		// heart beat,
		//  �������ĳһ��socket���㳬ʱ�ģ��Ǹ���select�Լ��Ľ��������㳬ʱ��
		if (nready == 0)
		{
			if (++missed_heartbeats > 3)
			{
				Log(LEVEL_ERROR, "connection dead.");
				// ʵ���Ͽ�������������Ѿ����ӵĿͻ��ˣ�����û�취���ÿ���ͻ�������������
			}
			cout << "missed heartbeat #" << missed_heartbeats << endl;
			tv.tv_sec = T2;

#ifdef CALCULATE_TIME
			cout << "select timeout: " << GetTime() - time << endl;
			// ��ΪnReady=0�����Ը���������������֧
			if (FD_ISSET(sockListen, &rset))
			{
				time_t time = GetTime();
				cout << "listem socket timeout: " << time - mapSockToTime[sockListen] << endl;
				mapSockToTime[sockListen] = time;
			}
			SOCKET sockfd;
			for (i = 0; i <= maxi; i++) {	/* check all clients for data */
				if ((sockfd = client[i]) < 0)
					continue;
				if (FD_ISSET(sockfd, &rset)) {
					time_t time = GetTime();
					cout << "connect socket:" << sockfd << " timeout:" << time - mapSockToTime[sockfd] << endl;
					mapSockToTime[sockfd] = time;
				}
			}
#endif
			continue;
		}
		// accept
		if (FD_ISSET(sockListen, &rset)) {	/* new client connection */
			struct sockaddr_in clientaddr;
			ZeroMemory((char *)&clientaddr, sizeof(clientaddr));
			int32_t clientlen = sizeof(clientaddr);
			SOCKET connectSock;
			// ��Ҫ�����յ�ַ�ı�����С����Ȼ��һ�οͻ��ĵ�ַ��ȡ����
			connectSock = accept(sockListen, (struct sockaddr*)&clientaddr, &clientlen);
			if (INVALID_SOCKET == connectSock)
			{
				if (WSAGetLastError() == WSAEINTR)
				{
					Log(LEVEL_ERROR, "accept error.");
					continue;	// back to for()
				}
				else
					Log(LEVEL_ERROR, "accept error.");
			}
			cout << "new client: " << inet_ntoa(clientaddr.sin_addr) 
				<< ", port " << ntohs(clientaddr.sin_port) << endl;

			for (i = 0; i < FD_SETSIZE; i++)
				if (client[i] < 0) {
					client[i] = connectSock;	/* save descriptor */
					break;
				}
			if (i == FD_SETSIZE)
			{
				Log(LEVEL_INFOR, "too many clients.");
				closesocket(connectSock);
				continue;
			}
#ifdef CALCULATE_TIME
			mapSockToTime[connectSock] = GetTime();
#endif
			FD_SET(connectSock, &allset);	/* add new descriptor to set */
			if (connectSock > maxfd)
				maxfd = connectSock;			/* for select */
			if (i > maxi)
				maxi = i;				/* max index in client[] array */

			if (--nready <= 0)
				continue;				/* no more readable descriptors */
		}
		missed_heartbeats = 0;
		tv.tv_sec = T1 + T2;
		SOCKET sockfd;
		for (i = 0; i <= maxi; i++) {	/* check all clients for data */
			if ((sockfd = client[i]) < 0)
				continue;
			if (FD_ISSET(sockfd, &rset)) {
				msg_t msg;
				if (false == RecvData(sockfd, &msg, sizeof(msg))) {
					/*�ͻ����˽�رգ�������ε���������Ӳ���Ͽ�����֪���Ƿ���Ч */
					closesocket(sockfd);
					FD_CLR(sockfd, &allset);
					client[i] = -1;
				}
				switch (msg.type)
				{
				case MSG_HEARTBEAT:
					Log(LEVEL_INFOR, "receive msg heart beat and return.");
					SendData(sockfd, MSG_HEARTBEAT, NULL, 0);
					break;
				case MSG_TYPE1:
				{
					char szBuf[128] = "hello client ,i receive: ";
					Log(LEVEL_INFOR, "receive %s", msg.data);
					snprintf(szBuf + strlen(szBuf), sizeof(szBuf)- strlen(szBuf),"%s", msg.data);
					SendData(sockfd, MSG_TYPE1, szBuf, strlen(szBuf));
				}
					break;
				case MSG_TYPE2:
					break;
				default:
					Log(LEVEL_INFOR, "error message type.");
				}

				if (--nready <= 0)
					break;				/* no more readable descriptors */
			}
		}
	}
	closesocket(sockListen);
	WSACleanup();
    return 0;
}


