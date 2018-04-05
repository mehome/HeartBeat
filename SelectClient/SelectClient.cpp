// SelectClient.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "HeartBeat.h"
#include <winsock2.h>
#include "Socket.h"
#include <iostream>
using namespace std;
#pragma comment(lib,"ws2_32.lib")
DWORD WINAPI UserDataThread(LPVOID lpParam);
int main()
{
	WSADATA wsaData;
	int nResult;
	nResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (NO_ERROR != nResult)
	{
		Log(LEVEL_ERROR, "WSAStartup error.");
	}
	SOCKET
	sockClient = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sockClient == INVALID_SOCKET)
	{
		Log(LEVEL_ERROR, "socket error.");
		return false;
	}

	sockaddr_in addr = { 0 };
	addr.sin_family = AF_INET;
	//已注册端口号：1024~49151
	addr.sin_port = htons(8888);
	addr.sin_addr.S_un.S_addr = inet_addr("127.0.0.1");
	if (0 != connect(sockClient, (sockaddr *)&addr, sizeof(addr)))
	{
		Log(LEVEL_ERROR, "connect error.");
		return false;
	}

	fd_set allfd;
	FD_ZERO(&allfd);
	FD_SET(sockClient, &allfd);
	DWORD nThreadID = 0;
	HANDLE hThread = ::CreateThread(0, 0, UserDataThread, (void *)&sockClient, 0, &nThreadID);
	struct timeval tv;
	tv.tv_sec = T1;
	tv.tv_usec = 0;
	int nHeartbeats = 0;
	int rc;
	fd_set readfd;
	for (;; )
	{
		readfd = allfd;
		rc = select(sockClient + 1, &readfd, NULL, NULL, &tv);
		if (rc < 0)
		{
			Log(LEVEL_ERROR, "select failure.");
			break;
		}
		if (rc == 0)		/* timed out */
		{
			if (++nHeartbeats > 3)
			{
				Log(LEVEL_ERROR, "connection dead.");
				// 可以关闭socke
				closesocket(sockClient);
				break;
			}
			cout << "sending heartbeat #" <<  nHeartbeats << endl;
			if (!SendData(sockClient, MSG_HEARTBEAT, NULL, 0))
			{
				Log(LEVEL_ERROR, "send failure.");
			}
			tv.tv_sec = T2;
			continue;
		}
		if (!FD_ISSET(sockClient, &readfd))
		{
			Log(LEVEL_ERROR, "select returned invalid socket.");
			continue;
		}
		msg_t msg;
		bool res = RecvData(sockClient,&msg, sizeof(msg));
		tv.tv_sec = T1;
		nHeartbeats = 0;
		if (!res)
		{
			closesocket(sockClient);
			FD_CLR(sockClient, &allfd);
			break;
		}
		switch (msg.type)
		{
		case MSG_HEARTBEAT:
			cout << "heart beat reply packet" << endl;;
			break;
		case MSG_TYPE1:
		{
			cout << msg.data << endl;
		}
		break;
		}
	}
	DWORD nExitCode = 0;
	WaitForSingleObject(hThread, INFINITE);
	GetExitCodeThread(hThread, &nExitCode);
	CloseHandle(hThread);
	closesocket(sockClient);
	WSACleanup();
    return 0;
}

DWORD WINAPI UserDataThread(LPVOID lpParam)
{
	SOCKET* pSock = (SOCKET*)lpParam;
	while (true)
	{
		string str;
		cin >> str;
		if (!SendData(*pSock, MSG_TYPE1,str.c_str(), str.size()))
			break;

	}
	return 0;
}


