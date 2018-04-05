// HeartBeatServer.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "IOCPHeartBeatServer.h"

int main()
{
	IOCPHeartBeatServer server;
	server.LoadSockLib();
	server.Start();
	getchar();
	server.Stop();
    return 0;
}

